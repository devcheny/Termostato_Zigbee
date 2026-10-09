#pragma once
/*
 * OTA delta para la librería Zigbee de arduino-esp32.
 *
 * Con -DCONFIG_ZB_DELTA_OTA=1 (build_opt.h), ZigbeeHandlers.cpp incluye este archivo
 * (está en la ruta de inclusión porque es la carpeta del sketch) y escribe la imagen
 * recibida con estas funciones en vez de con esp_ota_*().
 *
 * Acepta dos formatos dentro del elemento "Upgrade Image" del .ota:
 *   - Imagen completa: empieza por 0xE9 (cabecera de app ESP32). Se escribe tal cual.
 *   - Parche delta: "ZDLT" + tamaño del parche (uint32 LE) + parche de detools
 *     (secuencial, heatshrink). Se aplica sobre la app que está funcionando.
 * En los dos casos esp_ota_end() comprueba la imagen resultante (checksum y SHA-256)
 * antes de arrancar con ella.
 */

#include <stdint.h>
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

// Llamadas desde ZigbeeHandlers.cpp
esp_err_t esp_delta_ota_begin(const esp_partition_t *partition, size_t image_size, esp_ota_handle_t *out_handle);
esp_err_t esp_delta_ota_write(esp_ota_handle_t handle, const void *data, size_t size);
esp_err_t esp_delta_ota_end(esp_ota_handle_t handle);

// Para el sketch
// Llamar al arrancar. Devuelve true si un parche delta falló con esta versión de
// firmware: entonces hay que pedir solo imágenes completas (ver OTA_HW_VERSION_FULL_ONLY).
bool deltaOtaBegin(uint32_t fwVersion);
// true si una OTA ha fallado o lleva más de stallMs sin recibir datos. La librería
// Zigbee no se recupera bien de una OTA a medias, así que el sketch reinicia el ESP32.
bool deltaOtaNeedsRestart(uint32_t stallMs);
