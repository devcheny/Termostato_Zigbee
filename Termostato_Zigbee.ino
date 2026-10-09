#include <Arduino.h>
#include <Preferences.h>
#include <Wire.h>

/*
 * RECUERDA SELECCIONAR LA PLACA ANTES DE CARGAR O VERIFICAR
 * Placa: ESP32C6 Dev Module (core esp32 de Espressif 3.x)
 * Zigbee mode:      Zigbee ED (end device)
 * Partition Scheme: Zigbee 4MB with spiffs   (tiene 2 particiones de app -> OTA)
 * USB CDC On Boot:  Enabled  (para ver el Serial por el USB nativo)
 * Erase All Flash Before Sketch Upload: Enabled solo la primera vez
 *
 * Librería necesaria: LovyanGFX (para la pantalla, ver Pantalla.h).
 *
 * Termostato Zigbee para caldera con ESP32-C6 Super Mini, 2 relés, un SHT31 y
 * una pantalla táctil de 3,5" para manejarlo también a mano.
 *   - Relé CALDERA:     enciende la caldera (y con ella el agua caliente sanitaria).
 *   - Relé CALEFACCION: enciende la calefacción.
 *   La calefacción no puede funcionar sin caldera: encender la calefacción
 *   enciende la caldera, y apagar la caldera apaga la calefacción.
 *
 * En ZHA aparece "DIY Cheny Termostato" con:
 *   - climate: calefacción (Apagado / Calor, consigna, temperatura actual).
 *   - switch:  caldera (agua caliente).
 *   - sensor:  temperatura y humedad del SHT31.
 *   - number:  temperatura externa. HA escribe aquí la de otro sensor (p. ej.
 *              un SONOFF SNZB-02D) con una automatización.
 *   - switch:  usar sensor externo para regular (también desde la pantalla).
 *              Si el externo deja de llegar, vuelve solo al SHT31.
 *   - number:  ciclo mínimo de la caldera en minutos (0 para pruebas).
 *   - update:  actualizaciones de firmware por Zigbee (OTA).
 *
 * El control (histéresis) lo hace el propio ESP32: si se cae HA o la red
 * Zigbee, sigue regulando con la última consigna guardada en flash.
 * Ver README.md para el conexionado, OTA y la configuración de ZHA.
 *
 * Botón BOOT pulsado 3 s -> reset de fábrica Zigbee (para volver a emparejar).
 * LED RGB: azul parpadeando = buscando red, naranja = calefacción encendida,
 *          rojo parpadeando = fallo del sensor, apagado = conectado y en reposo.
 */

#ifndef ZIGBEE_MODE_ED
#error "Selecciona Tools -> Zigbee mode -> Zigbee ED (end device)"
#endif

#include "Zigbee.h"
#include "Pantalla.h"
#include "esp_delta_ota_ops.h"

// ---------------- Versión de firmware (OTA) ----------------
// Súbela en cada versión nueva que quieras instalar por OTA. make_ota.py la lee de aquí.
#define FW_VERSION      0x0000000A
#define OTA_HW_VERSION  0x0001
#define OTA_MANUFACTURER 0x131B   // código Zigbee de Espressif
#define OTA_IMAGE_TYPE  0x0C60    // identifica el firmware de este termostato

// OTA delta (esp_delta_ota_ops.cpp): GitHub publica, además de la imagen completa,
// parches desde versiones anteriores, que se descargan en minutos en vez de horas.
// Los parches solo se ofrecen a OTA_HW_VERSION; las imágenes completas, también a
// OTA_HW_VERSION_FULL_ONLY. Si un parche falla, el termostato se presenta con
// OTA_HW_VERSION_FULL_ONLY para recibir la imagen completa.
#define OTA_HW_VERSION_FULL_ONLY 0x0002
#define OTA_DELTA_SINCE 0x00000005   // primera versión que sabe aplicar parches (no cambiar)
#define OTA_STALL_MS    (5UL * 60 * 1000)   // OTA sin datos este tiempo -> reinicia

// Vuelta atrás: tras una OTA, la versión nueva solo se da por buena cuando lleva
// APP_VALID_AFTER_MS conectada a Zigbee. Si se cuelga o reinicia antes, el bootloader
// arranca la versión anterior; si no se conecta en APP_ROLLBACK_AFTER_MS, vuelve atrás.
#define APP_VALID_AFTER_MS    (2UL * 60 * 1000)
#define APP_ROLLBACK_AFTER_MS (15UL * 60 * 1000)

// ---------------- Configuración ----------------
#define MANUFACTURER  "DIY Cheny"
#define MODEL         "Termostato"

#define RELAY_CALDERA_PIN  18   // caldera / agua caliente sanitaria
#define RELAY_CALEF_PIN    19   // calefacción
#define RELAY_ON           HIGH // pon LOW si tus módulos de relé se activan a nivel bajo
#define RELAY_OFF          (RELAY_ON == HIGH ? LOW : HIGH)

#define SHT_SDA_PIN   6
#define SHT_SCL_PIN   7

#define STATUS_LED    RGB_BUILTIN   // WS2812 de la placa (GPIO8)
#define BUTTON_PIN    BOOT_PIN      // GPIO9

#define EP_THERMOSTAT  10
#define EP_ACS         11
#define EP_SENSOR      12
// El endpoint 13 era la "calefacción forzada" (eliminada en la v9)
#define EP_EXT_TEMP    14
#define EP_EXT_SELECT  15
#define EP_MIN_CYCLE   16

#define HYSTERESIS        0.3f      // ºC por debajo / encima de la consigna
#define MIN_CYCLE_DEFAULT 3         // minutos mínimos encendida/apagada (protege la caldera); se cambia desde HA
#define MIN_CYCLE_MAX     30
#define READ_INTERVAL_MS  10000     // lectura del SHT31
#define REPORT_INTERVAL_MS 60000    // envío periódico a ZHA aunque no cambie nada
#define SENSOR_FAIL_MS    60000     // sin lecturas válidas este tiempo -> apaga la calefacción
#define EXT_TIMEOUT_MS    (60UL * 60 * 1000)  // sin temperatura externa este tiempo -> usa el SHT31
#define SAVE_DELAY_MS     5000
#define RESET_HOLD_MS     3000

#define SETPOINT_DEFAULT  2000      // 20,00 ºC (unidades Zigbee: 0,01 ºC)
#define SETPOINT_ABS_MIN  500       // 5 ºC
#define SETPOINT_ABS_MAX  3000      // 30 ºC
// ------------------------------------------------

// ---------------- SHT31 (sin librerías externas) ----------------
uint8_t shtAddr = 0;

uint8_t shtCrc(const uint8_t *d) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < 2; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8; b++) crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc;
}

bool shtCommand(uint8_t addr, uint16_t cmd) {
  Wire.beginTransmission(addr);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

bool shtBegin() {
  const uint8_t addrs[] = { 0x44, 0x45 };
  for (uint8_t a : addrs) {
    if (shtCommand(a, 0x30A2)) {   // soft reset
      shtAddr = a;
      delay(10);
      shtCommand(a, 0x3066);       // calentador interno apagado
      return true;
    }
  }
  return false;
}

bool shtRead(float &t, float &h) {
  if (!shtAddr && !shtBegin()) return false;
  if (!shtCommand(shtAddr, 0x2400)) {   // medida única, alta repetibilidad, sin clock stretching
    shtAddr = 0;
    return false;
  }
  delay(20);
  uint8_t d[6];
  if (Wire.requestFrom(shtAddr, (uint8_t)6) != 6) return false;
  for (uint8_t i = 0; i < 6; i++) d[i] = Wire.read();
  if (shtCrc(d) != d[2] || shtCrc(d + 3) != d[5]) return false;
  t = -45.0f + 175.0f * ((d[0] << 8) | d[1]) / 65535.0f;
  h = 100.0f * ((d[3] << 8) | d[4]) / 65535.0f;
  return true;
}

// La pila Zigbee solo envía informes de atributos creados como reportables. Cambiar
// la marca después de crearlos hace que aborte al informar (probado), así que los
// endpoints que necesitan informar de atributos se construyen atributo a atributo.
static void addAttr(esp_zb_attribute_list_t *list, uint16_t cluster, uint16_t id, uint8_t type, uint8_t access, void *value) {
  if (esp_zb_cluster_add_attr(list, cluster, id, type, access, value) != ESP_OK) log_e("No se pudo añadir el atributo 0x%04x", id);
}

static esp_zb_cluster_list_t *newClusterList() {
  esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();
  esp_zb_cluster_list_add_basic_cluster(list, esp_zb_basic_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  esp_zb_cluster_list_add_identify_cluster(list, esp_zb_identify_cluster_create(NULL), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
  return list;
}

// Endpoint que informa a ZHA de sus atributos. La pila Zigbee aborta si se informa de
// un atributo sin configuración de informes (la crea ZHA al emparejar, pero no si el
// atributo no era reportable entonces), así que se crea también desde el firmware.
class ZigbeeReportingEP : public ZigbeeEP {
public:
  ZigbeeReportingEP(uint8_t endpoint) : ZigbeeEP(endpoint) {}

  bool enableReporting(uint16_t cluster, uint16_t attr, uint16_t minInterval, uint16_t maxInterval, uint16_t delta = 0) {
    esp_zb_zcl_reporting_info_t info = {};
    info.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_SRV;
    info.ep = _endpoint;
    info.cluster_id = cluster;
    info.cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE;
    info.attr_id = attr;
    info.u.send_info.min_interval = minInterval;
    info.u.send_info.max_interval = maxInterval;
    info.u.send_info.def_min_interval = minInterval;
    info.u.send_info.def_max_interval = maxInterval;
    info.u.send_info.delta.u16 = delta;
    info.dst.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    info.manuf_code = ESP_ZB_ZCL_ATTR_NON_MANUFACTURER_SPECIFIC;
    return setClusterReporting(&info);
  }

  bool reportAttr(uint16_t cluster, uint16_t attr) {
    esp_zb_zcl_report_attr_cmd_t cmd = {};
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT;
    cmd.attributeID = attr;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.clusterID = cluster;
    cmd.zcl_basic_cmd.src_endpoint = _endpoint;
    return reportClusterAttribute(&cmd);
  }
};

// ---------------- Endpoint termostato (no existe en la librería) ----------------
// Clúster Thermostat (0x0201) en modo servidor, solo calor. ZHA lo muestra como climate.
class ZigbeeHeatingThermostat : public ZigbeeReportingEP {
public:
  ZigbeeHeatingThermostat(uint8_t endpoint) : ZigbeeReportingEP(endpoint) {
    _device_id = ESP_ZB_HA_THERMOSTAT_DEVICE_ID;

    const uint16_t TH = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT;
    const uint8_t R = ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, RW = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, REP = ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
    int16_t local = (int16_t)0x8000;   // "desconocida" hasta la primera lectura
    int16_t heatSp = SETPOINT_DEFAULT, coolSp = 2600;
    int16_t absMin = SETPOINT_ABS_MIN, absMax = SETPOINT_ABS_MAX;
    uint8_t seq = ESP_ZB_ZCL_THERMOSTAT_CONTROL_SEQ_OF_OPERATION_HEATING_ONLY;
    uint8_t mode = ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
    int8_t calib = 0;
    uint16_t runningState = 0;   // bit 0 = calor: HA muestra "Calentando"
    uint8_t demand = 0;

    esp_zb_attribute_list_t *th = esp_zb_zcl_attr_list_create(TH);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, R | REP, &local);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, RW | REP, &heatSp);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_COOLING_SETPOINT_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, RW, &coolSp);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_CONTROL_SEQUENCE_OF_OPERATION_ID, ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, RW, &seq);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID, ESP_ZB_ZCL_ATTR_TYPE_8BIT_ENUM, RW | REP, &mode);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_ABS_MIN_HEAT_SETPOINT_LIMIT_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, R, &absMin);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_ABS_MAX_HEAT_SETPOINT_LIMIT_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, R, &absMax);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, RW, &absMin);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID, ESP_ZB_ZCL_ATTR_TYPE_S16, RW, &absMax);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_CALIBRATION_ID, ESP_ZB_ZCL_ATTR_TYPE_S8, RW, &calib);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID, ESP_ZB_ZCL_ATTR_TYPE_16BITMAP, R | REP, &runningState);
    addAttr(th, TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_PI_HEATING_DEMAND_ID, ESP_ZB_ZCL_ATTR_TYPE_U8, R | REP, &demand);

    _cluster_list = newClusterList();
    esp_zb_cluster_list_add_thermostat_cluster(_cluster_list, th, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    _ep_config = {
      .endpoint = _endpoint, .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID, .app_device_id = ESP_ZB_HA_THERMOSTAT_DEVICE_ID, .app_device_version = 0
    };
  }

  // Se llama desde la tarea Zigbee: solo anota el cambio, loop() lo procesa
  void onAttributeChange(void (*cb)(uint16_t attr, int32_t value)) { _cb = cb; }

  bool set(uint16_t attr, void *value) {
    return setClusterAttribute(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, value, false) == ESP_ZB_ZCL_STATUS_SUCCESS;
  }

  bool report(uint16_t attr) {
    return reportAttr(ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT, attr);
  }

  // Llamar tras Zigbee.begin(), antes de informar de nada
  void enableReports() {
    const uint16_t TH = ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT;
    enableReporting(TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, 30, 300, 10);   // 0,1 ºC
    enableReporting(TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID, 0, 300, 1);
    enableReporting(TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID, 0, 300);
    enableReporting(TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID, 0, 300);
    enableReporting(TH, ESP_ZB_ZCL_ATTR_THERMOSTAT_PI_HEATING_DEMAND_ID, 0, 300, 1);
  }

private:
  void (*_cb)(uint16_t, int32_t) = nullptr;

  void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) override {
    if (message->info.cluster != ESP_ZB_ZCL_CLUSTER_ID_THERMOSTAT || !_cb || !message->attribute.data.value) return;
    int32_t v;
    switch (message->attribute.data.type) {
      case ESP_ZB_ZCL_ATTR_TYPE_S8:  v = *(int8_t *)message->attribute.data.value; break;
      case ESP_ZB_ZCL_ATTR_TYPE_S16: v = *(int16_t *)message->attribute.data.value; break;
      default:                       v = *(uint8_t *)message->attribute.data.value; break;
    }
    _cb(message->attribute.id, v);
  }
};

ZigbeeHeatingThermostat zbThermostat(EP_THERMOSTAT);
// Interruptores como salidas binarias (BinaryOutput): ZHA usa su descripción como
// nombre del switch, así cada uno aparece ya con su nombre al emparejar.
class ZigbeeNamedSwitch : public ZigbeeReportingEP {
public:
  ZigbeeNamedSwitch(uint8_t endpoint, const char *name) : ZigbeeReportingEP(endpoint) {
    _device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID;
    const uint16_t BO = ESP_ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT;
    bool value = false, outOfService = false;
    uint8_t statusFlags = 0;
    char desc[ZB_MAX_NAME_LENGTH + 2];   // cadena ZCL: longitud + texto
    size_t len = strnlen(name, ZB_MAX_NAME_LENGTH);
    desc[0] = (char)len;
    memcpy(desc + 1, name, len);
    desc[len + 1] = 0;

    esp_zb_attribute_list_t *bo = esp_zb_zcl_attr_list_create(BO);
    addAttr(bo, BO, ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_PRESENT_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL,
            ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING, &value);
    addAttr(bo, BO, ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_OUT_OF_SERVICE_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL, ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, &outOfService);
    addAttr(bo, BO, ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_STATUS_FLAGS_ID, ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, &statusFlags);
    addAttr(bo, BO, ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_DESCRIPTION_ID, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, desc);

    _cluster_list = newClusterList();
    esp_zb_cluster_list_add_binary_output_cluster(_cluster_list, bo, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    _ep_config = {
      .endpoint = _endpoint, .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID, .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID, .app_device_version = 0
    };
  }

  // Se llama desde la tarea Zigbee cuando HA cambia el interruptor
  void onChange(void (*callback)(bool)) { _cb = callback; }

  bool set(bool value) {
    return setClusterAttribute(ESP_ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                               ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_PRESENT_VALUE_ID, &value, false) == ESP_ZB_ZCL_STATUS_SUCCESS;
  }

  // Llamar tras Zigbee.begin(), antes de informar de nada
  void enableReports() {
    enableReporting(ESP_ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT, ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_PRESENT_VALUE_ID, 0, 300);
  }

private:
  void (*_cb)(bool) = nullptr;

  void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) override {
    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BINARY_OUTPUT && message->attribute.id == ESP_ZB_ZCL_ATTR_BINARY_OUTPUT_PRESENT_VALUE_ID &&
        message->attribute.data.value && _cb)
      _cb(*(bool *)message->attribute.data.value);
  }
};

ZigbeeNamedSwitch       zbAcs(EP_ACS, "Caldera");
ZigbeeTempSensor        zbSensor(EP_SENSOR);

// Salidas analógicas (ZHA las muestra como number). ZHA no consigue leer sus atributos
// al emparejar, así que el termostato le envía rango, paso y unidades como informes.
// El nombre (descripción) no se puede enviar así y ZHA tampoco lo lee: hay que ponerlo
// a mano en HA (ver README).
class ZigbeeNumber : public ZigbeeReportingEP {
public:
  ZigbeeNumber(uint8_t endpoint, const char *name, float minValue, float maxValue, float step, uint16_t units)
    : ZigbeeReportingEP(endpoint) {
    _device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID;
    const uint16_t AO = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT;
    const uint8_t R = ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY, RW = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE, REP = ESP_ZB_ZCL_ATTR_ACCESS_REPORTING;
    float value = minValue > 0 ? minValue : 0;
    bool outOfService = false;
    uint8_t statusFlags = 0;
    char desc[ZB_MAX_NAME_LENGTH + 2];   // cadena ZCL: longitud + texto
    size_t len = strnlen(name, ZB_MAX_NAME_LENGTH);
    desc[0] = (char)len;
    memcpy(desc + 1, name, len);
    desc[len + 1] = 0;

    esp_zb_attribute_list_t *ao = esp_zb_zcl_attr_list_create(AO);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, RW | REP, &value);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MIN_PRESENT_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, R | REP, &minValue);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MAX_PRESENT_VALUE_ID, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, R | REP, &maxValue);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_RESOLUTION_ID, ESP_ZB_ZCL_ATTR_TYPE_SINGLE, R | REP, &step);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_DESCRIPTION_ID, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING, R | REP, desc);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_ENGINEERING_UNITS_ID, ESP_ZB_ZCL_ATTR_TYPE_16BIT_ENUM, R | REP, &units);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_OUT_OF_SERVICE_ID, ESP_ZB_ZCL_ATTR_TYPE_BOOL, RW, &outOfService);
    addAttr(ao, AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_STATUS_FLAGS_ID, ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, R, &statusFlags);

    _cluster_list = newClusterList();
    esp_zb_cluster_list_add_analog_output_cluster(_cluster_list, ao, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    _ep_config = {
      .endpoint = _endpoint, .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID, .app_device_id = ESP_ZB_HA_SIMPLE_SENSOR_DEVICE_ID, .app_device_version = 0
    };
  }

  // Se llama desde la tarea Zigbee cuando HA cambia el valor
  void onChange(void (*callback)(float)) { _cb = callback; }

  bool set(float value) {
    return setClusterAttribute(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                               ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, &value, false) == ESP_ZB_ZCL_STATUS_SUCCESS;
  }

  // Llamar tras Zigbee.begin(), antes de informar de nada
  void enableReports() {
    const uint16_t AO = ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT;
    for (uint16_t id : CAPABILITY_ATTRS) enableReporting(AO, id, 0, 3600);
    enableReporting(AO, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID, 0, 900);
  }

  // Envía a ZHA nombre, rango, paso y unidades (al conectarse)
  void reportCapabilities() {
    for (uint16_t id : CAPABILITY_ATTRS) reportAttr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT, id);
  }

private:
  static constexpr uint16_t CAPABILITY_ATTRS[] = {
    ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MIN_PRESENT_VALUE_ID, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_MAX_PRESENT_VALUE_ID,
    ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_RESOLUTION_ID, ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_ENGINEERING_UNITS_ID,
    // La descripción (texto) no: el SDK aborta al informar de cadenas, igual que con
    // valores booleanos (probado).
  };
  void (*_cb)(float) = nullptr;

  void zbAttributeSet(const esp_zb_zcl_set_attr_value_message_t *message) override {
    if (message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_OUTPUT && message->attribute.id == ESP_ZB_ZCL_ATTR_ANALOG_OUTPUT_PRESENT_VALUE_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_SINGLE && message->attribute.data.value && _cb)
      _cb(*(float *)message->attribute.data.value);
  }
};

ZigbeeNumber zbExtTemp(EP_EXT_TEMP, "Temperatura externa", -20, 60, 0.1, 62);              // ºC
ZigbeeNumber zbMinCycle(EP_MIN_CYCLE, "Ciclo mínimo caldera", 0, MIN_CYCLE_MAX, 1, 72);    // minutos
ZigbeeNamedSwitch       zbExtSelect(EP_EXT_SELECT, "Usar sensor externo");

// ---------------- Estado ----------------
struct Config {
  bool    heatMode;     // calefacción en modo Calor (true) o Apagado
  int16_t setpoint;     // 0,01 ºC
  int16_t minLimit, maxLimit;
  int8_t  calibration;  // 0,1 ºC
  bool    acsOn;        // caldera (agua caliente) encendida
  bool    useExternal;  // regular con el sensor externo
  uint8_t minCycle;     // minutos mínimos entre encendido y apagado de la calefacción
};
Config cfg;

float tempC = NAN, humidity = NAN;   // SHT31, temperatura ya calibrada
unsigned long lastGoodRead = 0;
bool sensorOk = false;

float extTemp = NAN;                 // temperatura externa recibida de HA
unsigned long extLastMs = 0;
bool extEver = false;

// Temperatura con la que se regula: la externa si está elegida y llega, si no la del SHT31
float ctrlTemp = NAN;
bool ctrlOk = false;
bool ctrlIsExternal = false;

bool heatDemand = false;             // la calefacción pide calor (histéresis)
unsigned long lastHeatSwitch = 0;
bool heatSwitchedOnce = false;

Preferences prefs;
unsigned long lastChange = 0;
bool pendingSave = false;
bool wasConnected = false;
volatile bool otaRunning = false;

// Cambios recibidos desde Zigbee (se escriben en la tarea Zigbee, se leen en loop)
volatile bool zbChanged = false;
volatile bool zbAcsChanged = false;
volatile bool zbAcsValue = false;
volatile bool zbExtTempChanged = false;
volatile float zbExtTempValue = 0;
volatile bool zbExtSelChanged = false;
volatile bool zbExtSelValue = false;
volatile bool zbMinCycleChanged = false;
volatile float zbMinCycleValue = 0;

// ---------------- Persistencia ----------------
void loadConfig() {
  prefs.begin("termo", true);
  cfg.heatMode    = prefs.getBool("heat", false);
  cfg.setpoint    = prefs.getShort("sp", SETPOINT_DEFAULT);
  cfg.minLimit    = prefs.getShort("min", SETPOINT_ABS_MIN);
  cfg.maxLimit    = prefs.getShort("max", SETPOINT_ABS_MAX);
  cfg.calibration = prefs.getChar("cal", 0);
  cfg.acsOn       = prefs.getBool("acs", false);
  cfg.useExternal = prefs.getBool("ext", false);
  cfg.minCycle    = prefs.getUChar("cyc", MIN_CYCLE_DEFAULT);
  if (cfg.minCycle > MIN_CYCLE_MAX) cfg.minCycle = MIN_CYCLE_DEFAULT;
  prefs.end();
  if (cfg.heatMode) cfg.acsOn = true;   // calefacción sin caldera no es un estado válido
}

void saveConfig() {
  prefs.begin("termo", false);
  prefs.putBool("heat", cfg.heatMode);
  prefs.putShort("sp", cfg.setpoint);
  prefs.putShort("min", cfg.minLimit);
  prefs.putShort("max", cfg.maxLimit);
  prefs.putChar("cal", cfg.calibration);
  prefs.putBool("acs", cfg.acsOn);
  prefs.putBool("ext", cfg.useExternal);
  prefs.putUChar("cyc", cfg.minCycle);
  prefs.end();
  Serial.println("Configuración guardada");
}

void markDirty() {
  lastChange = millis();
  pendingSave = true;
}

// ---------------- Relés ----------------
void applyRelays() {
  bool calef = heatDemand;
  bool caldera = cfg.acsOn || heatDemand;   // heatDemand sin acsOn solo ocurre en modo forzado
  // Al encender: primero la caldera. Al apagar: primero la calefacción.
  if (caldera) digitalWrite(RELAY_CALDERA_PIN, RELAY_ON);
  digitalWrite(RELAY_CALEF_PIN, calef ? RELAY_ON : RELAY_OFF);
  if (!caldera) digitalWrite(RELAY_CALDERA_PIN, RELAY_OFF);
}

// ---------------- Control ----------------
void setHeatDemand(bool on) {
  if (on == heatDemand) return;
  heatDemand = on;
  lastHeatSwitch = millis();
  heatSwitchedOnce = true;
  applyRelays();
  Serial.printf("Calefacción %s (T=%.2f, consigna=%.2f)\n", on ? "ENCENDIDA" : "APAGADA", ctrlTemp, cfg.setpoint / 100.0f);

  uint16_t running = on ? 0x0001 : 0x0000;
  uint8_t demand = on ? 100 : 0;
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID, &running);
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_PI_HEATING_DEMAND_ID, &demand);
  if (Zigbee.connected()) {
    zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID);
    zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_PI_HEATING_DEMAND_ID);
  }
}

void regulate() {
  // Apagado o sensor sin datos: corta al momento (sin esperar el ciclo mínimo)
  if (!cfg.heatMode || !ctrlOk) {
    setHeatDemand(false);
    return;
  }
  float sp = cfg.setpoint / 100.0f;
  bool want = heatDemand;
  if (ctrlTemp <= sp - HYSTERESIS) want = true;
  else if (ctrlTemp >= sp + HYSTERESIS) want = false;

  if (want != heatDemand && heatSwitchedOnce && millis() - lastHeatSwitch < cfg.minCycle * 60000UL) return;
  setHeatDemand(want);
}

// ---------------- Sensor ----------------
void readSensor() {
  float t, h;
  if (shtRead(t, h)) {
    tempC = t + cfg.calibration / 10.0f;
    humidity = h;
    lastGoodRead = millis();
    if (!sensorOk) Serial.println("SHT31 OK");
    sensorOk = true;

    zbSensor.setTemperature(tempC);
    zbSensor.setHumidity(humidity);
  } else if (sensorOk && millis() - lastGoodRead > SENSOR_FAIL_MS) {
    sensorOk = false;
    Serial.println("Fallo del SHT31");
  } else if (!sensorOk) {
    Serial.println("SHT31 no responde");
  }
  updateControlTemp();   // también caduca la temperatura externa si deja de llegar
}

bool extValid() {
  return extEver && millis() - extLastMs < EXT_TIMEOUT_MS;
}

// Elige la temperatura de control y la publica como temperatura del termostato
void updateControlTemp() {
  bool wasExt = ctrlIsExternal, wasOk = ctrlOk;
  if (cfg.useExternal && extValid()) {
    ctrlTemp = extTemp;
    ctrlOk = true;
    ctrlIsExternal = true;
  } else {
    ctrlTemp = tempC;
    ctrlOk = sensorOk;
    ctrlIsExternal = false;
  }
  if (cfg.useExternal && wasExt && !ctrlIsExternal) Serial.println("Temperatura externa sin datos: se usa el SHT31");
  if (wasOk && !ctrlOk) Serial.println("Sin temperatura: calefacción apagada por seguridad");
  if (ctrlOk) {
    int16_t local = (int16_t)lroundf(ctrlTemp * 100);
    zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, &local);
  }
}

void reportAll() {
  if (!Zigbee.connected()) return;
  if (ctrlOk) zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID);
  if (sensorOk) zbSensor.report();
  zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID);
}

// Publica en los atributos Zigbee la configuración actual (al arrancar o tras corregir un valor)
void publishConfig() {
  uint8_t mode = cfg.heatMode ? ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT : ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID, &mode);
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID, &cfg.setpoint);
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID, &cfg.minLimit);
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID, &cfg.maxLimit);
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_CALIBRATION_ID, &cfg.calibration);
  uint16_t running = heatDemand ? 0x0001 : 0x0000;
  zbThermostat.set(ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID, &running);
  // Los interruptores no se informan a mano: el SDK aborta al enviar un informe manual
  // de una salida binaria. La pila los informa sola al cambiar (ver enableReports()).
  zbAcs.set(cfg.acsOn);
  zbExtSelect.set(cfg.useExternal);
}

// ---------------- Callbacks Zigbee (tarea Zigbee: no tocar la pila aquí) ----------------
volatile int32_t zbPending[5];
volatile uint8_t zbPendingMask = 0;
enum { P_MODE, P_SETPOINT, P_MIN, P_MAX, P_CAL };

void onThermostatAttr(uint16_t attr, int32_t value) {
  int idx = -1;
  switch (attr) {
    case ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID:                 idx = P_MODE; break;
    case ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID:   idx = P_SETPOINT; break;
    case ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID:     idx = P_MIN; break;
    case ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID:     idx = P_MAX; break;
    case ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_CALIBRATION_ID: idx = P_CAL; break;
  }
  if (idx < 0) return;
  zbPending[idx] = value;
  zbPendingMask |= (1 << idx);
  zbChanged = true;
}

void onExtTemp(float value) {
  zbExtTempValue = value;
  zbExtTempChanged = true;
}

void onMinCycle(float value) {
  zbMinCycleValue = value;
  zbMinCycleChanged = true;
}

void onExtSelect(bool state) {
  zbExtSelValue = state;
  zbExtSelChanged = true;
}

void onAcsChange(bool state) {
  zbAcsValue = state;
  zbAcsChanged = true;
}

void onOtaState(bool active) {
  otaRunning = active;
  if (active) {
    // Durante la descarga se sigue regulando; no se reinicia hasta tenerla completa
    Serial.println("OTA: descargando firmware nuevo...");
  } else {
    Serial.println("OTA: terminada");
  }
}

void onIdentify(uint16_t time) {
  static bool blink = false;
  blink = !blink;
  rgbLedWrite(STATUS_LED, time && blink ? 40 : 0, time && blink ? 40 : 0, time && blink ? 40 : 0);
}

int16_t clampSetpoint(int32_t v) {
  if (v < cfg.minLimit) v = cfg.minLimit;
  if (v > cfg.maxLimit) v = cfg.maxLimit;
  return (int16_t)v;
}

// Calefacción y caldera van enlazadas: calefacción encendida => caldera encendida
void setHeatMode(bool on) {
  cfg.heatMode = on;
  if (on) cfg.acsOn = true;
}

void setCaldera(bool on) {
  cfg.acsOn = on;
  if (!on) cfg.heatMode = false;
}

// Publica la configuración y avisa a ZHA del modo y la consigna
void publishAndReport() {
  publishConfig();
  if (Zigbee.connected()) {
    if (!zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID) ||
        !zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID))
      Serial.println("Aviso: no se pudo informar a ZHA del modo o la consigna");
  }
}

void processZigbeeChanges() {
  if (zbChanged) {
    zbChanged = false;
    uint8_t mask = zbPendingMask;
    zbPendingMask = 0;
    bool fix = false;

    if (mask & (1 << P_MIN)) cfg.minLimit = constrain(zbPending[P_MIN], SETPOINT_ABS_MIN, SETPOINT_ABS_MAX);
    if (mask & (1 << P_MAX)) cfg.maxLimit = constrain(zbPending[P_MAX], SETPOINT_ABS_MIN, SETPOINT_ABS_MAX);
    if (cfg.minLimit > cfg.maxLimit) { cfg.maxLimit = cfg.minLimit; fix = true; }

    if (mask & (1 << P_MODE)) {
      bool wasAcs = cfg.acsOn;
      setHeatMode(zbPending[P_MODE] != ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF);
      if (cfg.acsOn != wasAcs) fix = true;   // publica que la caldera se ha encendido
      // Solo se admite Apagado / Calor: cualquier otro modo se corrige a Calor
      if (cfg.heatMode && zbPending[P_MODE] != ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT) fix = true;
      Serial.printf("Modo: %s\n", cfg.heatMode ? "CALOR" : "APAGADO");
    }
    if (mask & (1 << P_SETPOINT)) {
      cfg.setpoint = clampSetpoint(zbPending[P_SETPOINT]);
      if (cfg.setpoint != zbPending[P_SETPOINT]) fix = true;
      Serial.printf("Consigna: %.2f ºC\n", cfg.setpoint / 100.0f);
    } else if (mask & ((1 << P_MIN) | (1 << P_MAX))) {
      int16_t sp = clampSetpoint(cfg.setpoint);
      if (sp != cfg.setpoint) { cfg.setpoint = sp; fix = true; }
    }
    if (mask & (1 << P_CAL)) {
      cfg.calibration = constrain(zbPending[P_CAL], -50, 50);   // ±5 ºC
      if (cfg.calibration != zbPending[P_CAL]) fix = true;
      Serial.printf("Calibración: %.1f ºC\n", cfg.calibration / 10.0f);
      readSensor();   // aplica la calibración ya
    }

    if (fix) publishAndReport();
    markDirty();
    applyRelays();
    regulate();
  }

  if (zbAcsChanged) {
    zbAcsChanged = false;
    if (zbAcsValue != cfg.acsOn) {
      bool wasHeat = cfg.heatMode;
      setCaldera(zbAcsValue);
      Serial.printf("Caldera: %s\n", cfg.acsOn ? "ON" : "OFF");
      if (cfg.heatMode != wasHeat) {
        Serial.println("Caldera apagada: calefacción apagada");
        publishAndReport();
      }
      applyRelays();
      regulate();
      markDirty();
    }
  }

  if (zbExtTempChanged) {
    zbExtTempChanged = false;
    float v = zbExtTempValue;
    if (v > -40 && v < 80) {
      extTemp = v;
      extLastMs = millis();
      extEver = true;
      Serial.printf("Temperatura externa: %.2f ºC\n", v);
      updateControlTemp();
      regulate();
    }
  }

  if (zbExtSelChanged) {
    zbExtSelChanged = false;
    if (zbExtSelValue != cfg.useExternal) {
      cfg.useExternal = zbExtSelValue;
      Serial.printf("Sensor para regular: %s\n", cfg.useExternal ? "EXTERNO" : "SHT31");
      updateControlTemp();
      regulate();
      markDirty();
    }
  }

  if (zbMinCycleChanged) {
    zbMinCycleChanged = false;
    long v = lroundf(zbMinCycleValue);
    if (v < 0) v = 0;
    if (v > MIN_CYCLE_MAX) v = MIN_CYCLE_MAX;
    if (v != cfg.minCycle) {
      cfg.minCycle = v;
      Serial.printf("Ciclo mínimo de la caldera: %u min\n", cfg.minCycle);
      regulate();   // con 0, aplica ya el cambio pendiente
      markDirty();
    }
  }
}

// ---------------- Pantalla ----------------
// Cambio hecho desde la pantalla: aplicarlo y avisar a ZHA
void localConfigChanged() {
  publishAndReport();
  applyRelays();
  regulate();
  markDirty();
}

void handleUi() {
  switch (uiPoll()) {
    case UI_SP_DOWN:
      cfg.setpoint = clampSetpoint(cfg.setpoint - SP_STEP);
      localConfigChanged();
      break;
    case UI_SP_UP:
      cfg.setpoint = clampSetpoint(cfg.setpoint + SP_STEP);
      localConfigChanged();
      break;
    case UI_TOGGLE_HEAT:
      setHeatMode(!cfg.heatMode);
      Serial.printf("Pantalla -> modo %s\n", cfg.heatMode ? "CALOR" : "APAGADO");
      localConfigChanged();
      break;
    case UI_TOGGLE_ACS:
      setCaldera(!cfg.acsOn);
      Serial.printf("Pantalla -> caldera %s\n", cfg.acsOn ? "ON" : "OFF");
      localConfigChanged();
      break;
    case UI_TOGGLE_SOURCE:
      cfg.useExternal = !cfg.useExternal;
      Serial.printf("Pantalla -> sensor %s\n", cfg.useExternal ? "EXTERNO" : "SHT31");
      updateControlTemp();
      localConfigChanged();
      break;
    default: break;
  }

  static unsigned long lastDraw = 0;
  if (millis() - lastDraw < 100) return;
  lastDraw = millis();
  UiState s;
  s.temp = ctrlTemp;
  s.hum = humidity;
  s.sensorOk = ctrlOk;
  s.humOk = sensorOk;
  s.extSelected = cfg.useExternal;
  s.extActive = ctrlIsExternal;
  s.heatMode = cfg.heatMode;
  s.heating = heatDemand;
  s.setpoint = cfg.setpoint;
  s.acs = cfg.acsOn;
  s.connected = Zigbee.connected();
  s.ota = otaRunning;
  uiUpdate(s);
}

// ---------------- LED de estado ----------------
void updateLed(bool connected) {
  static unsigned long lastBlink = 0;
  static bool blinkOn = false;
  unsigned long now = millis();
  if (now - lastBlink > 500) {
    lastBlink = now;
    blinkOn = !blinkOn;
  }
  if (!ctrlOk && cfg.heatMode) rgbLedWrite(STATUS_LED, blinkOn ? 40 : 0, 0, 0);
  else if (!connected)            rgbLedWrite(STATUS_LED, 0, 0, blinkOn ? 30 : 0);
  else if (heatDemand)            rgbLedWrite(STATUS_LED, 30, 8, 0);
  else                            rgbLedWrite(STATUS_LED, 0, 0, 0);
}

// ---------------- Botón de reset ----------------
void checkButton() {
  if (digitalRead(BUTTON_PIN) != LOW) return;
  unsigned long t0 = millis();
  while (digitalRead(BUTTON_PIN) == LOW) {
    delay(50);
    if (millis() - t0 > RESET_HOLD_MS) {
      if (otaRunning) {
        Serial.println("OTA en curso, no se puede resetear ahora");
        return;
      }
      Serial.println("Reset de fábrica Zigbee, reiniciando...");
      digitalWrite(RELAY_CALEF_PIN, RELAY_OFF);
      digitalWrite(RELAY_CALDERA_PIN, RELAY_OFF);
      rgbLedWrite(STATUS_LED, 40, 0, 0);
      delay(1000);
      Zigbee.factoryReset();   // borra la red y reinicia
    }
  }
}

// ---------------- Vuelta atrás tras OTA ----------------
// Arduino da por buena la app nueva al arrancar; así la validación la hace checkAppValid()
extern "C" bool verifyRollbackLater() {
  return true;
}

void checkAppValid(bool connected) {
  static bool done = false;
  static unsigned long connectedSince = 0;
  if (done) return;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
    done = true;   // no viene de una OTA (p. ej. cargada por USB) o ya validada
    return;
  }
  unsigned long now = millis();
  if (!connected) connectedSince = 0;
  else if (!connectedSince) connectedSince = now ? now : 1;

  if (connectedSince && now - connectedSince > APP_VALID_AFTER_MS) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.printf("OTA: firmware 0x%08X validado\n", FW_VERSION);
    done = true;
  } else if (now > APP_ROLLBACK_AFTER_MS) {
    Serial.println("OTA: el firmware nuevo no se conecta a Zigbee, volviendo a la versión anterior");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

// ---------------- Main ----------------
void setup() {
  // Relés apagados cuanto antes
  digitalWrite(RELAY_CALDERA_PIN, RELAY_OFF);
  digitalWrite(RELAY_CALEF_PIN, RELAY_OFF);
  pinMode(RELAY_CALDERA_PIN, OUTPUT);
  pinMode(RELAY_CALEF_PIN, OUTPUT);

  Serial.begin(115200);
  rgbLedWrite(STATUS_LED, 0, 0, 0);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.printf("Termostato Zigbee, firmware 0x%08X\n", FW_VERSION);

  Wire.begin(SHT_SDA_PIN, SHT_SCL_PIN);
  if (shtBegin()) Serial.printf("SHT31 en 0x%02X\n", shtAddr);
  else Serial.println("SHT31 no encontrado, revisa el cableado");

  // Restaurar la configuración (tras un corte de luz sigue funcionando sin HA)
  loadConfig();
  applyRelays();   // agua caliente como estaba; la calefacción espera a tener temperatura

  // Pantalla (si no hay calibración del táctil, la ofrece durante unos segundos)
  uiBegin(prefs);

  zbThermostat.setManufacturerAndModel(MANUFACTURER, MODEL);
  zbThermostat.onAttributeChange(onThermostatAttr);
  zbThermostat.onIdentify(onIdentify);
  bool fullOnly = deltaOtaBegin(FW_VERSION);
  if (fullOnly) Serial.println("OTA: un parche delta falló con esta versión, se pedirán imágenes completas");
  zbThermostat.addOTAClient(FW_VERSION, FW_VERSION + 1, fullOnly ? OTA_HW_VERSION_FULL_ONLY : OTA_HW_VERSION,
                            OTA_MANUFACTURER, OTA_IMAGE_TYPE);
  zbThermostat.onOTAStateChange(onOtaState);

  zbAcs.setManufacturerAndModel(MANUFACTURER, MODEL);
  zbAcs.onChange(onAcsChange);


  zbExtTemp.setManufacturerAndModel(MANUFACTURER, MODEL);
  zbExtTemp.onChange(onExtTemp);

  zbMinCycle.setManufacturerAndModel(MANUFACTURER, MODEL);
  zbMinCycle.onChange(onMinCycle);

  zbExtSelect.setManufacturerAndModel(MANUFACTURER, MODEL);
  zbExtSelect.onChange(onExtSelect);

  zbSensor.setManufacturerAndModel(MANUFACTURER, MODEL);
  zbSensor.setMinMaxValue(-20, 60);
  zbSensor.setTolerance(0.2);
  zbSensor.addHumiditySensor(0, 100, 2, 50);

  Zigbee.addEndpoint(&zbThermostat);
  Zigbee.addEndpoint(&zbAcs);
  Zigbee.addEndpoint(&zbSensor);
  Zigbee.addEndpoint(&zbExtTemp);
  Zigbee.addEndpoint(&zbExtSelect);
  Zigbee.addEndpoint(&zbMinCycle);

  // End device con la radio siempre escuchando: responde al instante sin hacer de router
  Zigbee.setRxOnWhenIdle(true);
  if (!Zigbee.begin(ZIGBEE_END_DEVICE)) {
    Serial.println("Zigbee no ha arrancado, reiniciando...");
    delay(1000);
    ESP.restart();
  }
  Serial.println("Zigbee iniciado, buscando red...");

  zbThermostat.enableReports();
  zbAcs.enableReports();
  zbExtSelect.enableReports();
  zbExtTemp.enableReports();
  zbMinCycle.enableReports();
  zbMinCycle.set(cfg.minCycle);   // valor guardado, para que HA lo vea
  publishConfig();
  zbSensor.setReporting(30, 300, 0.2);
  zbSensor.setHumidityReporting(30, 300, 2);

  readSensor();
  updateControlTemp();
  regulate();
}

void loop() {
  unsigned long now = millis();
  bool connected = Zigbee.connected();

  if (connected && !wasConnected) {
    // Recién conectado: informar a ZHA del estado real y buscar actualizaciones
    Serial.printf("Conectado a la red Zigbee (firmware 0x%08X)\n", FW_VERSION);
    publishConfig();
    reportAll();
    zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID);
    zbThermostat.report(ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID);
    zbExtTemp.reportCapabilities();
    zbMinCycle.reportCapabilities();
    zbThermostat.requestOTAUpdate();   // primera consulta en ~1 min, luego cada hora
  }
  wasConnected = connected;

  processZigbeeChanges();
  checkAppValid(connected);

  // OTA fallida o parada: la librería Zigbee no se recupera sin reiniciar
  if (deltaOtaNeedsRestart(OTA_STALL_MS)) {
    Serial.println("OTA fallida o sin datos, reiniciando...");
    saveConfig();
    delay(1000);
    ESP.restart();
  }

  static unsigned long lastRead = 0;
  if (now - lastRead > READ_INTERVAL_MS) {
    lastRead = now;
    readSensor();
    regulate();
  }

  static unsigned long lastReport = 0;
  if (now - lastReport > REPORT_INTERVAL_MS) {
    lastReport = now;
    reportAll();
  }

  if (pendingSave && now - lastChange > SAVE_DELAY_MS) {
    pendingSave = false;
    saveConfig();
  }

  handleUi();
  updateLed(connected);
  checkButton();
  delay(10);
}
