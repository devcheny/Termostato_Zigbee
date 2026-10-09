// Ver esp_delta_ota_ops.h
#include "esp_delta_ota_ops.h"
#include <Arduino.h>
#include <Preferences.h>
#include "delta_ota_config.h"

static const uint8_t DELTA_MAGIC[4] = { 'Z', 'D', 'L', 'T' };
static const size_t DELTA_HEADER_LEN = 8;

enum Mode { MODE_IDLE, MODE_DETECT, MODE_FULL, MODE_DELTA, MODE_ERROR };

static volatile Mode mode = MODE_IDLE;
static esp_ota_handle_t otaHandle = 0;
static const esp_partition_t *fromPart = nullptr;
static size_t fromOffset = 0;
static detools_apply_patch_t patch;
static uint8_t header[DELTA_HEADER_LEN];
static size_t headerLen = 0;
static size_t written = 0;
static volatile uint32_t lastActivity = 0;
static volatile bool failed = false;
static uint32_t runningVersion = 0;

// ---------------- Callbacks de detools ----------------
static int fromRead(void *, uint8_t *buf, size_t size) {
  if (fromOffset + size > fromPart->size) return -DETOOLS_IO_FAILED;
  if (esp_partition_read(fromPart, fromOffset, buf, size) != ESP_OK) return -DETOOLS_IO_FAILED;
  fromOffset += size;
  return 0;
}

static int fromSeek(void *, int offset) {
  int64_t pos = (int64_t)fromOffset + offset;
  if (pos < 0 || pos > (int64_t)fromPart->size) return -DETOOLS_IO_FAILED;
  fromOffset = (size_t)pos;
  return 0;
}

static int toWrite(void *, const uint8_t *buf, size_t size) {
  if (esp_ota_write(otaHandle, buf, size) != ESP_OK) return -DETOOLS_IO_FAILED;
  written += size;
  return 0;
}

// ---------------- Errores ----------------
// Un parche que no encaja con la app instalada (p. ej. cargada por USB desde otra
// compilación) fallaría siempre. Se apunta para pedir solo imágenes completas.
static void rememberDeltaFailure() {
  Preferences p;
  p.begin("deltaota", false);
  p.putUInt("failver", runningVersion);
  p.end();
}

static esp_err_t fail(const char *what, int detoolsErr = 0) {
  if (detoolsErr) log_e("OTA delta: %s: %s (%d)", what, detools_error_as_string(-detoolsErr), detoolsErr);
  else log_e("OTA: %s", what);
  if (mode == MODE_DELTA || mode == MODE_DETECT) rememberDeltaFailure();
  if (mode != MODE_IDLE && mode != MODE_ERROR) esp_ota_abort(otaHandle);
  mode = MODE_ERROR;
  failed = true;
  return ESP_FAIL;
}

// ---------------- API para ZigbeeHandlers.cpp ----------------
esp_err_t esp_delta_ota_begin(const esp_partition_t *partition, size_t, esp_ota_handle_t *out_handle) {
  fromPart = esp_ota_get_running_partition();
  fromOffset = 0;
  headerLen = 0;
  written = 0;
  lastActivity = millis();
  // Borrado por sectores a medida que se escribe: no bloquea la pila Zigbee varios segundos
  esp_err_t ret = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &otaHandle);
  if (ret != ESP_OK) {
    mode = MODE_ERROR;
    failed = true;
    return ret;
  }
  mode = MODE_DETECT;
  *out_handle = otaHandle;
  return ESP_OK;
}

esp_err_t esp_delta_ota_write(esp_ota_handle_t, const void *data, size_t size) {
  const uint8_t *p = (const uint8_t *)data;
  lastActivity = millis();
  if (mode == MODE_ERROR || mode == MODE_IDLE) return ESP_FAIL;

  if (mode == MODE_DETECT) {
    if (headerLen == 0 && size > 0 && p[0] == 0xE9) {
      mode = MODE_FULL;
      log_i("OTA: imagen completa");
    } else {
      while (headerLen < DELTA_HEADER_LEN && size > 0) {
        header[headerLen++] = *p++;
        size--;
      }
      if (headerLen < DELTA_HEADER_LEN) return ESP_OK;
      if (memcmp(header, DELTA_MAGIC, sizeof(DELTA_MAGIC)) != 0) return fail("formato de imagen desconocido");
      uint32_t patchSize = header[4] | (header[5] << 8) | (header[6] << 16) | ((uint32_t)header[7] << 24);
      int res = detools_apply_patch_init(&patch, fromRead, fromSeek, patchSize, toWrite, nullptr);
      mode = MODE_DELTA;
      if (res < 0) return fail("no se puede iniciar el parche", res);
      log_i("OTA: parche delta de %u bytes sobre la partición %s", patchSize, fromPart->label);
    }
  }
  if (size == 0) return ESP_OK;

  if (mode == MODE_FULL) {
    if (esp_ota_write(otaHandle, p, size) != ESP_OK) return fail("error al escribir la imagen");
    written += size;
    return ESP_OK;
  }
  int res = detools_apply_patch_process(&patch, p, size);
  if (res < 0) return fail("error al aplicar el parche", res);
  return ESP_OK;
}

esp_err_t esp_delta_ota_end(esp_ota_handle_t) {
  if (mode == MODE_DELTA) {
    int res = detools_apply_patch_finalize(&patch);
    if (res < 0) return fail("parche incompleto", res);
    log_i("OTA delta: reconstruidos %u bytes", (unsigned)written);
  } else if (mode != MODE_FULL) {
    return fail("OTA terminada sin datos válidos");
  }
  Mode m = mode;
  mode = MODE_IDLE;
  esp_err_t ret = esp_ota_end(otaHandle);   // comprueba checksum y SHA-256 de la app nueva
  if (ret != ESP_OK) {
    log_e("OTA: la imagen nueva no es válida (%s)", esp_err_to_name(ret));
    if (m == MODE_DELTA) rememberDeltaFailure();
    mode = MODE_ERROR;
    failed = true;
  }
  return ret;
}

// ---------------- API para el sketch ----------------
bool deltaOtaBegin(uint32_t fwVersion) {
  runningVersion = fwVersion;
  Preferences p;
  p.begin("deltaota", false);
  uint32_t failVer = p.getUInt("failver", 0);
  if (failVer && failVer != fwVersion) {
    p.remove("failver");   // ya se actualizó a otra versión: vuelve a aceptar parches
    failVer = 0;
  }
  p.end();
  return failVer == fwVersion;
}

bool deltaOtaNeedsRestart(uint32_t stallMs) {
  if (failed) return true;
  Mode m = mode;
  bool active = m == MODE_DETECT || m == MODE_FULL || m == MODE_DELTA;
  return active && millis() - lastActivity > stallMs;
}
