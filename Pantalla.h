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
#define TFT_MISO   23   // solo T_DO: no conectes SDO(MISO) de la pantalla
#define TFT_CS     20
#define TFT_DC     14   // DC/RS
#define TFT_RST    0
#define TFT_BL     1    // LED (retroiluminación)
#define TOUCH_CS   2    // T_CS
#define TOUCH_IRQ  3    // T_IRQ

#define BL_ON         255      // brillo normal (0-255)
#define BL_DIM        15       // brillo en reposo (0 = apagada)
#define DIM_AFTER_MS  30000    // sin tocar este tiempo -> brillo de reposo
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
  int16_t setpoint;    // 0,01 ºC
  bool    acs;         // caldera (agua caliente) encendida
  bool    connected;   // red Zigbee
  bool    ota;         // descargando firmware
};

enum UiAction { UI_NONE, UI_SP_DOWN, UI_SP_UP, UI_TOGGLE_HEAT, UI_TOGGLE_ACS, UI_TOGGLE_SOURCE };

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

UiState prev;
bool drawn = false;
unsigned long lastTouch = 0;
bool dimmed = false;
bool pressed = false;           // dedo puesto (para no repetir acciones)
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
    lcd.drawString(label, r.x + r.w / 2, r.y + 22);
    lcd.setFont(&fonts::DejaVu24);
    lcd.drawString(state, r.x + r.w / 2, r.y + 52);
  } else {
    lcd.setFont(&fonts::DejaVu56);
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
  // Sensor con el que se regula (tocar esta zona lo cambia)
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
  if (acs)  drawButton(B_ACS, "Caldera", s.acs ? "ENCENDIDA" : "APAGADA",
                       s.acs ? C_BLUE : C_PANEL, s.acs ? C_BG : C_TEXT);
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

void calibrate(Preferences &prefs) {
  lcd.fillScreen(C_BG);
  lcd.setFont(&fonts::DejaVu18);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.setTextColor(C_TEXT);
  lcd.drawString("Toca el centro de cada flecha", 240, 150);
  uint16_t cal[8];
  lcd.calibrateTouch(cal, C_ORANGE, C_BG, 20);
  lcd.setTouchCalibrate(cal);
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
  bool all = !drawn;
  // Redondeo a lo que se ve en pantalla para no repintar por centésimas
  auto t10 = [](float v) { return (int)lroundf(v * 10); };

  if (all || s.connected != prev.connected || s.ota != prev.ota ||
      s.humOk != prev.humOk || (int)lroundf(s.hum) != (int)lroundf(prev.hum))
    drawTop(s);
  if (all || s.sensorOk != prev.sensorOk || t10(s.temp) != t10(prev.temp) || s.heatMode != prev.heatMode || s.heating != prev.heating ||
      s.extSelected != prev.extSelected || s.extActive != prev.extActive)
    drawTemp(s);
  if (all || s.setpoint != prev.setpoint || s.heatMode != prev.heatMode)
    drawSetpoint(s);
  if (all) {
    drawButton(B_MINUS, "-", nullptr, C_PANEL, C_TEXT);
    drawButton(B_PLUS, "+", nullptr, C_PANEL, C_TEXT);
  }
  drawButtons(s, all || s.heatMode != prev.heatMode, all || s.acs != prev.acs);

  prev = s;
  drawn = true;
}

// Lee el táctil. Devuelve la acción pulsada (con autorrepetición en + / -).
// El primer toque con la pantalla atenuada solo la enciende.
UiAction uiPoll() {
  using namespace ui;
  unsigned long now = millis();
  int32_t x, y;
  bool touch = lcd.getTouch(&x, &y);

  if (!touch) {
    pressed = false;
    repeatAction = UI_NONE;
    if (!dimmed && now - lastTouch > DIM_AFTER_MS) {
      dimmed = true;
      backlight(BL_DIM);
    }
    return UI_NONE;
  }

  lastTouch = now;
  if (dimmed) {
    dimmed = false;
    backlight(BL_ON);
    pressed = true;   // este toque no pulsa nada
    return UI_NONE;
  }

  if (pressed) {
    // Dedo mantenido en + / -: repetir
    if (repeatAction != UI_NONE && now >= nextRepeat) {
      nextRepeat = now + 150;
      return repeatAction;
    }
    return UI_NONE;
  }

  pressed = true;
  UiAction a = UI_NONE;
  if (B_MINUS.hit(x, y))     a = UI_SP_DOWN;
  else if (B_PLUS.hit(x, y)) a = UI_SP_UP;
  else if (B_HEAT.hit(x, y)) a = UI_TOGGLE_HEAT;
  else if (B_ACS.hit(x, y))  a = UI_TOGGLE_ACS;
  else if (R_TEMP.hit(x, y)) a = UI_TOGGLE_SOURCE;

  if (a == UI_SP_DOWN || a == UI_SP_UP) {
    repeatAction = a;
    nextRepeat = now + 500;
  }
  return a;
}
