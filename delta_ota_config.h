#pragma once
/*
 * Configuración de detools (lib/detools) para aplicar parches delta en el ESP32.
 * Tiene que ser la misma en todos los archivos que incluyen detools.h, porque
 * cambia el tamaño de sus estructuras.
 *
 * Solo hace falta aplicar parches secuenciales comprimidos con heatshrink.
 * GitHub Actions los crea con la misma configuración (ver make_ota.py).
 */
#define DETOOLS_CONFIG_FILE_IO                0
#define DETOOLS_CONFIG_COMPRESSION_NONE       1
#define DETOOLS_CONFIG_COMPRESSION_LZMA       0
#define DETOOLS_CONFIG_COMPRESSION_CRLE       0
#define DETOOLS_CONFIG_COMPRESSION_HEATSHRINK 1

// Ventana de heatshrink según indique el parche (en make_ota.py: 2^11 / 2^4)
#define HEATSHRINK_DYNAMIC_ALLOC              1

#ifdef __cplusplus
extern "C" {
#endif
#include "lib/detools/detools.h"
#ifdef __cplusplus
}
#endif
