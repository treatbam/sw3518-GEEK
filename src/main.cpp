#include <Arduino.h>
#include <esp_system.h>
#include <OneButton.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>
#include <string.h>

#include "pins.h"
#include "sw3518.h"
#include "secrets.h"
#include "geek_display.h"
#include "radio_tools.h"

enum class Page : uint8_t { Main = 0, UsbC = 1, UsbA = 2, History = 3 };
enum class Mode : uint8_t { Charger = 0, Radio = 1 };
enum class RadioPage : uint8_t { WifiScan = 0, ChannelHeat = 1, Sys = 2, Help = 3, Count = 4 };

static constexpr size_t kHistMax = 120;
static constexpr uint32_t kUiMs = 200;
static constexpr uint32_t kNightIdleMs = 90000;  // dim after 90s
static constexpr uint32_t kMqttMs = 5000;
static constexpr uint32_t kSdLogMs = 1000;
static constexpr uint32_t kAnimMs = 320;
static constexpr float kLoadMa = 50.0f;
static constexpr int kBlFull = 255;
static constexpr int kBlDim = 128;  // 50%

GeekDisplay tft;
GFXcanvas16 frame(240, 135);
HardwareSerial UartDbg(0);

static void logLine(const char* msg) {
  Serial.println(msg);
  Serial.flush();
  UartDbg.println(msg);
  UartDbg.flush();
}

SW3518 charger;
OneButton bootBtn(PIN_BOOT_BTN, true, true);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
WebServer web(80);
SPIClass* sdSpi = nullptr;
uint32_t lastWebHitMs = 0;
uint32_t ipShowUntilMs = 0;  // show IP near icons until this time
bool seenUsbC = false;
bool seenUsbA = false;
static constexpr uint32_t kWebActiveMs = 8000;
bool webStarted = false;

bool nightDim = false;
int blLevel = kBlFull;
Page page = Page::Main;
Mode mode = Mode::Charger;
RadioPage radioPage = RadioPage::WifiScan;
uint8_t nextFromMain = 0;  // 0=UsbC, 1=UsbA, 2=History
uint32_t modeToastUntil = 0;
char modeToast[16] = "";
uint32_t lastUiMs = 0;
uint32_t lastProbeMs = 0;
uint32_t lastActivityMs = 0;
uint32_t lastMqttMs = 0;
uint32_t lastSdMs = 0;
uint32_t lastBeatMs = 0;
uint32_t protoFlashUntil = 0;
SW3518::Protocol lastProtocol = SW3518::Protocol::None;
SW3518::Snapshot snap;

float histC[kHistMax] = {};
float histA[kHistMax] = {};
size_t histCount = 0;
uint32_t histPeriodMs = 250;

struct Session {
  bool active = false;       // load present right now (debounced off)
  uint32_t startMs = 0;      // first load this session; 0 = empty
  uint32_t lastSampleMs = 0;
  uint32_t chargedMs = 0;    // time with load only (for avg W / duration)
  double mwh = 0;
  float peakW = 0;
  float peakA = 0;
  float peakC_A = 0;
  float peakA_A = 0;
  float peakC_W = 0;
  float peakA_W = 0;
  float peakC_W_A = 0;  // amps at C peak watts
  float peakA_W_A = 0;  // amps at A peak watts
  uint16_t peakVoutMv = 0;
} session;

static constexpr uint32_t kSessionEndDebounceMs = 1500;

static float sessionAvgW() {
  if (session.chargedMs < 50 || session.mwh <= 0) return 0.f;
  // avg W = mWh / hours = mWh * 3600 / ms
  return static_cast<float>(session.mwh * 3600.0 / session.chargedMs);
}

struct Anim {
  enum Kind : uint8_t { Idle = 0, ZoomIn = 1, ZoomOut = 2 } kind = Idle;
  Page from = Page::Main;
  Page to = Page::Main;
  uint32_t startMs = 0;

  float rawT(uint32_t now) const {
    if (kind == Idle) return 1.f;
    float t = (now - startMs) / (float)kAnimMs;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return t;
  }
  static float ease(float t) { return t * t * (3.f - 2.f * t); }
  bool busy() const { return kind != Idle; }
} anim;

bool wifiEnabled = false;
bool sdOk = false;

#if defined(WIFI_SSID)
#define HAS_WIFI 1
#else
#define HAS_WIFI 0
#endif

struct Rect {
  float x, y, w, h;
};

static Rect lerpRect(const Rect& a, const Rect& b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t,
          a.h + (b.h - a.h) * t};
}

static Rect sparkMiniC() { return {4, 106, 110, 26}; }
static Rect sparkMiniA() { return {126, 106, 110, 26}; }
static Rect sparkPort() { return {4, 72, 232, 58}; }

static void applyBacklight() {
  if (blLevel >= kBlFull) digitalWrite(PIN_TFT_BL, HIGH);
  else analogWrite(PIN_TFT_BL, blLevel);
}

static void touchActivity() {
  lastActivityMs = millis();
  if (nightDim) {
    nightDim = false;
    blLevel = kBlFull;
    applyBacklight();
  }
}

static void clearSession() {
  session = Session{};
  histCount = 0;
  histPeriodMs = 250;
  memset(histC, 0, sizeof(histC));
  memset(histA, 0, sizeof(histA));
  Serial.println("Session cleared");
}

static void updateSession(uint32_t now) {
  const bool load = (snap.ia_ma > kLoadMa) || (snap.ic_ma > kLoadMa);
  static uint32_t loadGoneSince = 0;

  if (load) {
    loadGoneSince = 0;
    if (session.startMs == 0) {
      // Fresh session (after boot or long-hold clear)
      session.startMs = now;
      session.lastSampleMs = now;
      session.chargedMs = 0;
      session.mwh = 0;
      session.peakW = session.peakA = session.peakC_A = session.peakA_A = 0;
      session.peakC_W = session.peakA_W = 0;
      session.peakC_W_A = session.peakA_W_A = 0;
      session.peakVoutMv = 0;
      histCount = 0;
      histPeriodMs = 250;
    } else if (!session.active) {
      // Resume after pause — skip idle gap for energy / chargedMs
      session.lastSampleMs = now;
    } else {
      const uint32_t dt = now - session.lastSampleMs;
      const float dt_h = dt / 3600000.0f;
      session.mwh += snap.power_total_w * 1000.0f * dt_h;
      session.chargedMs += dt;
      session.lastSampleMs = now;
    }
    session.active = true;

    if (snap.power_total_w > session.peakW) session.peakW = snap.power_total_w;
    const float aTot = (snap.ia_ma + snap.ic_ma) / 1000.0f;
    if (aTot > session.peakA) session.peakA = aTot;
    const float cA = snap.ic_ma / 1000.0f;
    const float aA = snap.ia_ma / 1000.0f;
    if (cA > session.peakC_A) session.peakC_A = cA;
    if (aA > session.peakA_A) session.peakA_A = aA;
    if (snap.power_c_w > session.peakC_W) {
      session.peakC_W = snap.power_c_w;
      session.peakC_W_A = cA;
    }
    if (snap.power_a_w > session.peakA_W) {
      session.peakA_W = snap.power_a_w;
      session.peakA_W_A = aA;
    }
    if (snap.vout_mv > session.peakVoutMv) session.peakVoutMv = snap.vout_mv;
  } else if (session.active) {
    // Close out the last loaded interval before stopping the clock
    if (session.lastSampleMs && now > session.lastSampleMs) {
      const uint32_t dt = now - session.lastSampleMs;
      session.mwh += snap.power_total_w * 1000.0f * (dt / 3600000.0f);
      session.chargedMs += dt;
      session.lastSampleMs = now;
    }
    if (loadGoneSince == 0) loadGoneSince = now;
    if (now - loadGoneSince >= kSessionEndDebounceMs) {
      session.active = false;
    }
  }
}

static void downsampleHist() {
  const size_t half = kHistMax / 2;
  for (size_t i = 0; i < half; i++) {
    histC[i] = (histC[2 * i] + histC[2 * i + 1]) * 0.5f;
    histA[i] = (histA[2 * i] + histA[2 * i + 1]) * 0.5f;
  }
  histCount = half;
  histPeriodMs *= 2;
}

static void pushHistory() {
  if (histCount >= kHistMax) downsampleHist();
  histC[histCount] = snap.power_c_w;
  histA[histCount] = snap.power_a_w;
  histCount++;
}

static uint32_t histSpanMs() {
  if (histCount < 2) return histPeriodMs;
  return (uint32_t)((histCount - 1) * histPeriodMs);
}

static void formatDuration(uint32_t ms, char* out, size_t n) {
  const uint32_t s = ms / 1000, m = s / 60, h = m / 60;
  if (h > 0) snprintf(out, n, "%luh%02lum", (unsigned long)h, (unsigned long)(m % 60));
  else if (m > 0) snprintf(out, n, "%lum%02lus", (unsigned long)m, (unsigned long)(s % 60));
  else snprintf(out, n, "%lus", (unsigned long)s);
}

static void drawSparkline(Adafruit_GFX& g, int x, int y, int w, int h, const float* data,
                          uint16_t color) {
  g.drawRect(x, y, w, h, COL_DARKGREY);
  if (histCount < 2 || w < 4 || h < 4) return;
  float mx = 0.1f;
  for (size_t i = 0; i < histCount; i++) {
    if (data[i] > mx) mx = data[i];
  }
  int prevX = x + 1, prevY = y + h - 2;
  const size_t denom = histCount > 1 ? histCount - 1 : 1;
  for (size_t i = 0; i < histCount; i++) {
    const int px = x + 1 + (int)((w - 3) * i / denom);
    const int py = y + h - 2 - (int)((h - 4) * (data[i] / mx));
    if (i > 0) g.drawLine(prevX, prevY, px, py, color);
    prevX = px;
    prevY = py;
  }
}

static void startZoom(Anim::Kind kind, Page from, Page to) {
  anim.kind = kind;
  anim.from = from;
  anim.to = to;
  anim.startMs = millis();
}

static void showModeToast(const char* label) {
  strncpy(modeToast, label, sizeof(modeToast) - 1);
  modeToast[sizeof(modeToast) - 1] = 0;
  modeToastUntil = millis() + 900;
}

static void enterRadioMode() {
  mode = Mode::Radio;
  radioPage = RadioPage::WifiScan;
  anim.kind = Anim::Idle;
  // Scan works without joining a network; ensure radio is up
  if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
  RadioTools::enter();
  RadioTools::requestScan();
  showModeToast("RADIO");
  Serial.println("Mode: RADIO");
}

static void enterChargerMode() {
  mode = Mode::Charger;
  RadioTools::leave();
  page = Page::Main;
  nextFromMain = 0;
  anim.kind = Anim::Idle;
  showModeToast("CHARGER");
  Serial.println("Mode: CHARGER");
}

static void onBootClick() {
  touchActivity();
  if (anim.busy()) return;

  if (mode == Mode::Radio) {
    radioPage = static_cast<RadioPage>((static_cast<uint8_t>(radioPage) + 1) %
                                       static_cast<uint8_t>(RadioPage::Count));
    return;
  }

  if (page == Page::Main) {
    Page dest = Page::UsbC;
    if (nextFromMain == 1) dest = Page::UsbA;
    else if (nextFromMain == 2) dest = Page::History;
    startZoom(Anim::ZoomIn, Page::Main, dest);
  } else {
    startZoom(Anim::ZoomOut, page, Page::Main);
  }
}

static void onBootLong() {
  touchActivity();
  if (mode == Mode::Radio) {
    RadioTools::requestScan();
    showModeToast("RESCAN");
    return;
  }
  clearSession();
  page = Page::Main;
  nextFromMain = 0;
  anim.kind = Anim::Idle;
}

static void onBootDouble() {
  touchActivity();
  if (anim.busy()) return;

  if (mode == Mode::Radio) {
    uint8_t i = static_cast<uint8_t>(radioPage);
    i = (i == 0) ? (static_cast<uint8_t>(RadioPage::Count) - 1) : (i - 1);
    radioPage = static_cast<RadioPage>(i);
    return;
  }

  if (page == Page::History) {
    startZoom(Anim::ZoomOut, Page::History, Page::Main);
  } else if (page == Page::Main) {
    startZoom(Anim::ZoomIn, Page::Main, Page::History);
  } else {
    anim.kind = Anim::Idle;
    page = Page::History;
  }
}

static void onBootMulti() {
  touchActivity();
  const int n = bootBtn.getNumberClicks();
  if (n < 3) return;
  if (mode == Mode::Charger) enterRadioMode();
  else enterChargerMode();
}

static void finishAnim(uint32_t now) {
  if (!anim.busy()) return;
  if (anim.rawT(now) < 1.f) return;
  page = anim.to;
  if (anim.kind == Anim::ZoomOut) {
    if (anim.from == Page::UsbC) seenUsbC = true;
    if (anim.from == Page::UsbA) seenUsbA = true;
    // Full C then A rotation complete → flash IP 10s by the status icons
    if (seenUsbC && seenUsbA && anim.from == Page::UsbA) {
      ipShowUntilMs = now + 10000;
      seenUsbC = false;
      seenUsbA = false;
    }
    nextFromMain = (nextFromMain + 1) % 3;
  }
  anim.kind = Anim::Idle;
}



static void ipText(char* out, size_t n) {
#if HAS_WIFI
  if (WiFi.status() == WL_CONNECTED) {
    IPAddress ip = WiFi.localIP();
    snprintf(out, n, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  } else {
    snprintf(out, n, "wifi...");
  }
#else
  snprintf(out, n, "no-wifi");
#endif
}

static int wifiBars() {
#if HAS_WIFI
  if (WiFi.status() != WL_CONNECTED) return 0;
  const int rssi = WiFi.RSSI();
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 1;
#else
  return 0;
#endif
}

static bool webActive(uint32_t now) {
  return lastWebHitMs != 0 && (now - lastWebHitMs) < kWebActiveMs;
}

static void drawConnIcons(Adafruit_GFX& g, int16_t rightX, int16_t y, bool withIp = false) {
  const uint32_t now = millis();
  const bool mqttOk =
#if HAS_WIFI && defined(MQTT_HOST)
      mqtt.connected();
#else
      false;
#endif
  const int bars = wifiBars();
  const bool webOn = webActive(now);
  // right-aligned cluster: [wifi][mqtt][web]
  const int16_t xWeb = rightX - 12;
  const int16_t xMqtt = xWeb - 14;
  const int16_t xWifi = xMqtt - 16;
  drawWifiIcon(g, xWifi, y, bars, COL_CYAN, COL_DIM);
  drawMqttIcon(g, xMqtt, y, mqttOk, COL_GREEN, COL_DIM);
  drawWebIcon(g, xWeb, y, webOn, COL_ORANGE, COL_DIM);
  if (withIp && now < ipShowUntilMs) {
    char ip[20];
    ipText(ip, sizeof(ip));
    // sit just left of the icon cluster
    gfxText(g, xWifi - 4, y + 1, ip, COL_CYAN, COL_BLACK, 1, false, true);
  }
}

static void drawMissing() {
  frame.fillScreen(COL_BLACK);
  gfxText(frame, frame.width() / 2, frame.height() / 2 - 12, "SW3518 not found", COL_ORANGE,
          COL_BLACK, 1, true);
  gfxText(frame, frame.width() / 2, frame.height() / 2 + 4, "I2C 0x3C SDA16/SCL17", COL_DARKGREY,
          COL_BLACK, 1, true);
  tft.push(frame);
}

static void drawMainChrome() {
  const int w = frame.width();
  const bool charging = snap.ia_ma > kLoadMa || snap.ic_ma > kLoadMa;
  // Compact charge flag (left); icons (+ timed IP) on the right
  gfxText(frame, 4, 2, charging ? "CHG" : "IDLE", charging ? COL_GREEN : COL_DARKGREY, COL_BLACK, 1);
  drawConnIcons(frame, w - 2, 1, true);

  const bool flash = millis() < protoFlashUntil;
  const char* proto = SW3518::protocolName(snap.protocol);
  if (flash) frame.fillRect(w / 2 - 50, 16, 100, 14, COL_YELLOW);
  gfxText(frame, w / 2, 18, proto, flash ? COL_BLACK : COL_YELLOW, flash ? COL_YELLOW : COL_BLACK, 1,
          true);

  char buf[40];
  snprintf(buf, sizeof(buf), "%.1fW", snap.power_total_w);
  gfxText(frame, w / 2, 34, buf, COL_WHITE, COL_BLACK, 2, true);

  snprintf(buf, sizeof(buf), "in %.1fV", snap.vin_mv / 1000.0f);
  gfxText(frame, 4, 62, buf, COL_LIGHTGREY, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "out %.2fV", snap.vout_mv / 1000.0f);
  gfxText(frame, w / 2, 62, buf, COL_LIGHTGREY, COL_BLACK, 1);

  gfxText(frame, 4, 78, "C", COL_YELLOW, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA", snap.ic_ma / 1000.0f);
  gfxText(frame, 14, 78, buf, COL_WHITE, COL_BLACK, 1);
  gfxText(frame, w / 2, 78, "A", COL_MAGENTA, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA", snap.ia_ma / 1000.0f);
  gfxText(frame, w / 2 + 10, 78, buf, COL_WHITE, COL_BLACK, 1);

  if (session.startMs != 0 || session.mwh > 0.01) {
    char dur[16];
    formatDuration(session.chargedMs, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "%s  pk %.0fW avg %.0fW  %.0fmWh", dur, session.peakW,
             sessionAvgW(), session.mwh);
  } else {
    snprintf(buf, sizeof(buf), "idle  long=clear  x3=radio");
  }
  gfxText(frame, 4, 94, buf, COL_DARKGREY, COL_BLACK, 1);
}

static void drawPortChrome(bool usbC, float alpha) {
  // alpha 0..1 fades in labels (simple: skip if low)
  if (alpha < 0.35f) return;
  const int w = frame.width();
  const uint16_t accent = usbC ? COL_YELLOW : COL_MAGENTA;
  const float amps = (usbC ? snap.ic_ma : snap.ia_ma) / 1000.0f;
  const float watts = usbC ? snap.power_c_w : snap.power_a_w;
  const float peakA = usbC ? session.peakC_A : session.peakA_A;

  gfxText(frame, 4, 2, usbC ? "USB-C" : "USB-A", accent, COL_BLACK, 1);
  drawConnIcons(frame, w - 2, 1, true);
  gfxText(frame, w / 2, 2, SW3518::protocolName(snap.protocol), COL_DARKGREY, COL_BLACK, 1, true);

  char buf[32];
  snprintf(buf, sizeof(buf), "%.2fW", watts);
  gfxText(frame, w / 2, 18, buf, COL_WHITE, COL_BLACK, 2, true);

  snprintf(buf, sizeof(buf), "%.2fV", snap.vout_mv / 1000.0f);
  gfxText(frame, 4, 48, buf, COL_LIGHTGREY, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA pk%.2f", amps, peakA);
  gfxText(frame, w / 2 - 10, 48, buf, COL_WHITE, COL_BLACK, 1);

  char span[24], label[36];
  formatDuration(session.startMs ? session.chargedMs : histSpanMs(), span, sizeof(span));
  snprintf(label, sizeof(label), "span %s", span);
  gfxText(frame, 4, 62, label, COL_DARKGREY, COL_BLACK, 1);
}

static void drawHistoryPage() {
  frame.fillScreen(COL_BLACK);
  const int w = frame.width();
  gfxText(frame, 4, 2, "SESSION", COL_CYAN, COL_BLACK, 1);
  drawConnIcons(frame, w - 2, 1, false);

  char buf[40], dur[16], ip[20];
  if (session.startMs != 0 || session.mwh > 0.01) {
    formatDuration(session.chargedMs, dur, sizeof(dur));
  } else {
    snprintf(dur, sizeof(dur), "--");
  }
  ipText(ip, sizeof(ip));
  gfxText(frame, 4, 14, dur, COL_LIGHTGREY, COL_BLACK, 1);
  // IP always available here (handy for the web UI)
  gfxText(frame, frame.width() - 4, 14, ip, COL_CYAN, COL_BLACK, 1, false, true);

  // Peak = max instantaneous W; Avg = energy / charged time (not wall clock)
  const float wh = session.mwh / 1000.0f;
  const float avgW = sessionAvgW();
  snprintf(buf, sizeof(buf), "%.1fW", session.peakW);
  gfxText(frame, 4, 28, "PEAK", COL_DARKGREY, COL_BLACK, 1);
  gfxText(frame, 4, 40, buf, COL_WHITE, COL_BLACK, 2);

  snprintf(buf, sizeof(buf), "%.1fW", avgW);
  gfxText(frame, 88, 28, "AVG", COL_DARKGREY, COL_BLACK, 1);
  gfxText(frame, 88, 40, buf, COL_WHITE, COL_BLACK, 2);

  snprintf(buf, sizeof(buf), "%.3fWh", wh);
  gfxText(frame, 168, 28, "ENERGY", COL_DARKGREY, COL_BLACK, 1);
  gfxText(frame, 168, 40, buf, COL_WHITE, COL_BLACK, 1);

  // W with amps at that same peak-W sample (not independent peak A)
  snprintf(buf, sizeof(buf), "C pk %.1fW @%.2fA", session.peakC_W, session.peakC_W_A);
  gfxText(frame, 4, 64, buf, COL_YELLOW, COL_BLACK, 1);
  drawSparkline(frame, 4, 76, 110, 22, histC, COL_YELLOW);

  snprintf(buf, sizeof(buf), "A pk %.1fW @%.2fA", session.peakA_W, session.peakA_W_A);
  gfxText(frame, 122, 64, buf, COL_MAGENTA, COL_BLACK, 1);
  drawSparkline(frame, 122, 76, 110, 22, histA, COL_MAGENTA);

  snprintf(buf, sizeof(buf), "Vout pk %.2fV", session.peakVoutMv / 1000.0f);
  gfxText(frame, 4, 104, buf, COL_LIGHTGREY, COL_BLACK, 1);
  gfxText(frame, 4, 118, "long=clear  x3=radio  dbl=session", COL_DARKGREY, COL_BLACK, 1);
}

static void drawModeToast() {
  if (!modeToastUntil || millis() > modeToastUntil) return;
  const int tw = (int)strlen(modeToast) * 12 + 16;
  const int x = (frame.width() - tw) / 2;
  frame.fillRoundRect(x, 48, tw, 28, 4, COL_CYAN);
  gfxText(frame, frame.width() / 2, 56, modeToast, COL_BLACK, COL_CYAN, 2, true);
}

static void drawRadioFrame() {
  const bool wifiUp = wifiEnabled && WiFi.status() == WL_CONNECTED;
  const int8_t rssi = wifiUp ? (int8_t)WiFi.RSSI() : (int8_t)-127;
  switch (radioPage) {
    case RadioPage::WifiScan:
      RadioTools::drawApList(frame, COL_WHITE, COL_DARKGREY, COL_YELLOW, COL_BLACK);
      break;
    case RadioPage::ChannelHeat:
      RadioTools::drawChannelHeat(frame, COL_WHITE, COL_DARKGREY, COL_MAGENTA, COL_BLACK);
      break;
    case RadioPage::Sys:
      RadioTools::drawSys(frame, COL_WHITE, COL_DARKGREY, COL_CYAN, COL_BLACK, wifiUp, rssi);
      break;
    case RadioPage::Help:
    default:
      RadioTools::drawHelp(frame, COL_WHITE, COL_DARKGREY, COL_CYAN, COL_BLACK);
      break;
  }
  drawModeToast();
  tft.push(frame);
}

static void drawFrame(uint32_t now) {
  finishAnim(now);
  frame.fillScreen(COL_BLACK);

  if (anim.busy()) {
    const float t = Anim::ease(anim.rawT(now));
    const Page detail = (anim.kind == Anim::ZoomIn) ? anim.to : anim.from;
    const bool usbC = (detail == Page::UsbC);
    const float* data = usbC ? histC : histA;
    const uint16_t color = usbC ? COL_YELLOW : COL_MAGENTA;
    Rect mini = usbC ? sparkMiniC() : sparkMiniA();
    Rect full = sparkPort();

    if (detail == Page::History) {
      // Fade between main and history
      if (anim.kind == Anim::ZoomIn) {
        drawMainChrome();
        Rect mc = sparkMiniC(), ma = sparkMiniA();
        drawSparkline(frame, (int)mc.x, (int)mc.y, (int)mc.w, (int)mc.h, histC, COL_YELLOW);
        drawSparkline(frame, (int)ma.x, (int)ma.y, (int)ma.w, (int)ma.h, histA, COL_MAGENTA);
        if (t > 0.45f) {
          // overlay history fading in via solid fill then content
          frame.fillScreen(COL_BLACK);
          drawHistoryPage();
        }
      } else {
        drawHistoryPage();
        if (t > 0.55f) {
          frame.fillScreen(COL_BLACK);
          drawMainChrome();
          Rect mc = sparkMiniC(), ma = sparkMiniA();
          drawSparkline(frame, (int)mc.x, (int)mc.y, (int)mc.w, (int)mc.h, histC, COL_YELLOW);
          drawSparkline(frame, (int)ma.x, (int)ma.y, (int)ma.w, (int)ma.h, histA, COL_MAGENTA);
        }
      }
      drawModeToast();
  tft.push(frame);
      return;
    }

    float zt = (anim.kind == Anim::ZoomIn) ? t : (1.f - t);
    Rect r = lerpRect(mini, full, zt);

    if (zt < 0.55f) {
      drawMainChrome();
      Rect other = usbC ? sparkMiniA() : sparkMiniC();
      const float* otherData = usbC ? histA : histC;
      const uint16_t otherCol = usbC ? COL_MAGENTA : COL_YELLOW;
      drawSparkline(frame, (int)other.x, (int)other.y, (int)other.w, (int)other.h, otherData,
                    otherCol);
    } else {
      drawPortChrome(usbC, zt);
    }
    drawSparkline(frame, (int)r.x, (int)r.y, (int)r.w, (int)r.h, data, color);
    drawModeToast();
  tft.push(frame);
    return;
  }

  if (page == Page::Main) {
    drawMainChrome();
    Rect mc = sparkMiniC(), ma = sparkMiniA();
    drawSparkline(frame, (int)mc.x, (int)mc.y, (int)mc.w, (int)mc.h, histC, COL_YELLOW);
    drawSparkline(frame, (int)ma.x, (int)ma.y, (int)ma.w, (int)ma.h, histA, COL_MAGENTA);
  } else if (page == Page::History) {
    drawHistoryPage();
  } else {
    const bool usbC = (page == Page::UsbC);
    drawPortChrome(usbC, 1.f);
    Rect r = sparkPort();
    drawSparkline(frame, (int)r.x, (int)r.y, (int)r.w, (int)r.h, usbC ? histC : histA,
                  usbC ? COL_YELLOW : COL_MAGENTA);
  }
  drawModeToast();
  tft.push(frame);
}

static void setupWifi() {
#if HAS_WIFI
  wifiEnabled = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("WiFi connecting to %s\n", WIFI_SSID);
#if defined(MQTT_HOST)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
#endif
#else
  wifiEnabled = false;
  Serial.println("WiFi/MQTT disabled (no secrets.h credentials)");
#endif
}

#ifndef MQTT_BASE
#define MQTT_BASE "geek/sw3518"
#endif
#ifndef MQTT_DISCOVERY_PREFIX
#define MQTT_DISCOVERY_PREFIX "homeassistant"
#endif

static bool mqttDiscoverySent = false;
static char mqttDevId[24];
static char mqttAvailTopic[48];

static void mqttBuildIds() {
#if HAS_WIFI && defined(MQTT_HOST)
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(mqttDevId, sizeof(mqttDevId), "sw3518geek_%04x", (unsigned)(mac & 0xFFFF));
  snprintf(mqttAvailTopic, sizeof(mqttAvailTopic), "%s/status", MQTT_BASE);
#endif
}

static void publishHaDiscovery() {
#if HAS_WIFI && defined(MQTT_HOST)
  char topic[128];
  char payload[768];
  char state[64];

  auto discSensor = [&](const char* objectId, const char* name, const char* leaf, const char* unit,
                        const char* deviceClass, const char* stateClass) {
    snprintf(state, sizeof(state), "%s/%s", MQTT_BASE, leaf);
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", MQTT_DISCOVERY_PREFIX, mqttDevId,
             objectId);
    if (deviceClass && deviceClass[0]) {
      snprintf(payload, sizeof(payload),
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
               "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"unit_of_meas\":\"%s\","
               "\"dev_cla\":\"%s\",\"stat_cla\":\"%s\","
               "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
               "\"mf\":\"DIY\"}}",
               name, mqttDevId, objectId, state, mqttAvailTopic, unit, deviceClass, stateClass,
               mqttDevId);
    } else {
      snprintf(payload, sizeof(payload),
               "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
               "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"unit_of_meas\":\"%s\","
               "\"stat_cla\":\"%s\","
               "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
               "\"mf\":\"DIY\"}}",
               name, mqttDevId, objectId, state, mqttAvailTopic, unit, stateClass, mqttDevId);
    }
    mqtt.publish(topic, payload, true);
  };

  auto discText = [&](const char* objectId, const char* name, const char* leaf) {
    snprintf(state, sizeof(state), "%s/%s", MQTT_BASE, leaf);
    snprintf(topic, sizeof(topic), "%s/sensor/%s/%s/config", MQTT_DISCOVERY_PREFIX, mqttDevId,
             objectId);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
             "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
             "\"mf\":\"DIY\"}}",
             name, mqttDevId, objectId, state, mqttAvailTopic, mqttDevId);
    mqtt.publish(topic, payload, true);
  };

  auto discBinary = [&](const char* objectId, const char* name, const char* leaf) {
    snprintf(state, sizeof(state), "%s/%s", MQTT_BASE, leaf);
    snprintf(topic, sizeof(topic), "%s/binary_sensor/%s/%s/config", MQTT_DISCOVERY_PREFIX, mqttDevId,
             objectId);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"uniq_id\":\"%s_%s\",\"stat_t\":\"%s\",\"avty_t\":\"%s\","
             "\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\","
             "\"pl_on\":\"ON\",\"pl_off\":\"OFF\",\"dev_cla\":\"power\","
             "\"dev\":{\"ids\":[\"%s\"],\"name\":\"SW3518 GEEK\",\"mdl\":\"ESP32-S3-GEEK+SW3518\","
             "\"mf\":\"DIY\"}}",
             name, mqttDevId, objectId, state, mqttAvailTopic, mqttDevId);
    mqtt.publish(topic, payload, true);
  };

  discSensor("vin", "Input voltage", "vin", "V", "voltage", "measurement");
  discSensor("vout", "Output voltage", "vout", "V", "voltage", "measurement");
  discSensor("i_c", "USB-C current", "i_c", "A", "current", "measurement");
  discSensor("i_a", "USB-A current", "i_a", "A", "current", "measurement");
  discSensor("power", "Total power", "power", "W", "power", "measurement");
  discSensor("power_c", "USB-C power", "power_c", "W", "power", "measurement");
  discSensor("power_a", "USB-A power", "power_a", "W", "power", "measurement");
  discSensor("session_wh", "Session energy", "session_wh", "Wh", "energy", "total_increasing");
  discSensor("session_mwh", "Session energy mWh", "session_mwh", "mWh", "", "measurement");
  discSensor("session_peak_w", "Session peak power", "session_peak_w", "W", "power", "measurement");
  discText("protocol", "Charge protocol", "protocol");
  discBinary("charging", "Charging", "charging");
  Serial.println("HA MQTT discovery published");
#endif
}

static void ensureMqtt() {
#if HAS_WIFI && defined(MQTT_HOST)
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqtt.connected()) return;

  mqttBuildIds();
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);

  bool ok = false;
#if defined(MQTT_USER)
  ok = mqtt.connect(mqttDevId, MQTT_USER, MQTT_PASSWORD, mqttAvailTopic, 1, true, "offline");
#else
  ok = mqtt.connect(mqttDevId, mqttAvailTopic, 1, true, "offline");
#endif
  if (!ok) {
    Serial.println("MQTT connect failed");
    mqttDiscoverySent = false;
    return;
  }
  mqtt.publish(mqttAvailTopic, "online", true);
  mqttDiscoverySent = false;
  Serial.println("MQTT connected");
#endif
}

static void publishMqtt() {
#if HAS_WIFI && defined(MQTT_HOST)
  if (!mqtt.connected()) return;

  if (!mqttDiscoverySent) {
    publishHaDiscovery();
    mqttDiscoverySent = true;
  }

  char topic[64], val[48];
  auto pub = [&](const char* leaf, const char* v) {
    snprintf(topic, sizeof(topic), "%s/%s", MQTT_BASE, leaf);
    mqtt.publish(topic, v, true);
  };

  snprintf(val, sizeof(val), "%.3f", snap.vin_mv / 1000.0f);
  pub("vin", val);
  snprintf(val, sizeof(val), "%.3f", snap.vout_mv / 1000.0f);
  pub("vout", val);
  snprintf(val, sizeof(val), "%.3f", snap.ic_ma / 1000.0f);
  pub("i_c", val);
  snprintf(val, sizeof(val), "%.3f", snap.ia_ma / 1000.0f);
  pub("i_a", val);
  snprintf(val, sizeof(val), "%.3f", snap.power_total_w);
  pub("power", val);
  snprintf(val, sizeof(val), "%.3f", snap.power_c_w);
  pub("power_c", val);
  snprintf(val, sizeof(val), "%.3f", snap.power_a_w);
  pub("power_a", val);
  pub("protocol", SW3518::protocolName(snap.protocol));
  snprintf(val, sizeof(val), "%.1f", session.mwh);
  pub("session_mwh", val);
  snprintf(val, sizeof(val), "%.4f", session.mwh / 1000.0f);
  pub("session_wh", val);
  snprintf(val, sizeof(val), "%.2f", session.peakW);
  pub("session_peak_w", val);
  const bool charging = snap.ia_ma > kLoadMa || snap.ic_ma > kLoadMa;
  pub("charging", charging ? "ON" : "OFF");
  mqtt.publish(mqttAvailTopic, "online", true);
#endif
}


static void noteWebHit() { lastWebHitMs = millis(); }

static void handleRoot() {
  noteWebHit();
  char page[1200];
  const bool charging = snap.ia_ma > kLoadMa || snap.ic_ma > kLoadMa;
  snprintf(page, sizeof(page),
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta http-equiv=refresh content=2>"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<title>SW3518 GEEK</title>"
           "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:1.2rem}"
           "h1{font-size:1.2rem;color:#0ff} .g{color:#8f8} .card{background:#1c1c1c;padding:1rem;"
           "border-radius:10px;margin:.6rem 0} b{color:#fff}</style></head><body>"
           "<h1>SW3518 GEEK</h1>"
           "<div class=card><b>%.2f W</b> total &nbsp; %s<br>"
           "in %.2f V &nbsp; out %.2f V<br>"
           "USB-C %.2f A / %.2f W<br>"
           "USB-A %.2f A / %.2f W<br>"
           "protocol %s<br>"
           "session %.0f mWh (%.3f Wh) &nbsp; peak %.1f W</div>"
           "<div class=card class=g>MQTT %s &nbsp; Wi‑Fi %s (%d dBm)</div>"
           "<p><a href=/radio style=color:#0ff>radio</a> · <a href=/help style=color:#0ff>help</a></p><p style=color:#666>Auto-refresh 2s — icon on device lights while you are here.</p>"
           "</body></html>",
           snap.power_total_w, charging ? "CHARGING" : "IDLE", snap.vin_mv / 1000.0f,
           snap.vout_mv / 1000.0f, snap.ic_ma / 1000.0f, snap.power_c_w, snap.ia_ma / 1000.0f,
           snap.power_a_w, SW3518::protocolName(snap.protocol), session.mwh, session.mwh / 1000.0f,
           session.peakW,
#if HAS_WIFI && defined(MQTT_HOST)
           mqtt.connected() ? "up" : "down",
#else
           "n/a",
#endif
#if HAS_WIFI
           WiFi.status() == WL_CONNECTED ? "up" : "down",
           WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0
#else
           "n/a", 0
#endif
  );
  web.send(200, "text/html", page);
}

static void handleApi() {
  noteWebHit();
  char json[384];
  snprintf(json, sizeof(json),
           "{\"vin\":%.3f,\"vout\":%.3f,\"i_c\":%.3f,\"i_a\":%.3f,\"power\":%.3f,"
           "\"power_c\":%.3f,\"power_a\":%.3f,\"protocol\":\"%s\","
           "\"session_mwh\":%.1f,\"session_wh\":%.4f,\"peak_w\":%.2f}",
           snap.vin_mv / 1000.0f, snap.vout_mv / 1000.0f, snap.ic_ma / 1000.0f, snap.ia_ma / 1000.0f,
           snap.power_total_w, snap.power_c_w, snap.power_a_w, SW3518::protocolName(snap.protocol),
           session.mwh, session.mwh / 1000.0f, session.peakW);
  web.send(200, "application/json", json);
}

static void handleRadio() {
  noteWebHit();
  char body[1600];
  char apJson[768];
  RadioTools::jsonStatus(apJson, sizeof(apJson));
  const char* modeName = (mode == Mode::Radio) ? "radio" : "charger";
  snprintf(body, sizeof(body),
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta http-equiv=refresh content=3>"
           "<meta name=viewport content="width=device-width,initial-scale=1">"
           "<title>GEEK Radio</title>"
           "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:1.2rem}"
           "h1{font-size:1.2rem;color:#0ff}a{color:#0ff}.card{background:#1c1c1c;padding:1rem;"
           "border-radius:10px;margin:.6rem 0} pre{white-space:pre-wrap;color:#aaa;font-size:.85rem}"
           "</style></head><body>"
           "<h1>Radio</h1><p>Device mode: <b>%s</b> — "
           "<a href=/>charger</a> · <a href=/help>help</a></p>"
           "<div class=card><pre>%s</pre></div>"
           "<p style=color:#666>AP list from Wi‑Fi beacon scan (own RF view). "
           "Triple‑click BOOT on device to toggle Radio.</p>"
           "</body></html>",
           modeName, apJson);
  web.send(200, "text/html", body);
}

static void handleHelp() {
  noteWebHit();
  web.send(200, "text/html",
           "<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content="width=device-width,initial-scale=1">"
           "<title>GEEK Help</title>"
           "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;margin:1.2rem}"
           "h1{color:#0ff}li{margin:.35rem 0}a{color:#0ff}.card{background:#1c1c1c;padding:1rem;"
           "border-radius:10px}</style></head><body>"
           "<h1>BOOT controls</h1><div class=card><ul>"
           "<li><b>Short</b> — next page (charger zoom / radio pages)</li>"
           "<li><b>Double</b> — Session history (charger) or previous radio page</li>"
           "<li><b>Triple</b> — toggle Charger ↔ Radio</li>"
           "<li><b>Long</b> — clear session (charger) or rescan (radio)</li>"
           "</ul></div>"
           "<p><a href=/>charger</a> · <a href=/radio>radio</a> · <a href=/api>api</a></p>"
           "</body></html>");
}

static void setupWeb() {
#if HAS_WIFI
  if (webStarted) return;
  web.on("/", handleRoot);
  web.on("/api", handleApi);
  web.on("/radio", handleRadio);
  web.on("/help", handleHelp);
  web.onNotFound([]() {
    noteWebHit();
    web.send(404, "text/plain", "not found");
  });
  web.begin();
  webStarted = true;
  Serial.println("Web server :80");
#endif
}

static void setupSd() {
  sdSpi = new SPIClass(HSPI);
  sdSpi->begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  sdOk = SD.begin(PIN_SD_CS, *sdSpi, 20000000);
  Serial.println(sdOk ? "SD OK" : "SD not present");
  if (sdOk && !SD.exists("/sw3518.csv")) {
    File f = SD.open("/sw3518.csv", FILE_WRITE);
    if (f) {
      f.println("ms,vin_mv,vout_mv,ic_ma,ia_ma,power_w,protocol");
      f.close();
    }
  }
}

static void logSd(uint32_t now) {
  if (!sdOk) return;
  if (!(snap.ia_ma > kLoadMa || snap.ic_ma > kLoadMa)) return;
  File f = SD.open("/sw3518.csv", FILE_APPEND);
  if (!f) {
    sdOk = false;
    return;
  }
  f.printf("%lu,%u,%u,%u,%u,%.3f,%s\n", (unsigned long)now, snap.vin_mv, snap.vout_mv, snap.ic_ma,
           snap.ia_ma, snap.power_total_w, SW3518::protocolName(snap.protocol));
  f.close();
}

void setup() {
  pinMode(PIN_TFT_BL, OUTPUT);
  for (int i = 0; i < 4; ++i) {
    digitalWrite(PIN_TFT_BL, HIGH);
    delay(80);
    digitalWrite(PIN_TFT_BL, LOW);
    delay(80);
  }
  digitalWrite(PIN_TFT_BL, HIGH);

  Serial.begin(115200);
  UartDbg.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  const uint32_t serialDeadline = millis() + 2000;
  while (!Serial && millis() < serialDeadline) delay(10);

  logLine("");
  logLine("ESP32-S3-GEEK SW3518 stats");

  Serial.println("Adafruit ST7789 init...");
  Serial.flush();
  tft.beginPanel();
  Serial.println("panel ok");
  Serial.flush();

  tft.fillScreen(COL_RED);
  delay(200);
  tft.fillScreen(COL_GREEN);
  delay(200);
  tft.fillScreen(COL_BLACK);

  RadioTools::begin();
  bootBtn.attachClick(onBootClick);
  bootBtn.attachDoubleClick(onBootDouble);
  bootBtn.attachMultiClick(onBootMulti);
  bootBtn.attachLongPressStart(onBootLong);
  bootBtn.setLongPressIntervalMs(800);
  bootBtn.setClickTicks(450);
  lastActivityMs = millis();
  ipShowUntilMs = millis() + 90000;  // IP hint for 90s after boot

  setupWifi();
  setupSd();

  if (!charger.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000)) {
    Serial.println("SW3518 not found at 0x3C");
  } else {
    Serial.println("SW3518 OK");
  }
}

void loop() {
  bootBtn.tick();
  const uint32_t now = millis();
  RadioTools::tick(now);

  if (now - lastBeatMs >= 2000) {
    lastBeatMs = now;
    Serial.printf("alive %lu page=%u anim=%u hist=%u\n", (unsigned long)now, (unsigned)page,
                  (unsigned)anim.kind, (unsigned)histCount);
    Serial.flush();
  }

  if (wifiEnabled) {
    static wl_status_t lastWifi = WL_IDLE_STATUS;
    const wl_status_t st = WiFi.status();
    if (st == WL_CONNECTED) {
      if (lastWifi != WL_CONNECTED) {
        Serial.printf("WiFi IP %s\n", WiFi.localIP().toString().c_str());
      }
      setupWeb();
      web.handleClient();
    }
    lastWifi = st;
    ensureMqtt();
    mqtt.loop();
  }

  if (!nightDim && (now - lastActivityMs) >= kNightIdleMs) {
    nightDim = true;
    blLevel = kBlDim;
    applyBacklight();
  }

  if (!charger.present() && now - lastProbeMs > 1000) {
    lastProbeMs = now;
    if (charger.probe()) {
      charger.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
      Serial.println("SW3518 appeared");
    }
  }

  const uint32_t uiPeriod = anim.busy() ? 33 : kUiMs;  // ~30fps while zooming
  if (now - lastUiMs >= uiPeriod) {
    lastUiMs = now;
    if (mode == Mode::Radio) {
      // Keep session stats warm if charger is up, but UI is radio
      if (charger.present() && charger.readSnapshot(snap)) {
        pushHistory();
        updateSession(now);
        if (wifiEnabled && now - lastMqttMs >= kMqttMs) {
          lastMqttMs = now;
          publishMqtt();
        }
        if (now - lastSdMs >= kSdLogMs) {
          lastSdMs = now;
          logSd(now);
        }
      }
      drawFrame(now);
    } else if (!charger.present()) {
      drawMissing();
    } else if (charger.readSnapshot(snap)) {
      if (snap.protocol != lastProtocol) {
        lastProtocol = snap.protocol;
        protoFlashUntil = now + 2000;
      }
      pushHistory();
      updateSession(now);
      drawFrame(now);

      if (wifiEnabled && now - lastMqttMs >= kMqttMs) {
        lastMqttMs = now;
        publishMqtt();
      }
      if (now - lastSdMs >= kSdLogMs) {
        lastSdMs = now;
        logSd(now);
      }
    } else {
      drawMissing();
    }
  }
}
