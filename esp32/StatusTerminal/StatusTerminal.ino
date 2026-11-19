// Ruben's Status Terminal for the ESP32 "Cheap Yellow Display" (ESP32-2432S028R)
// Fully offline. Tap a button to set HERE / AWAY / BUSY / LUNCH.
// The last status is saved to flash, so it comes back after a power cycle.
//
// Libraries (Arduino Library Manager):
//   - TFT_eSPI (configured for the CYD, see README)
//   - XPT2046_Touchscreen by Paul Stoffregen

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>

// ---------- CYD pins ----------
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define BACKLIGHT    21

// Raw touch range. If taps land on the wrong button, adjust these (see README).
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft;
Preferences prefs;

constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t C_BG     = RGB(5, 8, 6);
const uint16_t C_FRAME  = RGB(24, 48, 30);
const uint16_t C_DIM    = RGB(60, 120, 75);
const uint16_t C_TEXT   = RGB(190, 255, 205);

struct Status {
  const char* label;   // big text + button label
  const char* line1;   // terminal line (max 24 chars at size 2)
  const char* line2;   // smaller detail line (max 50 chars at size 1)
  uint16_t color;
};

const Status STATUSES[] = {
  { "HERE",  "> system online",     "accepting requests. ping away.",           RGB(57, 255, 120)  },
  { "AWAY",  "> host unreachable",  "request timed out. please file a ticket.", RGB(255, 85, 85)   },
  { "BUSY",  "> process running",   "high CPU load. interrupt only if on fire.", RGB(255, 190, 40)  },
  { "LUNCH", "> afk: lunch.exe",    "refueling. will reconnect shortly.",        RGB(60, 210, 255)  },
};
const int NUM = sizeof(STATUSES) / sizeof(STATUSES[0]);

// Button row along the bottom
const int BTN_Y = 176, BTN_H = 56, BTN_W = 74, BTN_GAP = 4, BTN_X0 = 4;

// Big status text position
const int BIG_X = 16, BIG_Y = 52, BIG_SIZE = 6;   // GLCD font: 6x8 px per char * size

int currentIdx = 0;
unsigned long setAt = 0;
unsigned long lastTick = 0;
bool cursorOn = true;

// ---------- Drawing ----------

void drawFrame() {
  tft.fillScreen(C_BG);
  tft.drawRect(0, 0, 320, 240, C_FRAME);
  tft.drawFastHLine(0, 18, 320, C_FRAME);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(TL_DATUM);
  tft.drawString("ruben@it-dept:~$ ./status", 6, 6);
}

int cursorX() {
  return BIG_X + strlen(STATUSES[currentIdx].label) * 6 * BIG_SIZE + 6;
}

void drawCursor(bool on) {
  const Status& s = STATUSES[currentIdx];
  tft.fillRect(cursorX(), BIG_Y, 3 * BIG_SIZE, 8 * BIG_SIZE - 6, on ? s.color : C_BG);
}

void drawElapsed() {
  unsigned long secs = (millis() - setAt) / 1000;
  char buf[24];
  snprintf(buf, sizeof(buf), "for %02lu:%02lu:%02lu", secs / 3600, (secs / 60) % 60, secs % 60);
  tft.setTextFont(1);
  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(buf, 314, 6);
  tft.setTextDatum(TL_DATUM);
}

void drawStatus() {
  const Status& s = STATUSES[currentIdx];
  tft.fillRect(1, 19, 318, 152, C_BG);

  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);

  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString("STATUS", BIG_X, 32);

  tft.setTextSize(BIG_SIZE);
  tft.setTextColor(s.color, C_BG);
  tft.drawString(s.label, BIG_X, BIG_Y);

  tft.setTextSize(2);
  tft.setTextColor(C_TEXT, C_BG);
  tft.drawString(s.line1, BIG_X, 116);

  tft.setTextSize(1);
  tft.setTextColor(C_DIM, C_BG);
  tft.drawString(s.line2, BIG_X, 140);

  tft.drawFastHLine(1, 166, 318, C_FRAME);
  drawCursor(true);
  drawElapsed();
}

void drawButtons() {
  for (int i = 0; i < NUM; i++) {
    const Status& s = STATUSES[i];
    int x = BTN_X0 + i * (BTN_W + BTN_GAP);
    bool active = i == currentIdx;
    tft.fillRect(x, BTN_Y, BTN_W, BTN_H, active ? s.color : C_BG);
    tft.drawRect(x, BTN_Y, BTN_W, BTN_H, s.color);
    tft.setTextFont(1);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(active ? C_BG : s.color, active ? s.color : C_BG);
    tft.drawString(s.label, x + BTN_W / 2, BTN_Y + BTN_H / 2);
  }
  tft.setTextDatum(TL_DATUM);
}

// ---------- Touch ----------

int buttonAt(int x, int y) {
  if (y < BTN_Y || y >= BTN_Y + BTN_H) return -1;
  for (int i = 0; i < NUM; i++) {
    int bx = BTN_X0 + i * (BTN_W + BTN_GAP);
    if (x >= bx && x < bx + BTN_W) return i;
  }
  return -1;
}

void setStatus(int i) {
  currentIdx = i;
  setAt = millis();
  prefs.putUChar("idx", i);
  drawStatus();
  drawButtons();
}

void handleTouch() {
  if (!ts.tirqTouched() || !ts.touched()) return;
  TS_Point p = ts.getPoint();
  int x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
  int y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
  // Serial.printf("raw %d,%d -> %d,%d\n", p.x, p.y, x, y);  // uncomment to calibrate
  while (ts.touched()) delay(10);  // wait for release

  int i = buttonAt(x, y);
  if (i >= 0 && i != currentIdx) setStatus(i);
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, HIGH);

  tft.init();
  tft.setRotation(1);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  prefs.begin("status", false);
  currentIdx = prefs.getUChar("idx", 0);
  if (currentIdx >= NUM) currentIdx = 0;
  setAt = millis();

  drawFrame();
  drawStatus();
  drawButtons();
}

void loop() {
  handleTouch();
  if (millis() - lastTick >= 500) {
    lastTick = millis();
    cursorOn = !cursorOn;
    drawCursor(cursorOn);
    drawElapsed();
  }
  delay(10);
}
