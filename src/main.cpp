#include <Arduino.h>
#include <esp_system.h>
#include <OneButton.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <SD.h>
#include <math.h>

#include "pins.h"
#include "sw3518.h"
#include "secrets.h"
#include "geek_display.h"

enum class Page : uint8_t { Main = 0, UsbC = 1, UsbA = 2, History = 3 };

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
SPIClass* sdSpi = nullptr;

bool nightDim = false;
int blLevel = kBlFull;
Page page = Page::Main;
uint8_t nextFromMain = 0;  // 0=UsbC, 1=UsbA, 2=History
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
  bool active = false;
  uint32_t startMs = 0;
  uint32_t lastSampleMs = 0;
  double mwh = 0;
  float peakW = 0;
  float peakA = 0;
  float peakC_A = 0;
  float peakA_A = 0;
  float peakC_W = 0;
  float peakA_W = 0;
  uint16_t peakVoutMv = 0;
} session;

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
  if (load) {
    if (!session.active) {
      session.active = true;
      session.startMs = now;
      session.lastSampleMs = now;
      session.mwh = 0;
      session.peakW = session.peakA = session.peakC_A = session.peakA_A = 0;
      session.peakC_W = session.peakA_W = 0;
      session.peakVoutMv = 0;
      histCount = 0;
      histPeriodMs = 250;
    } else {
      const float dt_h = (now - session.lastSampleMs) / 3600000.0f;
      session.mwh += snap.power_total_w * 1000.0f * dt_h;
      session.lastSampleMs = now;
    }
    if (snap.power_total_w > session.peakW) session.peakW = snap.power_total_w;
    const float aTot = (snap.ia_ma + snap.ic_ma) / 1000.0f;
    if (aTot > session.peakA) session.peakA = aTot;
    const float cA = snap.ic_ma / 1000.0f;
    const float aA = snap.ia_ma / 1000.0f;
    if (cA > session.peakC_A) session.peakC_A = cA;
    if (aA > session.peakA_A) session.peakA_A = aA;
    if (snap.power_c_w > session.peakC_W) session.peakC_W = snap.power_c_w;
    if (snap.power_a_w > session.peakA_W) session.peakA_W = snap.power_a_w;
    if (snap.vout_mv > session.peakVoutMv) session.peakVoutMv = snap.vout_mv;
  } else if (session.active) {
    session.lastSampleMs = now;
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

static void onBootClick() {
  touchActivity();
  if (anim.busy()) return;

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
  clearSession();
  page = Page::Main;
  nextFromMain = 0;
  anim.kind = Anim::Idle;
}

static void onBootDouble() {
  touchActivity();
  if (anim.busy()) return;
  if (page == Page::History) {
    startZoom(Anim::ZoomOut, Page::History, Page::Main);
  } else if (page == Page::Main) {
    startZoom(Anim::ZoomIn, Page::Main, Page::History);
  } else {
    // From a port page: jump straight to session stats
    anim.kind = Anim::Idle;
    page = Page::History;
  }
}

static void finishAnim(uint32_t now) {
  if (!anim.busy()) return;
  if (anim.rawT(now) < 1.f) return;
  page = anim.to;
  if (anim.kind == Anim::ZoomOut) {
    nextFromMain = (nextFromMain + 1) % 3;
  }
  anim.kind = Anim::Idle;
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
  gfxText(frame, 4, 2, "CHARGER", COL_CYAN, COL_BLACK, 1);
  const bool charging = snap.ia_ma > kLoadMa || snap.ic_ma > kLoadMa;
  gfxText(frame, w - 4, 2, charging ? "CHG" : "IDLE", charging ? COL_GREEN : COL_DARKGREY, COL_BLACK,
          1, false, true);

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

  if (session.active || session.mwh > 0.01) {
    char dur[16];
    formatDuration(millis() - session.startMs, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "%s pk %.0fW %.0fmWh", dur, session.peakW, session.mwh);
  } else {
    snprintf(buf, sizeof(buf), "idle — long-hold clears");
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
  gfxText(frame, w - 4, 2, SW3518::protocolName(snap.protocol), COL_DARKGREY, COL_BLACK, 1, false,
          true);

  char buf[32];
  snprintf(buf, sizeof(buf), "%.2fW", watts);
  gfxText(frame, w / 2, 18, buf, COL_WHITE, COL_BLACK, 2, true);

  snprintf(buf, sizeof(buf), "%.2fV", snap.vout_mv / 1000.0f);
  gfxText(frame, 4, 48, buf, COL_LIGHTGREY, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA pk%.2f", amps, peakA);
  gfxText(frame, w / 2 - 10, 48, buf, COL_WHITE, COL_BLACK, 1);

  char span[24], label[36];
  formatDuration(session.active ? (millis() - session.startMs) : histSpanMs(), span, sizeof(span));
  snprintf(label, sizeof(label), "span %s", span);
  gfxText(frame, 4, 62, label, COL_DARKGREY, COL_BLACK, 1);
}

static void drawHistoryPage() {
  frame.fillScreen(COL_BLACK);
  gfxText(frame, 4, 2, "SESSION", COL_CYAN, COL_BLACK, 1);
  char buf[40], dur[16];
  if (session.active || session.mwh > 0.01) {
    formatDuration(millis() - session.startMs, dur, sizeof(dur));
  } else {
    snprintf(dur, sizeof(dur), "--");
  }
  gfxText(frame, frame.width() - 4, 2, dur, COL_LIGHTGREY, COL_BLACK, 1, false, true);

  snprintf(buf, sizeof(buf), "%.1fW", session.peakW);
  gfxText(frame, 8, 22, "PEAK W", COL_DARKGREY, COL_BLACK, 1);
  gfxText(frame, 8, 36, buf, COL_WHITE, COL_BLACK, 2);

  snprintf(buf, sizeof(buf), "%.0f", session.mwh);
  gfxText(frame, 130, 22, "mWh", COL_DARKGREY, COL_BLACK, 1);
  gfxText(frame, 130, 36, buf, COL_WHITE, COL_BLACK, 2);

  snprintf(buf, sizeof(buf), "C %.2fA / %.1fW", session.peakC_A, session.peakC_W);
  gfxText(frame, 8, 70, buf, COL_YELLOW, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "A %.2fA / %.1fW", session.peakA_A, session.peakA_W);
  gfxText(frame, 8, 86, buf, COL_MAGENTA, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "Vout pk %.2fV", session.peakVoutMv / 1000.0f);
  gfxText(frame, 8, 102, buf, COL_LIGHTGREY, COL_BLACK, 1);
  gfxText(frame, 8, 118, "long-hold = new session", COL_DARKGREY, COL_BLACK, 1);
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
  discSensor("session_mwh", "Session energy", "session_mwh", "mWh", "energy", "total_increasing");
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
  snprintf(val, sizeof(val), "%.2f", session.peakW);
  pub("session_peak_w", val);
  const bool charging = snap.ia_ma > kLoadMa || snap.ic_ma > kLoadMa;
  pub("charging", charging ? "ON" : "OFF");
  mqtt.publish(mqttAvailTopic, "online", true);
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

  bootBtn.attachClick(onBootClick);
  bootBtn.attachDoubleClick(onBootDouble);
  bootBtn.attachLongPressStart(onBootLong);
  bootBtn.setLongPressIntervalMs(800);
  lastActivityMs = millis();

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

  if (now - lastBeatMs >= 2000) {
    lastBeatMs = now;
    Serial.printf("alive %lu page=%u anim=%u hist=%u\n", (unsigned long)now, (unsigned)page,
                  (unsigned)anim.kind, (unsigned)histCount);
    Serial.flush();
  }

  if (wifiEnabled) {
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
    if (!charger.present()) {
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
