// Simulador de la lógica de relés. Lo compila y ejecuta tools/test_caldera.py, que genera
// los .inc con el código real del firmware. Aquí solo van las imitaciones del hardware
// (reloj, relés, Zigbee, pantalla) y las pruebas.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <random>

// ---------------- Imitación de Arduino ----------------
#define HIGH 1
#define LOW 0
#define constrain(v, a, b) ((v) < (a) ? (a) : ((v) > (b) ? (b) : (v)))

static uint32_t nowMs = 0;
uint32_t millis() { return nowMs; }

struct FakeSerial {
  template <typename... A> void printf(const char *, A...) {}
  template <typename... A> void println(A...) {}
} Serial;

// Tipos, constantes y variables reales del firmware
#include "firmware_tipos.inc"

// ---------------- Imitación de Zigbee ----------------
enum {
  ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF = 0, ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT = 4,
  ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID = 100, ESP_ZB_ZCL_ATTR_THERMOSTAT_PI_HEATING_DEMAND_ID,
  ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_ID, ESP_ZB_ZCL_ATTR_THERMOSTAT_SYSTEM_MODE_ID,
  ESP_ZB_ZCL_ATTR_THERMOSTAT_OCCUPIED_HEATING_SETPOINT_ID, ESP_ZB_ZCL_ATTR_THERMOSTAT_MIN_HEAT_SETPOINT_LIMIT_ID,
  ESP_ZB_ZCL_ATTR_THERMOSTAT_MAX_HEAT_SETPOINT_LIMIT_ID, ESP_ZB_ZCL_ATTR_THERMOSTAT_LOCAL_TEMPERATURE_CALIBRATION_ID,
};

static uint16_t haRunning = 0;   // lo último publicado como "calentando"
static bool haAcs = false;       // lo último publicado en el interruptor Caldera

struct FakeThermostat {
  void set(uint16_t attr, void *v) {
    if (attr == ESP_ZB_ZCL_ATTR_THERMOSTAT_THERMOSTAT_RUNNING_STATE_ID) haRunning = *(uint16_t *)v;
  }
  bool report(uint16_t) { return true; }
} zbThermostat;
struct FakeSwitch {
  bool *out;
  void set(bool v) { if (out) *out = v; }
} zbAcs{&haAcs}, zbExtSelect{nullptr};
struct FakeNumber { void set(float) {} } zbMinCycle, zbHystOn, zbHystOff;
struct FakeZigbee { bool connected() { return true; } } Zigbee;

// ---------------- Comprobaciones ----------------
static int failures = 0, checks = 0;
static const char *currentTest = "";

#define CHECK(cond, ...)                                             \
  do {                                                               \
    checks++;                                                        \
    if (!(cond)) {                                                   \
      if (failures++ < 30) {                                         \
        std::printf("  FALLO [%s] linea %d: ", currentTest, __LINE__); \
        std::printf(__VA_ARGS__);                                    \
        std::printf("\n");                                           \
      }                                                              \
    }                                                                \
  } while (0)

// ---------------- Relés: cada escritura se comprueba ----------------
static int relay[32];
static uint32_t boilerChangedAt = 0;   // último cambio real del relé de la caldera
static uint32_t boilerOffAt = 0, calefOffAt = 0;
static int writes = 0, boilerOffWrite = 0, calefOffWrite = 0;
static bool calefWasOn = false;        // la calefacción se ha cortado desde el arranque
static uint32_t intervalMs = 0;        // separación mínima exigida entre cambios (0 = sin comprobar)

void digitalWrite(int pin, int v) {
  writes++;
  if (relay[pin] != v) {
    if (pin == RELAY_CALDERA_PIN) {
      CHECK(nowMs - boilerChangedAt >= intervalMs, "la caldera cambia a los %u ms (ciclo %u ms)", (unsigned)(nowMs - boilerChangedAt), (unsigned)intervalMs);
      boilerChangedAt = nowMs;
      if (v == LOW) {
        boilerOffAt = nowMs;
        boilerOffWrite = writes;
        // La electroválvula debe tener el ciclo entero para cerrar
        if (calefWasOn) CHECK(nowMs - calefOffAt >= intervalMs, "caldera apagada a los %u ms de cortar la calefacción", (unsigned)(nowMs - calefOffAt));
      }
    } else if (v == LOW) {
      calefOffAt = nowMs;
      calefOffWrite = writes;
      calefWasOn = true;
    }
  }
  relay[pin] = v;
  CHECK(!(relay[RELAY_CALEF_PIN] == HIGH && relay[RELAY_CALDERA_PIN] == LOW), "calefacción encendida con la caldera apagada");
}

// ---------------- Imitación del sensor y la pantalla ----------------
static float simTemp = 18;
static bool simSensorOk = true;
void updateControlTemp();
void readSensor() {
  tempC = simTemp;
  sensorOk = simSensorOk;
  updateControlTemp();
}

static std::deque<UiAction> uiQueue;
static UiState lastUi;
UiAction uiPoll() {
  if (uiQueue.empty()) return UI_NONE;
  UiAction a = uiQueue.front();
  uiQueue.pop_front();
  return a;
}
void uiUpdate(const UiState &s) { lastUi = s; }

// Variables que el firmware declara junto a otras que no se usan aquí
uint32_t lastChange = 0;
bool pendingSave = false;
bool zbReady = true;
volatile bool otaRunning = false;

// Funciones reales del firmware
#include "firmware_extraido.inc"

// ---------------- Simulación de loop() y setup() ----------------
static uint32_t lastRead = 0;

// Las vueltas de loop() no duran siempre lo mismo: redibujar la pantalla tarda ~145 ms
static void tick(uint32_t stepMs = 10) {
  nowMs += stepMs;
  processZigbeeChanges();
  if (nowMs - lastRead > READ_INTERVAL_MS) {
    lastRead = nowMs;
    readSensor();
    regulate();
  }
  if (boilerWaiting && boilerWaitSecs() == 0) applyRelays();
  handleUi();
}

static void run(uint32_t ms) {
  for (uint32_t i = 0; i < ms / 10; ++i) tick();
}

// Como tras un arranque: relés apagados y configuración leída de la flash
static void boot(uint16_t cycleTenths, bool heat, bool acs, float temp, uint32_t startMs = 5000) {
  nowMs = startMs;
  lastRead = nowMs;
  relay[RELAY_CALDERA_PIN] = relay[RELAY_CALEF_PIN] = LOW;
  boilerChangedAt = nowMs;
  calefWasOn = false;
  intervalMs = cycleTenths * 6000U;
  boilerOn = calefOn = heatDemand = boilerWaiting = false;
  zbChanged = zbAcsChanged = zbExtTempChanged = zbExtSelChanged = zbMinCycleChanged = false;
  zbHystOnChanged = zbHystOffChanged = false;
  zbPendingMask = 0;
  extEver = false;
  ctrlOk = ctrlIsExternal = false;
  haRunning = 0;
  uiQueue.clear();
  cfg = Config{};
  cfg.heatMode = heat;
  cfg.acsOn = acs || heat;   // como loadConfig: calor sin caldera no es un estado válido
  cfg.setpoint = 2000;
  cfg.minLimit = SETPOINT_ABS_MIN;
  cfg.maxLimit = SETPOINT_ABS_MAX;
  cfg.minCycle = cycleTenths;
  cfg.hystOn = cfg.hystOff = HYST_DEFAULT;
  simTemp = temp;
  simSensorOk = true;
  // Lo mismo que setup()
  boilerSwitchedOnce = true;
  lastBoilerSwitch = millis();
  applyRelays();
  readSensor();
  updateControlTemp();
  regulate();
}

static void press(UiAction a) { uiQueue.push_back(a); tick(); }
static void haCaldera(bool on) { onAcsChange(on); tick(); }
static void haMode(bool heat) {
  zbPending[P_MODE] = heat ? ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_HEAT : ESP_ZB_ZCL_THERMOSTAT_SYSTEM_MODE_OFF;
  zbPendingMask |= 1 << P_MODE;
  zbChanged = true;
  tick();
}
static void haCycle(float minutes) { zbMinCycleValue = minutes; zbMinCycleChanged = true; tick(); }

static const uint16_t CYCLE = 30;              // 3 minutos, en décimas
static const uint32_t CYCLE_MS = 180000;
static const uint32_t ONE_HOUR = 3600000;

// Calefacción en marcha con la caldera encendida desde hace on_ms
static void heatingFor(uint32_t on_ms, uint32_t startMs = 5000) {
  boot(CYCLE, true, true, 18, startMs);
  run(CYCLE_MS + 20);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH && relay[RELAY_CALEF_PIN] == HIGH, "debería estar calentando");
  run(on_ms);
}

// La calefacción se corta ya y la caldera sigue exactamente el ciclo entero
static void expectFullRunOn(uint32_t t0) {
  CHECK(relay[RELAY_CALEF_PIN] == LOW, "la calefacción debe cortarse al instante");
  CHECK(calefOffAt == nowMs, "la calefacción se cortó a destiempo");
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "la caldera debe seguir encendida");
  CHECK(boilerWaiting, "debe constar que la caldera está esperando");
  run(CYCLE_MS - 1000);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "la caldera se apagó antes de tiempo, a los %u ms", (unsigned)(nowMs - t0));
  run(1020);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "la caldera sigue encendida tras el ciclo");
  CHECK(!boilerWaiting, "no debe quedar nada esperando");
}

// ---------------- Pruebas ----------------
static void t_screen_after_hour() {
  heatingFor(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  expectFullRunOn(nowMs);
}

static void t_ha_after_hour() {
  heatingFor(ONE_HOUR);
  haCaldera(false);
  expectFullRunOn(nowMs);
}

static void t_recent_boiler_gets_full_cycle() {
  heatingFor(60000);   // ya pasó el ciclo de arranque: lleva encendida 1 min
  press(UI_TOGGLE_ACS);
  expectFullRunOn(nowMs);
}

static void t_published_at_once() {
  heatingFor(ONE_HOUR);
  CHECK(haRunning == 1, "HA debe ver que calienta");
  press(UI_TOGGLE_ACS);
  CHECK(haRunning == 0, "HA debe dejar de ver que calienta en cuanto se corta");
  CHECK(!haAcs, "el interruptor Caldera debe publicarse apagado");
  CHECK(!cfg.acsOn && !cfg.heatMode, "caldera y modo calor deben quedar apagados");
  CHECK(pendingSave, "el cambio debe guardarse en flash");
}

static void t_countdown() {
  heatingFor(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  run(200);
  CHECK(lastUi.boilerOn && lastUi.waitSecs >= 179 && lastUi.waitSecs <= 180, "cuenta atrás: %u s, caldera %d", (unsigned)lastUi.waitSecs, lastUi.boilerOn);
  CHECK(!lastUi.heating, "la pantalla no debe mostrar calefacción");
  run(60000);
  CHECK(lastUi.waitSecs >= 119 && lastUi.waitSecs <= 120, "cuenta atrás tras 1 min: %u s", (unsigned)lastUi.waitSecs);
  run(CYCLE_MS - 60000);
  CHECK(!lastUi.boilerOn && lastUi.waitSecs == 0, "al acabar: %u s, caldera %d", (unsigned)lastUi.waitSecs, lastUi.boilerOn);
}

static void t_zero_cycle() {
  boot(0, true, true, 18);
  run(1000);
  CHECK(relay[RELAY_CALEF_PIN] == HIGH, "debería calentar");
  run(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  CHECK(relay[RELAY_CALEF_PIN] == LOW && relay[RELAY_CALDERA_PIN] == LOW, "con ciclo 0 se apagan las dos ya");
  CHECK(calefOffWrite < boilerOffWrite, "primero la calefacción y luego la caldera");
  CHECK(calefOffAt == boilerOffAt && calefOffAt == nowMs, "con ciclo 0 no hay espera");
}

static void t_no_heating_after_hour() {
  // Modo calor, pero la temperatura ya está alcanzada: la calefacción no está en marcha
  boot(CYCLE, true, true, 25);
  run(CYCLE_MS + ONE_HOUR);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH && relay[RELAY_CALEF_PIN] == LOW, "solo la caldera encendida");
  press(UI_TOGGLE_ACS);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "sin calefacción en marcha no hay ciclo extra");
}

static void t_no_heating_recent_only_remaining() {
  boot(CYCLE, false, true, 25);
  run(CYCLE_MS + 60000);
  uint32_t on = boilerChangedAt;
  press(UI_TOGGLE_ACS);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "aún no ha cumplido el ciclo");
  run(CYCLE_MS);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "debe apagarse");
  CHECK(boilerOffAt - on >= CYCLE_MS && boilerOffAt - on < CYCLE_MS + 50, "solo espera lo que falta: %u ms", (unsigned)(boilerOffAt - on));
}

static void t_caldera_back_on_cancels() {
  heatingFor(ONE_HOUR);
  uint32_t on = boilerChangedAt;
  press(UI_TOGGLE_ACS);
  run(60000);
  press(UI_TOGGLE_ACS);
  CHECK(!boilerWaiting, "volver a encender anula la espera");
  run(CYCLE_MS * 3);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH && boilerChangedAt == on, "la caldera no debe haberse apagado");
  CHECK(relay[RELAY_CALEF_PIN] == LOW, "los radiadores siguen apagados: apagar la caldera los apagó");
  press(UI_TOGGLE_HEAT);
  run(100);
  CHECK(relay[RELAY_CALEF_PIN] == HIGH, "al reactivar los radiadores calientan al momento");
}

static void t_heat_back_during_run_on() {
  heatingFor(ONE_HOUR);
  uint32_t on = boilerChangedAt;
  haCaldera(false);
  run(30000);
  haMode(true);
  run(100);
  CHECK(cfg.acsOn && relay[RELAY_CALEF_PIN] == HIGH, "modo calor enciende caldera y radiadores");
  CHECK(boilerChangedAt == on, "la caldera no debe haber parpadeado");
  run(CYCLE_MS * 2);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH && relay[RELAY_CALEF_PIN] == HIGH, "sigue calentando");
}

static void t_off_on_off_keeps_first_deadline() {
  heatingFor(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  uint32_t t0 = nowMs;
  run(60000);
  press(UI_TOGGLE_ACS);
  run(60000);
  press(UI_TOGGLE_ACS);   // ya sin calefacción en marcha: no vuelve a empezar la cuenta
  run(CYCLE_MS);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "debe apagarse");
  CHECK(boilerOffAt - t0 >= CYCLE_MS && boilerOffAt - t0 < CYCLE_MS + 50, "se apagó a los %u ms", (unsigned)(boilerOffAt - t0));
}

static void t_repeated_ha_message() {
  heatingFor(ONE_HOUR);
  haCaldera(false);
  uint32_t t0 = nowMs;
  run(60000);
  haCaldera(false);   // HA repite la misma orden: no debe alargar la espera
  run(CYCLE_MS);
  CHECK(boilerOffAt - t0 >= CYCLE_MS && boilerOffAt - t0 < CYCLE_MS + 50, "se apagó a los %u ms", (unsigned)(boilerOffAt - t0));
}

static void t_radiators_off_keeps_boiler() {
  heatingFor(ONE_HOUR);
  haMode(false);
  CHECK(relay[RELAY_CALEF_PIN] == LOW, "los radiadores se apagan al momento");
  run(ONE_HOUR);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH && cfg.acsOn, "la caldera sigue encendida");
  press(UI_TOGGLE_ACS);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "ya sin calefacción, se apaga sin ciclo extra");
}

// La electroválvula debe tener el ciclo entero para cerrar, sea cual sea el motivo del corte
static void t_radiators_cut_just_before_caldera_off() {
  heatingFor(ONE_HOUR);
  haMode(false);
  uint32_t cut = nowMs;
  CHECK(relay[RELAY_CALEF_PIN] == LOW, "los radiadores se apagan al momento");
  run(5000);
  press(UI_TOGGLE_ACS);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "la caldera no puede apagarse a los 5 s del corte");
  run(CYCLE_MS);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "debe apagarse");
  CHECK(boilerOffAt - cut >= CYCLE_MS && boilerOffAt - cut < CYCLE_MS + 50, "se apagó a los %u ms del corte", (unsigned)(boilerOffAt - cut));
}

static void t_setpoint_reached_just_before_caldera_off() {
  heatingFor(ONE_HOUR);
  simTemp = 21;
  run(READ_INTERVAL_MS + 20);
  CHECK(relay[RELAY_CALEF_PIN] == LOW, "consigna alcanzada: radiadores cortados");
  uint32_t cut = calefOffAt;
  run(2000);
  haCaldera(false);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "la caldera no puede apagarse a los 2 s del corte");
  run(CYCLE_MS);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "debe apagarse");
  CHECK(boilerOffAt - cut >= CYCLE_MS && boilerOffAt - cut < CYCLE_MS + 50, "se apagó a los %u ms del corte", (unsigned)(boilerOffAt - cut));
}

static void t_sensor_lost_just_before_caldera_off() {
  heatingFor(ONE_HOUR);
  simSensorOk = false;
  run(READ_INTERVAL_MS + 20);
  CHECK(relay[RELAY_CALEF_PIN] == LOW, "sin sensor: radiadores cortados");
  uint32_t cut = calefOffAt;
  press(UI_TOGGLE_ACS);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "la caldera no puede apagarse justo tras el corte");
  run(CYCLE_MS);
  CHECK(boilerOffAt - cut >= CYCLE_MS && boilerOffAt - cut < CYCLE_MS + 50, "se apagó a los %u ms del corte", (unsigned)(boilerOffAt - cut));
}

static void t_shorter_cycle_frees_wait() {
  heatingFor(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  run(10000);
  intervalMs = 0;
  haCycle(0);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW && !boilerWaiting, "acortar el ciclo libera la espera");
}

static void t_longer_cycle_extends_wait() {
  heatingFor(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  uint32_t t0 = nowMs;
  run(10000);
  haCycle(10);
  run(CYCLE_MS);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "con 10 min aún no se apaga");
  run(600000 - CYCLE_MS - 10000);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "debe apagarse a los 10 min");
  CHECK(boilerOffAt - t0 >= 600000 && boilerOffAt - t0 < 600050, "se apagó a los %u ms", (unsigned)(boilerOffAt - t0));
}

static void t_millis_rollover() {
  heatingFor(0, 0xFFFFFFFFU - CYCLE_MS - 90000);   // el reloj se desborda durante la espera
  uint32_t before = nowMs;
  press(UI_TOGGLE_ACS);
  CHECK(nowMs > before, "aún no se ha desbordado");
  expectFullRunOn(nowMs);
  CHECK(nowMs < before, "la prueba debe cruzar el desbordamiento");
}

static void t_slow_loop_passes_the_deadline() {
  // El bucle no cae justo en el final del ciclo: la espera debe terminar igual
  heatingFor(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  uint32_t t0 = nowMs;
  while (nowMs - t0 < CYCLE_MS - 100) tick(10);
  tick(147);
  tick(147);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "la caldera se queda encendida si el bucle se pasa del final");
  CHECK(!boilerWaiting, "no debe quedar nada esperando");

  // Igual si el bucle se bloquea varios segundos justo al acabar (flash, pila Zigbee)
  heatingFor(ONE_HOUR);
  press(UI_TOGGLE_ACS);
  t0 = nowMs;
  while (nowMs - t0 < CYCLE_MS - 100) tick(10);
  tick(5000);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "la caldera se queda encendida tras un bloqueo del bucle");
  CHECK(!boilerWaiting, "no debe quedar nada esperando");
}

static void t_boot_waits_cycle() {
  boot(CYCLE, true, true, 18);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW && relay[RELAY_CALEF_PIN] == LOW, "tras un corte no arranca enseguida");
  run(CYCLE_MS - 1000);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW, "aún espera el ciclo");
  run(1100);
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH && relay[RELAY_CALEF_PIN] == HIGH, "acabado el ciclo, calienta");
}

static void t_off_during_boot_wait() {
  boot(CYCLE, true, true, 18);
  run(30000);
  press(UI_TOGGLE_ACS);
  CHECK(!boilerWaiting, "ya no hay nada que encender");
  run(CYCLE_MS * 2);
  CHECK(relay[RELAY_CALDERA_PIN] == LOW && relay[RELAY_CALEF_PIN] == LOW, "no debe encenderse");
}

static void t_sensor_lost() {
  heatingFor(ONE_HOUR);
  simSensorOk = false;
  run(READ_INTERVAL_MS + 20);
  CHECK(relay[RELAY_CALEF_PIN] == LOW, "sin sensor se corta la calefacción");
  CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "la caldera sigue: está encendida a mano");
  CHECK(!heatDemand, "sin sensor no pide calor");
}

static void t_setpoint_reached_keeps_boiler() {
  heatingFor(ONE_HOUR);
  simTemp = 21;
  run(READ_INTERVAL_MS + 20);
  CHECK(relay[RELAY_CALEF_PIN] == LOW && relay[RELAY_CALDERA_PIN] == HIGH, "alcanzada la consigna, solo se cortan los radiadores");
  simTemp = 18;
  run(READ_INTERVAL_MS + 20);
  CHECK(relay[RELAY_CALEF_PIN] == HIGH, "vuelve a pedir calor");
}

// Órdenes al azar: en todo momento deben cumplirse las reglas de seguridad
static void fuzz(unsigned seed, uint16_t cycle) {
  boot(cycle, seed & 1, seed & 2, 18, 5000 + seed * 12345);
  uint32_t cycleMs = cycle * 6000U;
  std::mt19937 rng(seed);
  uint32_t runOnStart = 0;
  bool runOn = false;
  for (int op = 0; op < 400; ++op) {
    uint32_t wait = 100 + rng() % (cycleMs * 2 + 20000);
    for (uint32_t end = nowMs + wait; (int32_t)(end - nowMs) > 0;) {
      tick(1 + rng() % 200);
      if (runOn && nowMs - runOnStart < cycleMs) CHECK(relay[RELAY_CALDERA_PIN] == HIGH, "caldera apagada durante el ciclo tras cortar la calefacción");
      CHECK(boilerOn == (relay[RELAY_CALDERA_PIN] == HIGH) && calefOn == (relay[RELAY_CALEF_PIN] == HIGH), "estado distinto del relé");
      CHECK(!calefOn || (cfg.heatMode && heatDemand), "calefacción encendida sin pedirla");
      CHECK(!cfg.heatMode || cfg.acsOn, "modo calor sin caldera");
      // Nada se queda atascado: cumplido el ciclo, el relé hace lo que se le pide
      if (nowMs - lastBoilerSwitch > cycleMs + 450)
        CHECK(boilerOn == (cfg.acsOn || heatDemand), "la caldera se ha quedado atascada");
    }
    bool wasRunning = calefOn, wasAcs = cfg.acsOn;
    switch (rng() % 8) {
      case 0: press(UI_TOGGLE_ACS); break;
      case 1: press(UI_TOGGLE_HEAT); break;
      case 2: haCaldera(rng() & 1); break;
      case 3: haMode(rng() & 1); break;
      case 4: simTemp = 17 + (rng() % 60) / 10.0f; break;
      case 5: simSensorOk = rng() % 4 != 0; break;
      case 6: press((rng() & 1) ? UI_SP_UP : UI_SP_DOWN); break;
      default: break;
    }
    if (wasAcs && !cfg.acsOn && wasRunning) {
      CHECK(!calefOn && relay[RELAY_CALEF_PIN] == LOW, "la calefacción debe cortarse al instante");
      runOn = true;
      runOnStart = nowMs;
    }
  }
}

static void t_fuzz() {
  const uint16_t cycles[] = {0, 1, 5, CYCLE};
  for (unsigned seed = 1; seed <= 96; ++seed) fuzz(seed, cycles[seed % 4]);
}

#define RUN(fn)                                       \
  do {                                                \
    currentTest = #fn;                                \
    int before = failures;                            \
    fn();                                             \
    std::printf("%s %s\n", failures == before ? "ok   " : "MAL  ", #fn); \
  } while (0)

int main() {
  RUN(t_screen_after_hour);
  RUN(t_ha_after_hour);
  RUN(t_recent_boiler_gets_full_cycle);
  RUN(t_published_at_once);
  RUN(t_countdown);
  RUN(t_zero_cycle);
  RUN(t_no_heating_after_hour);
  RUN(t_no_heating_recent_only_remaining);
  RUN(t_caldera_back_on_cancels);
  RUN(t_heat_back_during_run_on);
  RUN(t_off_on_off_keeps_first_deadline);
  RUN(t_repeated_ha_message);
  RUN(t_radiators_off_keeps_boiler);
  RUN(t_radiators_cut_just_before_caldera_off);
  RUN(t_setpoint_reached_just_before_caldera_off);
  RUN(t_sensor_lost_just_before_caldera_off);
  RUN(t_shorter_cycle_frees_wait);
  RUN(t_longer_cycle_extends_wait);
  RUN(t_millis_rollover);
  RUN(t_slow_loop_passes_the_deadline);
  RUN(t_boot_waits_cycle);
  RUN(t_off_during_boot_wait);
  RUN(t_sensor_lost);
  RUN(t_setpoint_reached_keeps_boiler);
  RUN(t_fuzz);
  std::printf("%d comprobaciones, %d fallos\n", checks, failures);
  return failures ? 1 : 0;
}
