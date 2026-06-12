#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <string.h>

// ======================== [ SPRZĘT / PINOUT ] ========================
// TFT ILI9341 (SPI) — MISO niewykorzystywany (pin 19 użyty dla "W" taśmy RGBW)
#define TFT_CS   5
#define TFT_RST  21
#define TFT_DC   2
#define TFT_MOSI 23
#define TFT_CLK  18
Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

// Czujniki DS18B20: T1=heater (za grzałką), T2=box (wewnątrz komory)
#define ONE_WIRE_BUS 4
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
// >>> wstaw swoje adresy jeśli inne:
DeviceAddress sensor1 = {0x28, 0x8A, 0x1E, 0x90, 0x40, 0x24, 0x0B, 0xEF}; // T1
DeviceAddress sensor2 = {0x28, 0xDF, 0x96, 0x52, 0x40, 0x24, 0x0B, 0xE1}; // T2

// Wentylatory / grzałka (12 V, low-side MOSFET)
#define FAN_IN_PIN   25   // wlot, PWM
#define FAN_OUT_PIN  26   // wylot, PWM
#define HEATER_PIN   33   // grzałka, ON/OFF

// Taśma RGBW 12 V (4× MOSFET, PWM 25 kHz)
#define RGB_G_PIN 16
#define RGB_R_PIN 17
#define RGB_B_PIN 22
#define RGB_W_PIN 19

// Przyciski (INPUT_PULLUP) — legenda na ekranie: B1=^, B2=v, B3=<, B4=>
#define BTN_LEFT   27  // B3: <
#define BTN_RIGHT  14  // B4: >
#define BTN_UP     13  // B1: ^
#define BTN_DOWN   32  // B2: v

// ======================== [ STAŁE / USTAWIENIA ] =====================
const int HEADER_H = 32;            // wysokość paska zakładek
const int FOOTER_H = 24;            // wysokość stopki (legenda)
const int PWM_FREQ_FAN = 25000;     // 25 kHz — bez pisku
const int PWM_RES_8BIT = 8;         // PWM 0..255
const int PWM_CH_FAN_IN  = 0;
const int PWM_CH_FAN_OUT = 6;
const int RGB_FREQ = 25000;
const int RGB_RES  = 8;
const int CH_R = 1, CH_G = 2, CH_B = 3, CH_W = 4;

const uint16_t REPEAT_DELAY_MS = 350; // opóźnienie auto-powtarzania przytrzymania
const uint16_t REPEAT_RATE_MS  = 50;  // tempo auto-powtarzania
unsigned long debounceDelay = 180;    // debounce przycisków

// AUTO — histereza i boost
const float HYST_ON  = 0.3f;          // T2 <= SET - 0.3 -> grzej
const float HYST_OFF = 0.3f;          // T2 >= SET + 0.3 -> stop grzanie
const int   FAN_IN_MIN_ON = 40;       // przy grzaniu wymuszony przepływ
const uint16_t BOOST_LEVEL = 50;      // cykliczne "dopchnięcie" do 50%
const unsigned long BOOST_DUR_MS = 15000;  // przez 15 s
const unsigned long BOOST_PER_MS = 120000; // co 120 s

// Zabezpieczenie grzałki T1 (za grzałką)
const float HEATER_LIMIT_HI = 115.0f; // OFF powyżej 115 °C
const float HEATER_LIMIT_LO = 110.0f; // zezwolenie poniżej 110 °C

// ======================== [ ZMIENNE ROBOCZE ] ========================
float temp1 = 0, temp2 = 0;           // T1=heater, T2=box
unsigned long lastTempRead = 0;
const unsigned long tempInterval = 1000;

enum Page : uint8_t { PAGE_DASH = 0, PAGE_CTRL = 1, PAGE_LIGHT = 2, PAGE_AUTO = 3 };
Page currentPage = PAGE_DASH;
bool focusHeader = true; // true=na zakładkach, false=wewnątrz strony
uint8_t ctrlSel = 0, lightSel = 0, autoSel = 0;

int  fanIn_percent = 0, fanOut_percent = 0; // 0..100
bool heater_on_manual = false;        // manual w CTRL
bool autoEnabled = false;             // AUTO start wyłączone
bool heaterLimitActive = false;       // stan ograniczenia T1
bool heater_on_auto = false;          // aktualny stan grzania w AUTO
float setpointC = 40.0f;              // SET dla T2 (°C)
unsigned long lastBoostMs = 0, boostStartMs = 0;
bool boosting = false;

int hueDeg = 0, satPct = 100, valPct = 100, whitePct = 0; // LIGHT (HSV+W)

// Przyciski — triggery i auto-repeat
struct BtnRep { bool isDown = false; unsigned long downAt = 0; unsigned long lastRep = 0; };
BtnRep repL, repR, repU, repD;
static bool lastL = HIGH, lastR = HIGH, lastU = HIGH, lastD = HIGH;
unsigned long lastPL = 0, lastPR = 0, lastPU = 0, lastPD = 0;

// ======================== [ NARZĘDZIA / HELPERY ] ====================
static inline bool btnPressed(uint8_t pin) { return digitalRead(pin) == LOW; }
static inline int  pctToDuty8(int p) { if (p < 0) p = 0; if (p > 100) p = 100; return (p * 255) / 100; }
static inline int  clampi(int v, int lo, int hi) { if (v < lo) return lo; if (v > hi) return hi; return v; }

static inline void setFanInPercent(int p) {
  p = clampi(p, 0, 100); fanIn_percent = p;
  ledcWrite(PWM_CH_FAN_IN, pctToDuty8(p));
}
static inline void setFanOutPercent(int p) {
  p = clampi(p, 0, 100); fanOut_percent = p;
  ledcWrite(PWM_CH_FAN_OUT, pctToDuty8(p));
}
static inline void setHeaterOutput(bool on) { digitalWrite(HEATER_PIN, on ? HIGH : LOW); }

template<typename F>
inline void repeatIfHeld(BtnRep &rep, unsigned long now, F&& fn) {
  if (rep.isDown && (now - rep.downAt >= REPEAT_DELAY_MS) && (now - rep.lastRep >= REPEAT_RATE_MS)) {
    rep.lastRep = now; fn();
  }
}

// ======================== [ LIGHT: HSV->RGB + Paleta ] ===============
void hsv2rgb(float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
  while (h < 0) h += 360.f; while (h >= 360) h -= 360.f;
  float c = v * s, x = c * (1 - fabs(fmod(h / 60.f, 2) - 1)), m = v - c;
  float rf = 0, gf = 0, bf = 0;
  if      (h < 60)  { rf = c; gf = x; bf = 0; }
  else if (h < 120) { rf = x; gf = c; bf = 0; }
  else if (h < 180) { rf = 0; gf = c; bf = x; }
  else if (h < 240) { rf = 0; gf = x; bf = c; }
  else if (h < 300) { rf = x; gf = 0; bf = c; }
  else              { rf = c; gf = 0; bf = x; }
  r = (uint8_t)((rf + m) * 255 + 0.5f);
  g = (uint8_t)((gf + m) * 255 + 0.5f);
  b = (uint8_t)((bf + m) * 255 + 0.5f);
}

static inline void applyLight() {
  uint8_t r, g, b;
  hsv2rgb((float)hueDeg, satPct / 100.f, valPct / 100.f, r, g, b);
  ledcWrite(CH_R, r); ledcWrite(CH_G, g); ledcWrite(CH_B, b);
  ledcWrite(CH_W, pctToDuty8(whitePct));
}

// Paleta HUE×SAT renderowana do bufora i "blitowana" bez migotania
const int PAL_X = 10, PAL_Y = 40, PAL_W = 200, PAL_H = 100;
static uint16_t paletteBuf[PAL_W * PAL_H];
bool paletteDirty = true;
const int CROSS_HALF = 6;
int crossX = -1, crossY = -1;

inline int hueToX(int hue) { int x = PAL_X + (hue * (PAL_W - 1)) / 360; return clampi(x, PAL_X, PAL_X + PAL_W - 1); }
inline int satToY(int sat) { int y = PAL_Y + (PAL_H - 1) - (sat * (PAL_H - 1)) / 100; return clampi(y, PAL_Y, PAL_Y + PAL_H - 1); }

void renderPaletteToBuffer() {
  float v = valPct / 100.f;
  for (int y = 0; y < PAL_H; ++y) {
    float s = (float)(PAL_H - 1 - y) / (PAL_H - 1);
    for (int x = 0; x < PAL_W; ++x) {
      float h = (float)x * 360.f / (PAL_W - 1);
      uint8_t r, g, b; hsv2rgb(h, s, v, r, g, b);
      paletteBuf[y * PAL_W + x] = tft.color565(r, g, b);
    }
  }
}

void blitPalette() { tft.drawRGBBitmap(PAL_X, PAL_Y, paletteBuf, PAL_W, PAL_H); }

void restoreCrosshairUnder() {
  if (crossX < 0) return;
  int hx0 = max(PAL_X, crossX - CROSS_HALF), hx1 = min(PAL_X + PAL_W - 1, crossX + CROSS_HALF);
  if (crossY >= PAL_Y && crossY < PAL_Y + PAL_H && hx1 >= hx0) {
    const uint16_t* src = &paletteBuf[(crossY - PAL_Y) * PAL_W + (hx0 - PAL_X)];
    tft.drawRGBBitmap(hx0, crossY, src, hx1 - hx0 + 1, 1);
  }
  static uint16_t vbuf[CROSS_HALF * 2 + 1];
  int vy0 = max(PAL_Y, crossY - CROSS_HALF), vy1 = min(PAL_Y + PAL_H - 1, crossY + CROSS_HALF);
  if (crossX >= PAL_X && crossX < PAL_X + PAL_W && vy1 >= vy0) {
    for (int i = 0; i <= vy1 - vy0; i++) vbuf[i] = paletteBuf[(vy0 + i - PAL_Y) * PAL_W + (crossX - PAL_X)];
    tft.drawRGBBitmap(crossX, vy0, vbuf, 1, vy1 - vy0 + 1);
  }
}

void drawCrosshairAt(int cx, int cy) {
  int hx0 = max(PAL_X, cx - CROSS_HALF), hx1 = min(PAL_X + PAL_W - 1, cx + CROSS_HALF);
  if (cy >= PAL_Y && cy < PAL_Y + PAL_H && hx1 >= hx0) tft.drawFastHLine(hx0, cy, hx1 - hx0 + 1, ILI9341_WHITE);
  int vy0 = max(PAL_Y, cy - CROSS_HALF), vy1 = min(PAL_Y + PAL_H - 1, cy + CROSS_HALF);
  if (cx >= PAL_X && cx < PAL_X + PAL_W && vy1 >= vy0) tft.drawFastVLine(cx, vy0, vy1 - vy0 + 1, ILI9341_WHITE);
  crossX = cx; crossY = cy;
}

void moveCrosshair() {
  int nx = hueToX(hueDeg), ny = satToY(satPct);
  restoreCrosshairUnder();
  drawCrosshairAt(nx, ny);
}

// ======================== [ UI: TABY / STOPKA ] ======================
void drawTabs() {
  tft.fillRect(0, 0, 320, HEADER_H, ILI9341_BLACK);
  const char* names[4] = {"DASH", "CTRL", "LIGHT", "AUTO"};
  for (int i = 0; i < 4; i++) {
    int x0 = i * (320 / 4), w = (i == 3) ? (320 - x0) : (320 / 4);
    uint16_t frame = (i == (int)currentPage) ? (focusHeader ? ILI9341_YELLOW : ILI9341_CYAN) : ILI9341_DARKGREY;
    tft.drawRect(x0 + 1, 1, w - 2, HEADER_H - 2, frame);
    tft.setTextSize(2);
    tft.setTextColor(i == (int)currentPage ? frame : ILI9341_WHITE);
    int tw = 6 * strlen(names[i]) * 2;
    tft.setCursor(x0 + (w - tw) / 2, 8); tft.print(names[i]);
  }
}

void drawFooter() {
  tft.fillRect(0, 240 - FOOTER_H, 320, FOOTER_H, ILI9341_BLACK);
  tft.setTextSize(2); tft.setTextColor(ILI9341_DARKGREY);
  tft.setCursor(8, 240 - FOOTER_H + 4); tft.print("B1:^  B2:v  B3:<  B4:>");
}

// ======================== [ DASH – PODGLĄD ] =========================
const int DASH_XV = 170, DASH_WV = 140, DASH_Y0 = HEADER_H + 10;

void updateDashValues() {
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  int y = DASH_Y0;
  tft.fillRect(DASH_XV, y, DASH_WV, 18, ILI9341_BLACK); tft.setCursor(DASH_XV, y); tft.printf("%.1f C", temp1); y += 22; // T1
  tft.fillRect(DASH_XV, y, DASH_WV, 18, ILI9341_BLACK); tft.setCursor(DASH_XV, y); tft.printf("%.1f C", temp2); y += 28; // T2
  tft.fillRect(DASH_XV, y, DASH_WV, 18, ILI9341_BLACK); tft.setCursor(DASH_XV, y); tft.printf("%3d%%", fanIn_percent);  y += 22; // IN
  tft.fillRect(DASH_XV, y, DASH_WV, 18, ILI9341_BLACK); tft.setCursor(DASH_XV, y); tft.printf("%3d%%", fanOut_percent); y += 22; // OUT
  tft.fillRect(DASH_XV, y, DASH_WV, 18, ILI9341_BLACK); tft.setCursor(DASH_XV, y);
  if (autoEnabled) tft.print("AUTO"); else tft.print(heater_on_manual ? "ON" : "OFF");
}

void drawDash() {
  tft.fillRect(0, HEADER_H, 320, 240 - HEADER_H - FOOTER_H, ILI9341_BLACK);
  tft.setTextSize(2); tft.setTextColor(ILI9341_WHITE);
  int y = DASH_Y0;
  tft.setCursor(10, y); tft.print("T1 heat:"); y += 22;
  tft.setCursor(10, y); tft.print("T2 box :"); y += 28;
  tft.setCursor(10, y); tft.print("FAN IN :"); y += 22;
  tft.setCursor(10, y); tft.print("FAN OUT:"); y += 22;
  tft.setCursor(10, y); tft.print("HEATER :");
  updateDashValues();
}

// ======================== [ CTRL – RĘCZNIE ] =========================
void drawCtrl() {
  tft.fillRect(0, HEADER_H, 320, 240 - HEADER_H - FOOTER_H, ILI9341_BLACK);
  tft.setTextSize(2);
  auto row = [&](int idx, const char* name, const char* val) {
    int y = HEADER_H + 18 + idx * 28;
    bool sel = (!focusHeader && ctrlSel == idx);
    if (sel) tft.fillRect(6, y - 2, 308, 24, ILI9341_DARKGREY);
    tft.setCursor(10, y); tft.setTextColor(sel ? ILI9341_YELLOW : ILI9341_WHITE); tft.print(name);
    tft.setCursor(220, y); tft.print(val);
  };
  char v0[8]; snprintf(v0, sizeof(v0), "%3d%%", fanIn_percent);
  char v1[8]; snprintf(v1, sizeof(v1), "%3d%%", fanOut_percent);
  row(0, "FAN IN", v0);
  row(1, "FAN OUT", v1);
  row(2, "HEATER", autoEnabled ? "AUTO" : (heater_on_manual ? "ON" : "OFF"));
}

// ======================== [ LIGHT – RGBW ] ===========================
void drawLightPanel() {
  int x0 = PAL_X + PAL_W + 8;
  tft.fillRect(x0 - 4, PAL_Y - 4, 320 - x0 - 6, PAL_H + 8, ILI9341_BLACK);
  tft.drawRect(x0 - 4, PAL_Y - 4, 320 - x0 - 6, PAL_H + 8, ILI9341_DARKGREY);
  tft.setTextSize(2);
  auto row = [&](int idx, const char* name, int val, const char* unit) {
    int y = PAL_Y + 8 + idx * 22;
    bool sel = (!focusHeader && lightSel == idx);
    tft.setCursor(x0, y); tft.setTextColor(sel ? ILI9341_YELLOW : ILI9341_WHITE);
    tft.printf("%s:%3d%s", name, val, unit);
  };
  row(0, "HUE", hueDeg, "");
  row(1, "SAT", satPct, "%");
  row(2, "VAL", valPct, "%");
  row(3, "WHT", whitePct, "%");
}

void drawLight() {
  tft.fillRect(0, HEADER_H, 320, 240 - HEADER_H - FOOTER_H, ILI9341_BLACK);
  if (paletteDirty) {
    tft.fillRect(PAL_X - CROSS_HALF, PAL_Y - CROSS_HALF, PAL_W + 2 * CROSS_HALF, PAL_H + 2 * CROSS_HALF, ILI9341_BLACK);
    renderPaletteToBuffer(); blitPalette(); paletteDirty = false; crossX = -1; crossY = -1;
  } else blitPalette();
  // kursor
  int nx = hueToX(hueDeg), ny = satToY(satPct);
  if (crossX >= 0) restoreCrosshairUnder();
  drawCrosshairAt(nx, ny);
  drawLightPanel();
}

// ======================== [ AUTO – REGULACJA ] =======================
const int AUTO_LABEL_X = 10, AUTO_VAL_X = 110, AUTO_WV = 200;

void updateAutoValues() {
  tft.setTextSize(2);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  int y = HEADER_H + 18;
  tft.fillRect(AUTO_VAL_X, y, AUTO_WV, 18, ILI9341_BLACK); tft.setCursor(AUTO_VAL_X, y); tft.printf("%.1f C", setpointC); y += 28;
  tft.fillRect(AUTO_VAL_X, y, AUTO_WV, 18, ILI9341_BLACK); tft.setCursor(AUTO_VAL_X, y); tft.print(autoEnabled ? "ON" : "OFF");
  y = HEADER_H + 18 + 2 * 28 + 12;
  tft.fillRect(AUTO_VAL_X, y, AUTO_WV, 18, ILI9341_BLACK); tft.setCursor(AUTO_VAL_X, y); tft.printf("%.1f C%s", temp1, heaterLimitActive ? " (LIMIT)" : ""); y += 22;
  tft.fillRect(AUTO_VAL_X, y, AUTO_WV, 18, ILI9341_BLACK); tft.setCursor(AUTO_VAL_X, y); tft.printf("%.1f C", temp2); y += 22;
  tft.fillRect(AUTO_VAL_X, y, AUTO_WV, 18, ILI9341_BLACK); tft.setCursor(AUTO_VAL_X, y); tft.print(heater_on_auto ? "ON" : "OFF"); y += 22;
  tft.fillRect(AUTO_VAL_X, y, AUTO_WV, 18, ILI9341_BLACK); tft.setCursor(AUTO_VAL_X, y); tft.printf("IN:%3d  OUT:%3d", fanIn_percent, fanOut_percent);
}

void drawAuto() {
  tft.fillRect(0, HEADER_H, 320, 240 - HEADER_H - FOOTER_H, ILI9341_BLACK);
  tft.setTextSize(2);
  auto row = [&](int idx, const char* name) {
    int y = HEADER_H + 18 + idx * 28; bool sel = (!focusHeader && autoSel == idx);
    if (sel) tft.fillRect(6, y - 2, 308, 24, ILI9341_DARKGREY);
    tft.setCursor(AUTO_LABEL_X, y); tft.setTextColor(sel ? ILI9341_YELLOW : ILI9341_WHITE); tft.print(name);
  };
  row(0, "SET");
  row(1, "AUTO");
  int y = HEADER_H + 18 + 2 * 28 + 12;
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(AUTO_LABEL_X, y); tft.print("T1:");   y += 22;
  tft.setCursor(AUTO_LABEL_X, y); tft.print("T2:");   y += 22;
  tft.setCursor(AUTO_LABEL_X, y); tft.print("HEAT:"); y += 22;
  tft.setCursor(AUTO_LABEL_X, y); tft.print("FANS:");
  updateAutoValues();
}

// rampowanie procentów dla cichej pracy
static inline int rampTo(int current, int target, int step) {
  if (target > current) return current + min(step, target - current);
  if (target < current) return current - min(step, current - target);
  return current;
}

inline float T_heater() { return temp1; }
inline float T_box()    { return temp2; }

void autoControlUpdate() {
  // Limit T1 grzałki
  if (!heaterLimitActive && T_heater() > HEATER_LIMIT_HI) heaterLimitActive = true;
  if (heaterLimitActive  && T_heater() < HEATER_LIMIT_LO) heaterLimitActive = false;

  // Histereza na T2
  bool wantHeat = heater_on_auto;
  if (!heaterLimitActive) {
    if (!heater_on_auto && T_box() <= setpointC - HYST_ON)  wantHeat = true;
    if ( heater_on_auto && T_box() >= setpointC + HYST_OFF) wantHeat = false;
  } else wantHeat = false;
  heater_on_auto = wantHeat;
  setHeaterOutput(heater_on_auto);

  unsigned long now = millis();

  // FAN IN — przy grzaniu min. 40% + boost 50%/15s co 120s; przy chłodzeniu 30..50% (łagodnie)
  int fanInCmd = 0;
  if (heater_on_auto) {
    if (!boosting) {
      if (now - lastBoostMs >= BOOST_PER_MS) { boosting = true; boostStartMs = now; lastBoostMs = now; }
    } else if (now - boostStartMs >= BOOST_DUR_MS) { boosting = false; }
    fanInCmd = boosting ? BOOST_LEVEL : FAN_IN_MIN_ON;
  } else {
    float over = T_box() - setpointC;
    if (over > 0.2f) {
      int extra = (int)((over - 0.2f) * 10.0f); // +~10%/°C
      fanInCmd = clampi(30 + extra, 30, 50);
    } else fanInCmd = 0;
    boosting = false;
  }

  // FAN OUT — chłodzenie łagodne: 0.5..3.0 °C powyżej SET -> 25..70% (bez 100%)
  int fanOutCmd = 0;
  float over = T_box() - setpointC;
  if (over > 0.5f) {
    float o = max(0.5f, min(3.0f, over));
    float pct = 25.0f + (o - 0.5f) * ((70.0f - 25.0f) / (3.0f - 0.5f));
    fanOutCmd = (int)(pct + 0.5f);
  }

  // Zastosuj z rampą (~8–10%/s)
  setFanInPercent (rampTo(fanIn_percent,  fanInCmd,  7));
  setFanOutPercent(rampTo(fanOut_percent, fanOutCmd, 8));
}

// ======================== [ STEROWANIE – AKCJE ] =====================
inline void light_hue_dec() { hueDeg = (hueDeg + 360 - 10) % 360; applyLight(); moveCrosshair(); drawLightPanel(); }
inline void light_hue_inc() { hueDeg = (hueDeg + 10) % 360;       applyLight(); moveCrosshair(); drawLightPanel(); }
inline void light_sat_dec() { if (satPct > 0)   { satPct -= 5; if (satPct < 0)   satPct = 0;   applyLight(); moveCrosshair(); drawLightPanel(); } }
inline void light_sat_inc() { if (satPct < 100) { satPct += 5; if (satPct > 100) satPct = 100; applyLight(); moveCrosshair(); drawLightPanel(); } }
inline void light_val_dec() { if (valPct > 0)   { valPct -= 5; if (valPct < 0)   valPct = 0;   paletteDirty = true; applyLight(); drawLight(); } }
inline void light_val_inc() { if (valPct < 100) { valPct += 5; if (valPct > 100) valPct = 100; paletteDirty = true; applyLight(); drawLight(); } }
inline void light_wht_dec() { if (whitePct > 0)   { whitePct -= 5; if (whitePct < 0)   whitePct = 0;   applyLight(); drawLightPanel(); } }
inline void light_wht_inc() { if (whitePct < 100) { whitePct += 5; if (whitePct > 100) whitePct = 100; applyLight(); drawLightPanel(); } }

inline void ctrl_fanIn_dec()  { int np = fanIn_percent  - 10; if (np < 0)   np = 100; setFanInPercent(np);  drawCtrl(); }
inline void ctrl_fanIn_inc()  { int np = fanIn_percent  + 10; if (np > 100) np = 0;   setFanInPercent(np);  drawCtrl(); }
inline void ctrl_fanOut_dec() { int np = fanOut_percent - 10; if (np < 0)   np = 100; setFanOutPercent(np); drawCtrl(); }
inline void ctrl_fanOut_inc() { int np = fanOut_percent + 10; if (np > 100) np = 0;   setFanOutPercent(np); drawCtrl(); }

// ======================== [ SETUP / LOOP ] ===========================
void drawPage() { // pełny odrys strony
  drawTabs();
  switch (currentPage) {
    case PAGE_DASH:  drawDash();  break;
    case PAGE_CTRL:  drawCtrl();  break;
    case PAGE_LIGHT: drawLight(); break;
    case PAGE_AUTO:  drawAuto();  break;
  }
  drawFooter();
}

void setup() {
  Serial.begin(115200);

  // MISO=-1, bo pin 19 używamy na kanał "W" taśmy
  SPI.begin(TFT_CLK, -1, TFT_MOSI, TFT_CS);
  tft.begin(); tft.setRotation(1); tft.fillScreen(ILI9341_BLACK);

  sensors.begin();

  pinMode(BTN_LEFT,  INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);
  pinMode(BTN_UP,    INPUT_PULLUP);
  pinMode(BTN_DOWN,  INPUT_PULLUP);

  ledcSetup(PWM_CH_FAN_IN,  PWM_FREQ_FAN, PWM_RES_8BIT);
  ledcSetup(PWM_CH_FAN_OUT, PWM_FREQ_FAN, PWM_RES_8BIT);
  ledcAttachPin(FAN_IN_PIN,  PWM_CH_FAN_IN);
  ledcAttachPin(FAN_OUT_PIN, PWM_CH_FAN_OUT);
  setFanInPercent(0); setFanOutPercent(0);

  pinMode(HEATER_PIN, OUTPUT);
  setHeaterOutput(false); // grzałka OFF na starcie

  ledcSetup(CH_R, RGB_FREQ, RGB_RES);
  ledcSetup(CH_G, RGB_FREQ, RGB_RES);
  ledcSetup(CH_B, RGB_FREQ, RGB_RES);
  ledcSetup(CH_W, RGB_FREQ, RGB_RES);
  ledcAttachPin(RGB_R_PIN, CH_R);
  ledcAttachPin(RGB_G_PIN, CH_G);
  ledcAttachPin(RGB_B_PIN, CH_B);
  ledcAttachPin(RGB_W_PIN, CH_W);
  applyLight();

  drawPage();
}

void loop() {
  unsigned long now = millis();

  // Odczyt temperatur co 1 s + ewentualna aktualizacja ekranów z wartościami
  if (now - lastTempRead > tempInterval) {
    lastTempRead = now;
    sensors.requestTemperatures();
    temp1 = sensors.getTempC(sensor1); // T1 heater
    temp2 = sensors.getTempC(sensor2); // T2 box
    if (autoEnabled) autoControlUpdate();
    if (currentPage == PAGE_DASH) updateDashValues();
    if (currentPage == PAGE_AUTO) updateAutoValues();
  }

  // Przyciski z debounce + rejestr przytrzymania
  auto onPress = [&](bool &last, unsigned long &lastP, BtnRep &rep, bool b) {
    if (b != last && (now - lastP > debounceDelay)) {
      lastP = now; last = b;
      if (b) { rep.isDown = true; rep.downAt = now; rep.lastRep = now; return true; }
      else   { rep.isDown = false; }
    }
    return false;
  };

  bool bL = btnPressed(BTN_LEFT), bR = btnPressed(BTN_RIGHT), bU = btnPressed(BTN_UP), bD = btnPressed(BTN_DOWN);
  bool trigL = onPress(lastL, lastPL, repL, bL);
  bool trigR = onPress(lastR, lastPR, repR, bR);
  bool trigU = onPress(lastU, lastPU, repU, bU);
  bool trigD = onPress(lastD, lastPD, repD, bD);

  // Nawigacja: najpierw zakładki
  if (focusHeader) {
    if (trigD && bD) { // wejdź w treść, ale DASH jest tylko podglądem
      if (currentPage != PAGE_DASH) {
        focusHeader = false;
        if (currentPage == PAGE_CTRL)  ctrlSel  = 0;
        if (currentPage == PAGE_LIGHT) lightSel = 0;
        if (currentPage == PAGE_AUTO)  autoSel  = 0;
        drawPage();
      }
    }
    if (trigL && bL) { currentPage = (Page)((currentPage + 3) % 4); if (currentPage == PAGE_LIGHT) paletteDirty = true; drawPage(); }
    if (trigR && bR) { currentPage = (Page)((currentPage + 1) % 4); if (currentPage == PAGE_LIGHT) paletteDirty = true; drawPage(); }
  } else {
    // W treści: ^ wyżej (na górze wraca do zakładek)
    if (trigU && bU) {
      if (currentPage == PAGE_CTRL) {
        if (ctrlSel > 0) { ctrlSel--; drawCtrl(); drawFooter(); }
        else { focusHeader = true; drawTabs(); drawFooter(); }
      } else if (currentPage == PAGE_LIGHT) {
        if (lightSel > 0) { lightSel--; drawLightPanel(); drawFooter(); }
        else { focusHeader = true; drawTabs(); drawFooter(); }
      } else if (currentPage == PAGE_AUTO) {
        if (autoSel > 0) { autoSel--; drawAuto(); drawFooter(); }
        else { focusHeader = true; drawTabs(); drawFooter(); }
      }
    }
    // v niżej (bez zawijania)
    if (trigD && bD) {
      if (currentPage == PAGE_CTRL)  { if (ctrlSel  < 2) { ctrlSel++;  drawCtrl();       drawFooter(); } }
      if (currentPage == PAGE_LIGHT) { if (lightSel < 3) { lightSel++; drawLightPanel(); drawFooter(); } }
      if (currentPage == PAGE_AUTO)  { if (autoSel  < 1) { autoSel++;  drawAuto();       drawFooter(); } }
    }
    // </> — modyfikacje + auto-repeat
    if (currentPage == PAGE_CTRL) {
      auto disableAuto = [&]() {
        if (autoEnabled) { autoEnabled = false; heater_on_auto = false; setHeaterOutput(heater_on_manual); }
      };
      if (trigL && bL) { disableAuto();
        if      (ctrlSel == 0) ctrl_fanIn_dec();
        else if (ctrlSel == 1) ctrl_fanOut_dec();
        else if (ctrlSel == 2) { heater_on_manual = !heater_on_manual; setHeaterOutput(heater_on_manual); drawCtrl(); }
      }
      if (trigR && bR) { disableAuto();
        if      (ctrlSel == 0) ctrl_fanIn_inc();
        else if (ctrlSel == 1) ctrl_fanOut_inc();
        else if (ctrlSel == 2) { heater_on_manual = !heater_on_manual; setHeaterOutput(heater_on_manual); drawCtrl(); }
      }
      if (ctrlSel == 0) { repeatIfHeld(repL, now, []() { ctrl_fanIn_dec(); });  repeatIfHeld(repR, now, []() { ctrl_fanIn_inc(); }); }
      if (ctrlSel == 1) { repeatIfHeld(repL, now, []() { ctrl_fanOut_dec(); }); repeatIfHeld(repR, now, []() { ctrl_fanOut_inc(); }); }
    }
    else if (currentPage == PAGE_LIGHT) {
      auto leftAct  = [&]() { if (lightSel == 0) light_hue_dec(); else if (lightSel == 1) light_sat_dec(); else if (lightSel == 2) light_val_dec(); else light_wht_dec(); };
      auto rightAct = [&]() { if (lightSel == 0) light_hue_inc(); else if (lightSel == 1) light_sat_inc(); else if (lightSel == 2) light_val_inc(); else light_wht_inc(); };
      if (trigL && bL) leftAct(); if (trigR && bR) rightAct();
      repeatIfHeld(repL, now, [&]() { leftAct();  });
      repeatIfHeld(repR, now, [&]() { rightAct(); });
    }
    else if (currentPage == PAGE_AUTO) {
      if (trigL && bL) {
        if (autoSel == 0) { setpointC -= 0.5f; if (setpointC < 10.0f) setpointC = 10.0f; updateAutoValues(); }
        else if (autoSel == 1) {
          autoEnabled = !autoEnabled;
          if (!autoEnabled) { heater_on_auto = false; setHeaterOutput(heater_on_manual); setFanInPercent(fanIn_percent); setFanOutPercent(fanOut_percent); }
          updateAutoValues();
        }
      }
      if (trigR && bR) {
        if (autoSel == 0) { setpointC += 0.5f; if (setpointC > 80.0f) setpointC = 80.0f; updateAutoValues(); }
        else if (autoSel == 1) {
          autoEnabled = !autoEnabled;
          if (!autoEnabled) { heater_on_auto = false; setHeaterOutput(heater_on_manual); setFanInPercent(fanIn_percent); setFanOutPercent(fanOut_percent); }
          updateAutoValues();
        }
      }
      if (autoSel == 0) {
        repeatIfHeld(repL, now, [&]() { setpointC -= 0.5f; if (setpointC < 10.0f) setpointC = 10.0f; updateAutoValues(); });
        repeatIfHeld(repR, now, [&]() { setpointC += 0.5f; if (setpointC > 80.0f) setpointC = 80.0f; updateAutoValues(); });
      }
    }
  }

  delay(10);
}
