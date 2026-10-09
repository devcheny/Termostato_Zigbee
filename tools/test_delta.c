/*
 * Comprueba un parche delta antes de publicarlo (lo ejecuta GitHub Actions).
 *
 * Aplica el parche con la misma lib/detools y la misma configuración que el ESP32
 * (delta_ota_config.h), en trozos del tamaño que envía ZHA, y compara el resultado
 * con la imagen completa nueva.
 *
 *   gcc -O2 -I. tools/test_delta.c detools_build.c heatshrink_build.c -o test_delta
 *   ./test_delta anterior_completa.ota parche.ota nueva_completa.ota
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../delta_ota_config.h"

#define CHUNK 50   // ZHA envía bloques de 50 bytes como máximo

struct buf {
    uint8_t *data;
    size_t size;
};

static struct buf from, to, expected;
static size_t from_pos;

static struct buf read_file(const char *path) {
    struct buf b = { 0 };
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    b.size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = malloc(b.size);
    if (fread(b.data, 1, b.size, f) != b.size) { perror(path); exit(1); }
    fclose(f);
    return b;
}

static uint32_t u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

// Devuelve el contenido del elemento "Upgrade Image" de un .ota
static struct buf ota_element(const char *path) {
    struct buf f = read_file(path);
    if (f.size < 66 || u32(f.data) != 0x0BEEF11E) { fprintf(stderr, "%s: no es un .ota\n", path); exit(1); }
    size_t hdr = u16(f.data + 6);
    struct buf e = { f.data + hdr + 6, u32(f.data + hdr + 2) };
    if (u16(f.data + hdr) != 0 || hdr + 6 + e.size != f.size) { fprintf(stderr, "%s: elemento no válido\n", path); exit(1); }
    return e;
}

static int from_read(void *arg, uint8_t *b, size_t size) {
    (void)arg;
    if (from_pos + size > from.size) return -DETOOLS_IO_FAILED;
    memcpy(b, from.data + from_pos, size);
    from_pos += size;
    return 0;
}

static int from_seek(void *arg, int offset) {
    (void)arg;
    long pos = (long)from_pos + offset;
    if (pos < 0 || pos > (long)from.size) return -DETOOLS_IO_FAILED;
    from_pos = pos;
    return 0;
}

static int to_write(void *arg, const uint8_t *b, size_t size) {
    (void)arg;
    to.data = realloc(to.data, to.size + size);
    memcpy(to.data + to.size, b, size);
    to.size += size;
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "uso: %s anterior_completa.ota parche.ota nueva_completa.ota\n", argv[0]);
        return 2;
    }
    from = ota_element(argv[1]);
    struct buf delta = ota_element(argv[2]);
    expected = ota_element(argv[3]);
    if (from.data[0] != 0xE9 || expected.data[0] != 0xE9) { fprintf(stderr, "faltan imágenes completas\n"); return 1; }
    if (delta.size < 8 || memcmp(delta.data, "ZDLT", 4) != 0) { fprintf(stderr, "el parche no empieza por ZDLT\n"); return 1; }

    struct detools_apply_patch_t patch;
    int res = detools_apply_patch_init(&patch, from_read, from_seek, u32(delta.data + 4), to_write, NULL);
    if (res < 0) { fprintf(stderr, "init: %s\n", detools_error_as_string(res)); return 1; }

    // Como en el ESP32: el primer bloque pierde 6 bytes con la cabecera del elemento
    size_t pos = 8;
    size_t n = CHUNK - 6 - 8;
    while (pos < delta.size) {
        if (n > delta.size - pos) n = delta.size - pos;
        res = detools_apply_patch_process(&patch, delta.data + pos, n);
        if (res < 0) { fprintf(stderr, "process: %s\n", detools_error_as_string(res)); return 1; }
        pos += n;
        n = CHUNK;
    }
    res = detools_apply_patch_finalize(&patch);
    if (res < 0) { fprintf(stderr, "finalize: %s\n", detools_error_as_string(res)); return 1; }

    if (to.size != expected.size || memcmp(to.data, expected.data, to.size) != 0) {
        fprintf(stderr, "el resultado (%zu bytes) no coincide con la imagen nueva (%zu bytes)\n", to.size, expected.size);
        return 1;
    }
    printf("OK: %s reconstruye %zu bytes\n", argv[2], to.size);
    return 0;
}
