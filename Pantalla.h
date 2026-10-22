#pragma once
/*
 * Pantalla táctil 3,5" 320x480 SPI (ILI9488 o ST7796S + táctil resistivo XPT2046).
 * Librería: LovyanGFX (gestor de librerías). Se configura aquí, sin tocar la librería.
 *
 * El sketch principal rellena un UiState, llama a uiUpdate() para pintar solo lo
 * que ha cambiado y a uiPoll() para saber qué botón se ha pulsado.
 * Las fuentes de LovyanGFX solo tienen ASCII: los textos van sin tildes y el
 * símbolo de grado se dibuja a mano.
 */

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Preferences.h>

// ---------------- Configuración de la pantalla ----------------
// Chip de la pantalla: estas pantallas de 3,5" se venden como "ILI9341", pero a 320x480
// llevan ILI9488 o ST7796S. Si la imagen sale rara o en negro, prueba la otra.
#define PANEL_ILI9488          // comenta esta línea para usar ST7796S
#define PANEL_INVERT  false    // pon true si los colores salen invertidos (fondo blanco)
#define PANEL_BGR     false    // pon true si el rojo y el azul salen cambiados

#define TFT_SCK    21
#define TFT_MOSI   22   // SDI(MOSI) y T_DIN
#define TFT_MISO   23   // solo T_DO (o T_OUT): no conectes SDO(MISO) de la pantalla
#define TFT_CS     20
#define TFT_DC     14   // DC/RS
#define TFT_RST    0
#define TFT_BL     1    // LED (retroiluminación)
#define TOUCH_CS   2    // T_CS
#define TOUCH_IRQ  3    // T_IRQ

#define BL_ON         255      // brillo normal (0-255)
#define BL_DIM        0        // brillo en reposo (0 = apagada; alarga la vida de la retroiluminación)
#define DIM_AFTER_MS  30000    // sin tocar este tiempo -> brillo de reposo (y vuelta a la pantalla principal)
#define SP_STEP       50       // paso de la consigna: 0,5 ºC
// ---------------------------------------------------------------

class LGFX : public lgfx::LGFX_Device {
#ifdef PANEL_ILI9488
  lgfx::Panel_ILI9488 _panel;
#else
  lgfx::Panel_ST7796 _panel;
#endif
  lgfx::Bus_SPI _bus;
  lgfx::Touch_XPT2046 _touch;

public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000;   // con cables largos, si hay basura en pantalla, baja a 20 MHz
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = TFT_SCK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_dc = TFT_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = TFT_CS;
      cfg.pin_rst = TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 320;
      cfg.panel_height = 480;
      cfg.readable = false;
      cfg.invert = PANEL_INVERT;
      cfg.rgb_order = PANEL_BGR;
      cfg.bus_shared = true;   // el bus SPI se comparte con el táctil
      _panel.config(cfg);
    }
    {
      auto cfg = _touch.config();
      cfg.x_min = 300;  cfg.x_max = 3900;   // valores aproximados hasta calibrar
      cfg.y_min = 200;  cfg.y_max = 3900;
      cfg.pin_int = TOUCH_IRQ;
      cfg.bus_shared = true;
      cfg.offset_rotation = 0;
      cfg.spi_host = SPI2_HOST;
      cfg.freq = 1000000;
      cfg.pin_sclk = TFT_SCK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_cs = TOUCH_CS;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
    setPanel(&_panel);
  }
};

struct UiState {
  float   temp;        // ºC con la que se regula
  float   hum;         // % (SHT31)
  bool    sensorOk;    // hay temperatura para regular
  bool    humOk;       // el SHT31 responde
  bool    extSelected; // elegido el sensor externo
  bool    extActive;   // se está usando el externo (si no llega, se usa el SHT31)
  bool    heatMode;    // calefacción en modo Calor
  bool    heating;     // relé de calefacción activo
  bool    heatWanted;  // la calefacción pide calor (puede estar esperando a la caldera)
  bool    boilerOn;    // relé de la caldera activo
  uint32_t waitSecs;   // segundos hasta que la caldera pueda cambiar (ciclo mínimo); 0 = nada pendiente
  int16_t setpoint;    // 0,01 ºC
  bool    acs;         // caldera (agua caliente) encendida
  bool    connected;   // red Zigbee
  bool    ota;         // descargando firmware
  float   tempInt;     // ºC del SHT31 (ya calibrada; vale si humOk)
  float   tempExt;     // ºC externa recibida de HA (vale si extOk)
  bool    extOk;       // la temperatura externa es reciente
  uint16_t minCycle;   // ciclo mínimo de la caldera, décimas de minuto
  uint16_t hystOn;     // margen de arranque bajo la consigna, centésimas de ºC
  uint16_t hystOff;    // margen de corte sobre la consigna, centésimas de ºC
};

enum UiAction { UI_NONE, UI_SP_DOWN, UI_SP_UP, UI_TOGGLE_HEAT, UI_TOGGLE_ACS, UI_SRC_INT, UI_SRC_EXT, UI_CYC_DOWN, UI_CYC_UP,
                UI_HON_DOWN, UI_HON_UP, UI_HOFF_DOWN, UI_HOFF_UP };

namespace ui {

LGFX lcd;

// Colores
const uint32_t C_BG     = 0x101418;
const uint32_t C_PANEL  = 0x22282E;
const uint32_t C_TEXT   = 0xF0F0F0;
const uint32_t C_DIM    = 0x8A949E;
const uint32_t C_ORANGE = 0xFF8C1A;
const uint32_t C_BLUE   = 0x2E9BFF;
const uint32_t C_RED    = 0xFF4040;
const uint32_t C_GREEN  = 0x3CCB6A;
const uint32_t C_WAIT   = 0x7A4A10;   // botón esperando al ciclo mínimo

struct Rect {
  int16_t x, y, w, h;
  bool hit(int32_t px, int32_t py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
const Rect R_TOP   = { 0, 0, 480, 36 };
const Rect R_TEMP  = { 0, 40, 240, 190 };
const Rect R_SP    = { 244, 40, 236, 98 };
const Rect B_MINUS = { 256, 144, 100, 76 };
const Rect B_PLUS  = { 368, 144, 100, 76 };
const Rect B_HEAT  = { 8, 238, 228, 76 };
const Rect B_ACS   = { 244, 238, 228, 76 };
const Rect B_MENU  = { 170, 2, 140, 32 };    // "Ajustes", en la barra superior
// Pantalla de ajustes
const Rect B_BACK      = { 4, 2, 140, 32 };
const Rect B_SRC_INT   = { 8, 64, 228, 60 };
const Rect B_SRC_EXT   = { 244, 64, 228, 60 };
const Rect B_CYC_MINUS = { 8, 156, 100, 60 };
const Rect B_CYC_PLUS  = { 372, 156, 100, 60 };
const Rect R_CYC       = { 116, 156, 248, 60 };
// Márgenes de la histéresis, uno en cada mitad
const Rect B_HON_MINUS  = { 8, 248, 60, 60 };
const Rect R_HON        = { 68, 248, 108, 60 };
const Rect B_HON_PLUS   = { 176, 248, 60, 60 };
const Rect B_HOFF_MINUS = { 244, 248, 60, 60 };
const Rect R_HOFF       = { 304, 248, 108, 60 };
const Rect B_HOFF_PLUS  = { 412, 248, 60, 60 };

enum Screen { SCR_MAIN, SCR_MENU };
Screen screen = SCR_MAIN;
Screen drawnScreen = SCR_MAIN;

UiState prev;
bool drawn = false;
unsigned long lastTouch = 0;
bool dimmed = false;
bool pressed = false;           // dedo puesto (para no repetir acciones)
bool settling = false;          // dedo recién apoyado, esperando lectura estable
unsigned long touchDownAt = 0;
int32_t lastX = 0, lastY = 0;   // última lectura con el dedo puesto
unsigned long releaseSince = 0; // primera lectura sin dedo tras un toque (0 = no)
const unsigned long TOUCH_SETTLE_MS = 40;
const unsigned long TOUCH_RELEASE_MS = 60;
UiAction repeatAction = UI_NONE;
unsigned long nextRepeat = 0;

void backlight(uint8_t v) { ledcWrite(TFT_BL, v); }

// Texto centrado en cx con el símbolo de grado detrás
void drawDegrees(const char *txt, int32_t cx, int32_t cy, const lgfx::IFont *font, uint32_t color) {
  lcd.setFont(font);
  lcd.setTextDatum(textdatum_t::middle_left);
  int32_t h = lcd.fontHeight();
  int32_t r = h / 12 + 2;
  int32_t w = lcd.textWidth(txt) + r * 2 + 4;
  int32_t x = cx - w / 2;
  lcd.setTextColor(color);
  lcd.drawString(txt, x, cy);
  int32_t dx = x + lcd.textWidth(txt) + r + 3;
  int32_t dy = cy - h / 2 + r + h / 10;
  lcd.fillCircle(dx, dy, r, color);
  lcd.fillCircle(dx, dy, r - (r / 3 > 2 ? r / 3 : 2), C_BG);   // anillo: el fondo de estas zonas es C_BG
}

void drawButton(const Rect &r, const char *label, const char *state, uint32_t fill, uint32_t textColor) {
  lcd.fillRoundRect(r.x, r.y, r.w, r.h, 12, fill);
  lcd.setTextColor(textColor);
  lcd.setTextDatum(textdatum_t::middle_center);
  if (state) {
    lcd.setFont(&fonts::DejaVu18);
    lcd.drawString(label, r.x + r.w / 2, r.y + r.h * 29 / 100);
    lcd.setFont(&fonts::DejaVu24);
    lcd.drawString(state, r.x + r.w / 2, r.y + r.h * 68 / 100);
  } else {
    lcd.setFont(r.h >= 70 ? &fonts::DejaVu56 : &fonts::DejaVu40);
    lcd.drawString(label, r.x + r.w / 2, r.y + r.h / 2 + 2);
  }
}

void drawTop(const UiState &s) {
  lcd.fillRect(R_TOP.x, R_TOP.y, R_TOP.w, R_TOP.h, C_PANEL);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_left);
  uint32_t zc = s.ota ? C_BLUE : (s.connected ? C_GREEN : C_RED);
  lcd.fillCircle(16, 18, 6, zc);
  lcd.setTextColor(C_TEXT);
  lcd.drawString(s.ota ? "Actualizando..." : (s.connected ? "Zigbee" : "Sin red"), 30, 19);

  lcd.fillRoundRect(B_MENU.x, B_MENU.y, B_MENU.w, B_MENU.h, 8, C_BG);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString("Ajustes", B_MENU.x + B_MENU.w / 2, B_MENU.y + B_MENU.h / 2 + 1);

  char buf[16];
  if (s.humOk) snprintf(buf, sizeof(buf), "Hum %.0f%%", s.hum);
  else strcpy(buf, "Hum --");
  lcd.setTextDatum(textdatum_t::middle_right);
  lcd.setTextColor(C_TEXT);
  lcd.drawString(buf, 470, 19);
}

void drawTemp(const UiState &s) {
  lcd.fillRect(R_TEMP.x, R_TEMP.y, R_TEMP.w, R_TEMP.h, C_BG);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_center);
  // Sensor con el que se regula (se elige en Ajustes)
  const char *src;
  uint32_t srcColor = C_DIM;
  if (!s.extSelected)      src = "Sensor interno";
  else if (s.extActive)    src = "Sensor externo";
  else                   { src = "Externo sin datos"; srcColor = C_ORANGE; }
  lcd.setTextColor(srcColor);
  lcd.drawString(src, 120, 60);

  char buf[12];
  if (s.sensorOk) snprintf(buf, sizeof(buf), "%.1f", s.temp);
  else strcpy(buf, "--.-");
  drawDegrees(buf, 120, 132, &fonts::DejaVu72, s.sensorOk ? C_TEXT : C_RED);

  const char *st;
  uint32_t c;
  if (!s.sensorOk)      { st = "Sensor sin datos"; c = C_RED; }
  else if (!s.heatMode) { st = "Calefaccion apagada"; c = C_DIM; }
  else if (s.heating)   { st = "Calentando"; c = C_ORANGE; }
  else if (s.heatWanted) { st = "Esperando a la caldera"; c = C_ORANGE; }
  else                  { st = "En reposo"; c = C_DIM; }
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(c);
  lcd.drawString(st, 120, 205);
}

void drawSetpoint(const UiState &s) {
  lcd.fillRect(R_SP.x, R_SP.y, R_SP.w, R_SP.h, C_BG);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_DIM);
  lcd.drawString("Consigna", 362, 60);
  char buf[12];
  snprintf(buf, sizeof(buf), "%.1f", s.setpoint / 100.0f);
  drawDegrees(buf, 362, 106, &fonts::DejaVu40, s.heatMode ? C_TEXT : C_DIM);
}

void drawButtons(const UiState &s, bool heat, bool acs) {
  if (heat) drawButton(B_HEAT, "Radiadores", s.heatMode ? "ENCENDIDA" : "APAGADA",
                       s.heatMode ? C_ORANGE : C_PANEL, s.heatMode ? C_BG : C_TEXT);
  if (acs && s.waitSecs) {
    // Cambio retenido por el ciclo mínimo de la caldera: cuenta atrás en el botón
    char buf[20];
    snprintf(buf, sizeof(buf), "%s %lu:%02lu", s.boilerOn ? "Apaga" : "Enciende",
             (unsigned long)(s.waitSecs / 60), (unsigned long)(s.waitSecs % 60));
    drawButton(B_ACS, "Caldera", buf, C_WAIT, C_TEXT);
  } else if (acs) drawButton(B_ACS, "Caldera", s.acs ? "ENCENDIDA" : "APAGADA",
                       s.acs ? C_BLUE : C_PANEL, s.acs ? C_BG : C_TEXT);
}

// ---------------- Pantalla de ajustes ----------------
void drawMenuTop() {
  lcd.fillRect(R_TOP.x, R_TOP.y, R_TOP.w, R_TOP.h, C_PANEL);
  lcd.fillRoundRect(B_BACK.x, B_BACK.y, B_BACK.w, B_BACK.h, 8, C_BG);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextColor(C_TEXT);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString("< Volver", B_BACK.x + B_BACK.w / 2, B_BACK.y + B_BACK.h / 2 + 1);
  lcd.drawString("Ajustes", 240, 19);
}

void drawMenuSource(const UiState &s) {
  lcd.fillRect(0, 38, 480, 90, C_BG);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_DIM);
  lcd.drawString("Termometro para regular", 240, 51);
  char a[16], b[16];
  if (s.humOk) snprintf(a, sizeof(a), "%.1f C", s.tempInt);
  else strcpy(a, "sin datos");
  if (s.extOk) snprintf(b, sizeof(b), "%.1f C", s.tempExt);
  else strcpy(b, "sin datos");
  drawButton(B_SRC_INT, "Interno", a, s.extSelected ? C_PANEL : C_BLUE, s.extSelected ? C_TEXT : C_BG);
  drawButton(B_SRC_EXT, "Externo", b, s.extSelected ? C_BLUE : C_PANEL, s.extSelected ? C_BG : C_TEXT);
}

// Fila de ajuste numérico: título, - valor +
void drawMenuValue(const char *title, const char *value, const Rect &minus, const Rect &val, const Rect &plus, bool buttons) {
  if (buttons) {
    lcd.fillRect(0, val.y - 28, 480, val.h + 30, C_BG);
    lcd.setFont(&fonts::DejaVu18);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(C_DIM);
    lcd.drawString(title, 240, val.y - 14);
    drawButton(minus, "-", nullptr, C_PANEL, C_TEXT);
    drawButton(plus, "+", nullptr, C_PANEL, C_TEXT);
  }
  lcd.fillRect(val.x, val.y, val.w, val.h, C_BG);
  lcd.setFont(&fonts::DejaVu40);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_TEXT);
  lcd.drawString(value, val.x + val.w / 2, val.y + val.h / 2 + 2);
}

void drawMenuCycle(const UiState &s, bool buttons) {
  char buf[16];
  snprintf(buf, sizeof(buf), "%.1f min", s.minCycle / 10.0f);
  drawMenuValue("Ciclo minimo caldera (encendida o apagada)", buf, B_CYC_MINUS, R_CYC, B_CYC_PLUS, buttons);
}

// Media fila: título, - valor + (para los dos márgenes de la histéresis)
void drawMenuHalf(const char *title, uint16_t hundredths, const Rect &minus, const Rect &val, const Rect &plus, bool buttons) {
  if (buttons) {
    lcd.fillRect(minus.x, val.y - 28, plus.x + plus.w - minus.x, val.h + 30, C_BG);
    lcd.setFont(&fonts::DejaVu18);
    lcd.setTextDatum(textdatum_t::middle_center);
    lcd.setTextColor(C_DIM);
    lcd.drawString(title, val.x + val.w / 2, val.y - 14);
    drawButton(minus, "-", nullptr, C_PANEL, C_TEXT);
    drawButton(plus, "+", nullptr, C_PANEL, C_TEXT);
  }
  char buf[12];
  if (hundredths % 10 == 0) snprintf(buf, sizeof(buf), "%.1f C", hundredths / 100.0f);
  else snprintf(buf, sizeof(buf), "%.2f C", hundredths / 100.0f);
  lcd.fillRect(val.x, val.y, val.w, val.h, C_BG);
  lcd.setFont(&fonts::DejaVu24);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_TEXT);
  lcd.drawString(buf, val.x + val.w / 2, val.y + val.h / 2 + 2);
}

// ---------------- Calibración del táctil ----------------
bool loadCalibration(Preferences &prefs) {
  uint16_t cal[8];
  prefs.begin("tft", true);
  bool ok = prefs.getBytes("cal", cal, sizeof(cal)) == sizeof(cal);
  prefs.end();
  if (ok) lcd.setTouchCalibrate(cal);
  return ok;
}

// Cruz de calibración
void drawCross(int32_t x, int32_t y, uint32_t color) {
  lcd.drawFastHLine(x - 15, y, 31, color);
  lcd.drawFastVLine(x, y - 15, 31, color);
  lcd.drawCircle(x, y, 6, color);
}

// Lectura cruda estable del táctil en un punto: espera a que se pulse, promedia y
// espera a que se suelte. Admite más variación que la calibración de LovyanGFX,
// que en las esquinas de algunos táctiles no llega a aceptar nunca el toque.
void readCalPoint(int32_t &rx, int32_t &ry) {
  const int SAMPLES = 6, MAX_ERR = 60;
  int32_t sx = 0, sy = 0, n = 0, x1, y1, x2, y2;
  while (n < SAMPLES) {
    // Las lecturas de los primeros ms tras apoyar el dedo saltan: se descartan
    if (!lcd.getTouchRaw(&x1, &y1)) {
      while (!lcd.getTouchRaw(&x1, &y1)) delay(2);
      delay(100);
      continue;
    }
    delay(10);
    if (!lcd.getTouchRaw(&x2, &y2) || abs(x1 - x2) > MAX_ERR || abs(y1 - y2) > MAX_ERR) continue;
    sx += x1 + x2;
    sy += y1 + y2;
    n++;
  }
  rx = sx / (2 * SAMPLES);
  ry = sy / (2 * SAMPLES);
  while (lcd.getTouchRaw(&x1, &y1)) delay(5);
  delay(300);
}

// Calibra con 4 puntos separados CAL_INSET px de los bordes (las esquinas del táctil
// resistivo responden mal) y extrapola a las esquinas, que es lo que espera
// setTouchCalibrate. Se hace en la orientación propia del táctil (rotación 0, 320x480):
// orden de los puntos (0,0), (0,h-1), (w-1,0), (w-1,h-1).
void calibrate(Preferences &prefs) {
  const int32_t CAL_INSET = 40;
  uint8_t rot = lcd.getRotation();
  lcd.setRotation(0);
  int32_t w = lcd.width(), h = lcd.height();
  lcd.fillScreen(C_BG);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_TEXT);
  lcd.drawString("Toca el centro", w / 2, h / 2 - 12);
  lcd.drawString("de cada cruz", w / 2, h / 2 + 12);

  int32_t px[4], py[4], rx[4], ry[4];
  for (int i = 0; i < 4; ++i) {
    px[i] = (i & 2) ? w - 1 - CAL_INSET : CAL_INSET;
    py[i] = (i & 1) ? h - 1 - CAL_INSET : CAL_INSET;
    drawCross(px[i], py[i], C_ORANGE);
    readCalPoint(rx[i], ry[i]);
    drawCross(px[i], py[i], C_BG);
    Serial.printf("Calibración punto %d (%ld,%ld): crudo %ld,%ld\n", i, (long)px[i], (long)py[i], (long)rx[i], (long)ry[i]);
  }

  // Extrapolación a las esquinas: cada esquina se aleja de su punto lo mismo, en
  // proporción, que la distancia entre puntos opuestos
  float kx = (float)CAL_INSET / (w - 1 - 2 * CAL_INSET);
  float ky = (float)CAL_INSET / (h - 1 - 2 * CAL_INSET);
  uint16_t cal[8];
  for (int i = 0; i < 4; ++i) {
    int ox = i ^ 2, oy = i ^ 1;   // punto opuesto en x y en y
    float cx = rx[i] + (rx[i] - rx[ox]) * kx + (rx[i] - rx[oy]) * ky;
    float cy = ry[i] + (ry[i] - ry[ox]) * kx + (ry[i] - ry[oy]) * ky;
    cal[i * 2]     = (uint16_t)constrain(lroundf(cx), 0, 4095);
    cal[i * 2 + 1] = (uint16_t)constrain(lroundf(cy), 0, 4095);
  }
  lcd.setTouchCalibrate(cal);
  lcd.setRotation(rot);
  prefs.begin("tft", false);
  prefs.putBytes("cal", cal, sizeof(cal));
  prefs.end();
  Serial.println("Táctil calibrado");
}

}  // namespace ui

// Arranca la pantalla. Si no hay calibración guardada, o se toca la pantalla
// durante el arranque, lanza la calibración del táctil.
void uiBegin(Preferences &prefs) {
  using namespace ui;
  ledcAttach(TFT_BL, 5000, 8);
  backlight(0);
  lcd.init();
  pinMode(TOUCH_IRQ, INPUT_PULLUP);   // sin pantalla conectada, que no lea toques falsos
  lcd.setRotation(1);                 // horizontal, 480x320
  lcd.fillScreen(C_BG);
  backlight(BL_ON);

  bool hasCal = loadCalibration(prefs);
  lcd.setFont(&fonts::DejaVu24);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_TEXT);
  lcd.drawString("Termostato", 240, 130);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextColor(C_DIM);
  lcd.drawString(hasCal ? "Toca la pantalla para calibrar" : "Toca la pantalla para calibrar el tactil", 240, 180);

  // Ventana para pedir la calibración (más larga si nunca se ha calibrado)
  unsigned long t0 = millis(), wait = hasCal ? 2000 : 8000;
  int32_t x, y;
  while (millis() - t0 < wait) {
    if (lcd.getTouchRaw(&x, &y)) {
      while (lcd.getTouchRaw(&x, &y)) delay(20);   // espera a que se suelte
      calibrate(prefs);
      break;
    }
    delay(20);
  }
  lcd.fillScreen(C_BG);
  drawn = false;
  lastTouch = millis();
}

// Repinta solo las zonas que han cambiado
void uiUpdate(const UiState &s) {
  using namespace ui;
  bool all = !drawn || screen != drawnScreen;
  if (drawn && screen != drawnScreen) lcd.fillScreen(C_BG);
  drawnScreen = screen;
  // Redondeo a lo que se ve en pantalla para no repintar por centésimas
  auto t10 = [](float v) { return isnan(v) ? INT32_MIN : (int)lroundf(v * 10); };

  if (screen == SCR_MENU) {
    if (all) drawMenuTop();
    if (all || s.extSelected != prev.extSelected || s.extOk != prev.extOk || s.humOk != prev.humOk ||
        t10(s.tempInt) != t10(prev.tempInt) || t10(s.tempExt) != t10(prev.tempExt))
      drawMenuSource(s);
    if (all || s.minCycle != prev.minCycle)
      drawMenuCycle(s, all);
    if (all || s.hystOn != prev.hystOn)
      drawMenuHalf("Arranca por debajo", s.hystOn, B_HON_MINUS, R_HON, B_HON_PLUS, all);
    if (all || s.hystOff != prev.hystOff)
      drawMenuHalf("Corta por encima", s.hystOff, B_HOFF_MINUS, R_HOFF, B_HOFF_PLUS, all);
    prev = s;
    drawn = true;
    return;
  }

  if (all || s.connected != prev.connected || s.ota != prev.ota ||
      s.humOk != prev.humOk || (int)lroundf(s.hum) != (int)lroundf(prev.hum))
    drawTop(s);
  if (all || s.sensorOk != prev.sensorOk || t10(s.temp) != t10(prev.temp) || s.heatMode != prev.heatMode || s.heating != prev.heating ||
      s.heatWanted != prev.heatWanted ||
      s.extSelected != prev.extSelected || s.extActive != prev.extActive)
    drawTemp(s);
  if (all || s.setpoint != prev.setpoint || s.heatMode != prev.heatMode)
    drawSetpoint(s);
  if (all) {
    drawButton(B_MINUS, "-", nullptr, C_PANEL, C_TEXT);
    drawButton(B_PLUS, "+", nullptr, C_PANEL, C_TEXT);
  }
  drawButtons(s, all || s.heatMode != prev.heatMode,
              all || s.acs != prev.acs || s.boilerOn != prev.boilerOn || s.waitSecs != prev.waitSecs);

  prev = s;
  drawn = true;
}

UiAction pressAt(int32_t x, int32_t y, unsigned long now);

// Lee el táctil. Devuelve la acción pulsada (con autorrepetición en + / -).
// El primer toque con la pantalla atenuada solo la enciende.
UiAction uiPoll() {
  using namespace ui;
  unsigned long now = millis();
  int32_t x, y;
  bool touch = lcd.getTouch(&x, &y);

  if (!touch) {
    // Con poca presión el táctil pierde el contacto unos ms aunque el dedo siga
    // apoyado: solo cuenta como soltar tras TOUCH_RELEASE_MS seguidos sin lectura
    if (pressed || settling) {
      if (!releaseSince) releaseSince = now;
      if (now - releaseSince < TOUCH_RELEASE_MS) return UI_NONE;
    }
    releaseSince = 0;
    // Toque tan breve que no llegó a asentarse: vale su última lectura
    if (settling) return pressAt(lastX, lastY, now);
    pressed = false;
    repeatAction = UI_NONE;
    if (!dimmed && now - lastTouch > DIM_AFTER_MS) {
      dimmed = true;
      backlight(BL_DIM);
      screen = SCR_MAIN;   // al apagarse, vuelve de los ajustes a la principal
    }
    return UI_NONE;
  }

  releaseSince = 0;
  lastTouch = now;
  lastX = x;
  lastY = y;
  if (dimmed) {
    dimmed = false;
    backlight(BL_ON);
    pressed = true;   // este toque no pulsa nada
    return UI_NONE;
  }

  if (pressed) {
    // Dedo mantenido en un + / -: repetir
    if (repeatAction != UI_NONE && now >= nextRepeat) {
      nextRepeat = now + 150;
      return repeatAction;
    }
    return UI_NONE;
  }

  // Las primeras lecturas al apoyar el dedo saltan decenas de píxeles (la presión aún
  // no es firme): se decide con la lectura tomada TOUCH_SETTLE_MS después
  if (!settling) {
    settling = true;
    touchDownAt = now;
    return UI_NONE;
  }
  if (now - touchDownAt < TOUCH_SETTLE_MS) return UI_NONE;
  return pressAt(x, y, now);
}

// Acción del botón que hay en (x, y)
UiAction pressAt(int32_t x, int32_t y, unsigned long now) {
  using namespace ui;
  settling = false;
  pressed = true;
  UiAction a = UI_NONE;
  if (screen == SCR_MENU) {
    if (B_BACK.hit(x, y))           screen = SCR_MAIN;
    else if (B_SRC_INT.hit(x, y))   a = UI_SRC_INT;
    else if (B_SRC_EXT.hit(x, y))   a = UI_SRC_EXT;
    else if (B_CYC_MINUS.hit(x, y)) a = UI_CYC_DOWN;
    else if (B_CYC_PLUS.hit(x, y))  a = UI_CYC_UP;
    else if (B_HON_MINUS.hit(x, y))  a = UI_HON_DOWN;
    else if (B_HON_PLUS.hit(x, y))   a = UI_HON_UP;
    else if (B_HOFF_MINUS.hit(x, y)) a = UI_HOFF_DOWN;
    else if (B_HOFF_PLUS.hit(x, y))  a = UI_HOFF_UP;
  } else {
    if (B_MINUS.hit(x, y))     a = UI_SP_DOWN;
    else if (B_PLUS.hit(x, y)) a = UI_SP_UP;
    else if (B_HEAT.hit(x, y)) a = UI_TOGGLE_HEAT;
    else if (B_ACS.hit(x, y))  a = UI_TOGGLE_ACS;
    else if (B_MENU.hit(x, y)) screen = SCR_MENU;
  }

  if (a == UI_SP_DOWN || a == UI_SP_UP || a == UI_CYC_DOWN || a == UI_CYC_UP ||
      a == UI_HON_DOWN || a == UI_HON_UP || a == UI_HOFF_DOWN || a == UI_HOFF_UP) {
    repeatAction = a;
    nextRepeat = now + 500;
  }
  return a;
}
