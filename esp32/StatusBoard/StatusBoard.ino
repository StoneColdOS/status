// Ruben's Status Board for the ESP32 "Cheap Yellow Display" (ESP32-2432S028R)
// Shows the current status from Firebase and lets you change it with on-screen buttons.
//
// Libraries (Arduino Library Manager):
//   - TFT_eSPI (configured for the CYD, see README)
//   - XPT2046_Touchscreen by Paul Stoffregen
//   - ArduinoJson by Benoit Blanchon (v7)

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include "secrets.h"

// ---------- CYD touch pins ----------
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define BACKLIGHT    21

// Raw touch range. If taps land in the wrong spot, adjust these (see README).
#define TOUCH_X_MIN 200
#define TOUCH_X_MAX 3700
#define TOUCH_Y_MIN 240
#define TOUCH_Y_MAX 3800

#define POLL_MS 3000

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft;

constexpr uint16_t RGB(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

const uint16_t C_BG    = RGB(13, 12, 18);
const uint16_t C_PANEL = RGB(21, 20, 28);
const uint16_t C_LINE  = RGB(38, 36, 47);
const uint16_t C_TEXT  = RGB(242, 240, 247);
const uint16_t C_MUTED = RGB(142, 138, 156);

struct Preset {
  const char* key;
  const char* chip;
  const char* label;     // short button label
  const char* headline;
  const char* note;
  uint16_t color;
};

// Keep these in sync with PRESETS in index.html
const Preset PRESETS[] = {
  { "in",      "Available",      "In",      "Ruben Is In",    "Ask away. Yes, I've already tried turning it off and on again.", RGB(74, 222, 128) },
  { "focus",   "Focused",        "Focus",   "Heads Down",     "Deep in a project. Emergencies only, please.",                   RGB(167, 139, 250) },
  { "meeting", "In a meeting",   "Meeting", "In a Meeting",   "Probably one that could have been an email.",                    RGB(96, 165, 250) },
  { "lunch",   "At lunch",       "Lunch",   "Out to Lunch",   "Refueling. Back soon.",                                          RGB(251, 191, 36) },
  { "dnd",     "Do not disturb", "DND",     "Do Not Disturb", "Unless something is actually on fire.",                          RGB(248, 113, 113) },
  { "out",     "Away",           "Out",     "Ruben Is Out",   "Submit a ticket and it'll be waiting for me.",                   RGB(248, 113, 113) },
};
const int NUM_PRESETS = sizeof(PRESETS) / sizeof(PRESETS[0]);

// Button grid: 3 columns x 2 rows along the bottom
const int BTN_W = 100, BTN_H = 46, BTN_Y0 = 138, BTN_GAP = 6, BTN_X0 = 5;

struct Status { String key, headline, note, back; };
Status current;
String lastPayload;
String idToken;
unsigned long tokenExpiresAt = 0;
unsigned long lastPoll = 0;

const Preset* findPreset(const String& key) {
  for (int i = 0; i < NUM_PRESETS; i++) if (key == PRESETS[i].key) return &PRESETS[i];
  return &PRESETS[0];
}

// ---------- Drawing ----------

// Word-wraps text into a box. Returns the y below the last line drawn.
int drawWrapped(const String& text, int x, int y, int maxW, uint8_t font, int lineH, int maxLines, uint16_t color) {
  tft.setTextFont(font);
  tft.setTextColor(color, C_PANEL);
  tft.setTextDatum(TL_DATUM);
  String line, word;
  int lines = 0;
  String s = text + " ";
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c != ' ') { word += c; continue; }
    String trial = line.length() ? line + " " + word : word;
    if (tft.textWidth(trial) <= maxW) {
      line = trial;
    } else {
      if (lines == maxLines - 1) { line += "..."; break; }
      tft.drawString(line, x, y);
      y += lineH; lines++;
      line = word;
    }
    word = "";
  }
  if (line.length() && lines < maxLines) { tft.drawString(line, x, y); y += lineH; }
  return y;
}

void drawStatus() {
  const Preset* p = findPreset(current.key);
  tft.fillRect(0, 0, 320, 132, C_PANEL);
  tft.fillRect(0, 0, 5, 132, p->color);

  // Chip
  tft.fillCircle(17, 15, 4, p->color);
  tft.setTextFont(2);
  tft.setTextColor(p->color, C_PANEL);
  tft.setTextDatum(TL_DATUM);
  tft.drawString(p->chip, 27, 7);

  int y = drawWrapped(current.headline.length() ? current.headline : String(p->headline), 14, 30, 296, 4, 26, 2, C_TEXT);
  y = drawWrapped(current.note, 14, y + 4, 296, 2, 16, 2, C_MUTED);

  if (current.back.length()) {
    tft.setTextFont(2);
    tft.setTextColor(C_TEXT, C_PANEL);
    tft.drawString("Back by " + current.back, 14, 112);
  }
  tft.drawFastHLine(0, 132, 320, C_LINE);
}

void drawButtons() {
  tft.fillRect(0, 133, 320, 107, C_BG);
  for (int i = 0; i < NUM_PRESETS; i++) {
    int col = i % 3, row = i / 3;
    int x = BTN_X0 + col * (BTN_W + BTN_GAP);
    int y = BTN_Y0 + row * (BTN_H + BTN_GAP);
    bool active = current.key == PRESETS[i].key;
    tft.fillRoundRect(x, y, BTN_W, BTN_H, 8, active ? PRESETS[i].color : C_PANEL);
    tft.drawRoundRect(x, y, BTN_W, BTN_H, 8, active ? PRESETS[i].color : C_LINE);
    if (!active) tft.fillCircle(x + 14, y + BTN_H / 2, 4, PRESETS[i].color);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(active ? C_BG : C_TEXT, active ? PRESETS[i].color : C_PANEL);
    tft.drawString(PRESETS[i].label, x + BTN_W / 2 + (active ? 0 : 6), y + BTN_H / 2);
  }
  tft.setTextDatum(TL_DATUM);
}

void drawMessage(const String& msg, uint16_t color) {
  tft.fillRect(0, 0, 320, 132, C_PANEL);
  tft.setTextFont(2);
  tft.setTextColor(color, C_PANEL);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(msg, 160, 66);
  tft.setTextDatum(TL_DATUM);
}

void drawAll() { drawStatus(); drawButtons(); }

// ---------- Network ----------

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  drawMessage("Connecting to Wi-Fi...", C_MUTED);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  if (WiFi.status() != WL_CONNECTED) drawMessage("Wi-Fi failed. Retrying...", RGB(248, 113, 113));
}

bool signIn() {
  WiFiClientSecure client;
  client.setInsecure(); // Skips certificate checks. Fine for a status board, see README.
  HTTPClient http;
  String url = "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=" FIREBASE_API_KEY;
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");

  JsonDocument req;
  req["email"] = FIREBASE_EMAIL;
  req["password"] = FIREBASE_PASSWORD;
  req["returnSecureToken"] = true;
  String body;
  serializeJson(req, body);

  int code = http.POST(body);
  bool ok = false;
  if (code == 200) {
    JsonDocument res;
    if (!deserializeJson(res, http.getString())) {
      idToken = res["idToken"].as<String>();
      long expiresIn = res["expiresIn"].as<String>().toInt(); // usually 3600
      tokenExpiresAt = millis() + (unsigned long)(expiresIn - 300) * 1000UL;
      ok = idToken.length() > 0;
    }
  } else {
    Serial.printf("Sign-in failed: HTTP %d\n", code);
  }
  http.end();
  return ok;
}

bool ensureToken() {
  if (idToken.length() && (long)(tokenExpiresAt - millis()) > 0) return true;
  return signIn();
}

void applyPayload(const String& payload) {
  JsonDocument doc;
  if (deserializeJson(doc, payload) || doc.isNull()) {
    current = { "in", PRESETS[0].headline, PRESETS[0].note, "" };
  } else {
    current.key = doc["key"] | "in";
    current.headline = doc["headline"] | "";
    current.note = doc["note"] | "";
    current.back = doc["back"] | "";
  }
}

void poll() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, String(DATABASE_URL) + "/status.json")) return;
  int code = http.GET();
  if (code == 200) {
    String payload = http.getString();
    if (payload != lastPayload) {
      lastPayload = payload;
      applyPayload(payload);
      drawAll();
    }
  } else {
    Serial.printf("Poll failed: HTTP %d\n", code);
  }
  http.end();
}

bool putStatus(const Preset& p) {
  if (!ensureToken()) return false;

  JsonDocument doc;
  doc["key"] = p.key;
  doc["headline"] = p.headline;
  doc["note"] = p.note;
  doc["back"] = "";
  doc["updatedAt"][".sv"] = "timestamp";
  String body;
  serializeJson(doc, body);

  for (int attempt = 0; attempt < 2; attempt++) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    if (!http.begin(client, String(DATABASE_URL) + "/status.json?auth=" + idToken)) return false;
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(body);
    http.end();
    if (code == 200) return true;
    if (code == 401 && attempt == 0 && signIn()) continue; // token expired, retry once
    Serial.printf("Save failed: HTTP %d\n", code);
    return false;
  }
  return false;
}

// ---------- Touch ----------

int buttonAt(int x, int y) {
  for (int i = 0; i < NUM_PRESETS; i++) {
    int bx = BTN_X0 + (i % 3) * (BTN_W + BTN_GAP);
    int by = BTN_Y0 + (i / 3) * (BTN_H + BTN_GAP);
    if (x >= bx && x < bx + BTN_W && y >= by && y < by + BTN_H) return i;
  }
  return -1;
}

void handleTouch() {
  if (!ts.tirqTouched() || !ts.touched()) return;
  TS_Point p = ts.getPoint();
  int x = constrain(map(p.x, TOUCH_X_MIN, TOUCH_X_MAX, 0, 320), 0, 319);
  int y = constrain(map(p.y, TOUCH_Y_MIN, TOUCH_Y_MAX, 0, 240), 0, 239);
  while (ts.touched()) delay(10); // wait for release

  int i = buttonAt(x, y);
  if (i < 0 || current.key == PRESETS[i].key) return;

  Status previous = current;
  current = { PRESETS[i].key, PRESETS[i].headline, PRESETS[i].note, "" };
  drawAll(); // show it right away

  if (putStatus(PRESETS[i])) {
    lastPoll = 0; // refresh from the database on the next loop
  } else {
    current = previous;
    drawMessage("Couldn't save. Check Wi-Fi/login.", RGB(248, 113, 113));
    delay(1500);
    drawAll();
  }
}

// ---------- Main ----------

void setup() {
  Serial.begin(115200);
  pinMode(BACKLIGHT, OUTPUT);
  digitalWrite(BACKLIGHT, HIGH);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(C_BG);

  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  current = { "in", PRESETS[0].headline, PRESETS[0].note, "" };
  connectWiFi();
  drawAll();
  if (!signIn()) Serial.println("Initial sign-in failed. Will retry when you tap a button.");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) { connectWiFi(); drawAll(); return; }
  handleTouch();
  if (millis() - lastPoll > POLL_MS || lastPoll == 0) {
    lastPoll = millis();
    poll();
  }
  delay(20);
}
