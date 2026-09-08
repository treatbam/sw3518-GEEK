#include <Arduino.h>
#include <esp_system.h>
#include <OneButton.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <SD.h>

#include "pins.h"
#include "sw3518.h"
#include "secrets.h"
#include "geek_display.h"

enum class Page : uint8_t { Main = 0, UsbC = 1, UsbA = 2 };

static constexpr size_t kHist = 48;
static constexpr uint32_t kUiMs = 250;
static constexpr uint32_t kHomeTimeoutMs = 60000;
static constexpr uint32_t kNightIdleMs = 45000;
static constexpr uint32_t kMqttMs = 5000;
static constexpr uint32_t kSdLogMs = 1000;
static constexpr float kLoadMa = 50.0f;
static constexpr int kBlFull = 255;
static constexpr int kBlDim = 40;

GeekDisplay tft;
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

bool backlightForcedOff = false;
bool nightDim = false;
int blLevel = kBlFull;
Page page = Page::Main;
uint32_t lastUiMs = 0;
uint32_t lastProbeMs = 0;
uint32_t lastNavMs = 0;
uint32_t lastActivityMs = 0;
uint32_t lastMqttMs = 0;
uint32_t lastSdMs = 0;
uint32_t lastBeatMs = 0;
uint32_t protoFlashUntil = 0;
SW3518::Protocol lastProtocol = SW3518::Protocol::None;
SW3518::Snapshot snap;

float histC[kHist] = {};
float histA[kHist] = {};
size_t histIdx = 0;
size_t histCount = 0;

struct Session {
  bool active = false;
  uint32_t startMs = 0;
  uint32_t lastSampleMs = 0;
  double mwh = 0;
  float peakW = 0;
  float peakA = 0;
  float peakC_A = 0;
  float peakA_A = 0;
} session;

bool wifiEnabled = false;
bool sdOk = false;

#if defined(WIFI_SSID)
#define HAS_WIFI 1
#else
#define HAS_WIFI 0
#endif

static void applyBacklight() {
  if (backlightForcedOff) {
    digitalWrite(PIN_TFT_BL, LOW);
    return;
  }
  if (blLevel >= kBlFull) digitalWrite(PIN_TFT_BL, HIGH);
  else analogWrite(PIN_TFT_BL, blLevel);
}

static void touchActivity() {
  lastActivityMs = millis();
  lastNavMs = lastActivityMs;
  if (nightDim && !backlightForcedOff) {
    nightDim = false;
    blLevel = kBlFull;
    applyBacklight();
  }
}

static void goHome() {
  page = Page::Main;
  touchActivity();
}

static void clearSession() {
  session = Session{};
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
  } else if (session.active) {
    session.lastSampleMs = now;
  }
}

static void onBootClick() {
  touchActivity();
  if (page == Page::Main) page = Page::UsbC;
  else if (page == Page::UsbC) page = Page::UsbA;
  else page = Page::Main;
}

static void onBootDouble() {
  touchActivity();
  if (page != Page::Main) goHome();
  else clearSession();
}

static void onBootLong() {
  touchActivity();
  backlightForcedOff = !backlightForcedOff;
  if (!backlightForcedOff) {
    nightDim = false;
    blLevel = kBlFull;
  }
  applyBacklight();
}

static void pushHistory() {
  histC[histIdx] = snap.power_c_w;
  histA[histIdx] = snap.power_a_w;
  histIdx = (histIdx + 1) % kHist;
  if (histCount < kHist) histCount++;
}

static void drawSparkline(int x, int y, int w, int h, const float* data, uint16_t color) {
  if (histCount < 2) return;
  float mx = 0.1f;
  for (size_t i = 0; i < histCount; i++) {
    const size_t idx = (histIdx + kHist - histCount + i) % kHist;
    if (data[idx] > mx) mx = data[idx];
  }
  tft.drawRect(x, y, w, h, COL_DARKGREY);
  int prevX = x + 1, prevY = y + h - 2;
  const size_t denom = histCount > 1 ? histCount - 1 : 1;
  for (size_t i = 0; i < histCount; i++) {
    const size_t idx = (histIdx + kHist - histCount + i) % kHist;
    const int px = x + 1 + (int)((w - 3) * i / denom);
    const int py = y + h - 2 - (int)((h - 4) * (data[idx] / mx));
    if (i > 0) tft.drawLine(prevX, prevY, px, py, color);
    prevX = px;
    prevY = py;
  }
}

static void formatDuration(uint32_t ms, char* out, size_t n) {
  const uint32_t s = ms / 1000, m = s / 60, h = m / 60;
  if (h > 0) snprintf(out, n, "%luh%02lum", (unsigned long)h, (unsigned long)(m % 60));
  else snprintf(out, n, "%lum%02lus", (unsigned long)m, (unsigned long)(s % 60));
}

static void drawMissing() {
  tft.fillScreen(COL_BLACK);
  tft.text(tft.W() / 2, tft.H() / 2 - 12, "SW3518 not found", COL_ORANGE, COL_BLACK, 1, true);
  tft.text(tft.W() / 2, tft.H() / 2 + 4, "I2C 0x3C SDA16/SCL17", COL_DARKGREY, COL_BLACK, 1, true);
}

static void drawMain(uint32_t now) {
  const int w = tft.W();
  tft.fillScreen(COL_BLACK);

  tft.text(4, 2, "CHARGER", COL_CYAN, COL_BLACK, 1);
  const bool charging = snap.ia_ma > kLoadMa || snap.ic_ma > kLoadMa;
  tft.text(w - 4, 2, charging ? "CHG" : "IDLE", charging ? COL_GREEN : COL_DARKGREY, COL_BLACK, 1, false, true);

  const bool flash = now < protoFlashUntil;
  const char* proto = SW3518::protocolName(snap.protocol);
  if (flash) tft.fillRect(w / 2 - 50, 16, 100, 14, COL_YELLOW);
  tft.text(w / 2, 18, proto, flash ? COL_BLACK : COL_YELLOW, flash ? COL_YELLOW : COL_BLACK, 1, true);

  char buf[40];
  snprintf(buf, sizeof(buf), "%.1fW", snap.power_total_w);
  tft.text(w / 2, 34, buf, COL_WHITE, COL_BLACK, 2, true);

  snprintf(buf, sizeof(buf), "in %.1fV", snap.vin_mv / 1000.0f);
  tft.text(4, 62, buf, COL_LIGHTGREY, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "out %.2fV", snap.vout_mv / 1000.0f);
  tft.text(w / 2, 62, buf, COL_LIGHTGREY, COL_BLACK, 1);

  tft.text(4, 78, "C", COL_YELLOW, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA", snap.ic_ma / 1000.0f);
  tft.text(14, 78, buf, COL_WHITE, COL_BLACK, 1);
  tft.text(w / 2, 78, "A", COL_MAGENTA, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fA", snap.ia_ma / 1000.0f);
  tft.text(w / 2 + 10, 78, buf, COL_WHITE, COL_BLACK, 1);

  if (session.active || session.mwh > 0.01) {
    char dur[16];
    formatDuration(now - session.startMs, dur, sizeof(dur));
    snprintf(buf, sizeof(buf), "%s pk %.0fW %.0fmWh", dur, session.peakW, session.mwh);
  } else {
    snprintf(buf, sizeof(buf), "session -- dbl-tap clear");
  }
  tft.text(4, 94, buf, COL_DARKGREY, COL_BLACK, 1);

  drawSparkline(4, 106, w / 2 - 6, 26, histC, COL_YELLOW);
  drawSparkline(w / 2 + 2, 106, w / 2 - 6, 26, histA, COL_MAGENTA);
}

static void drawPort(bool usbC) {
  const int w = tft.W();
  tft.fillScreen(COL_BLACK);
  const uint16_t accent = usbC ? COL_YELLOW : COL_MAGENTA;
  const float amps = (usbC ? snap.ic_ma : snap.ia_ma) / 1000.0f;
  const float watts = usbC ? snap.power_c_w : snap.power_a_w;
  const float* hist = usbC ? histC : histA;
  const float peakA = usbC ? session.peakC_A : session.peakA_A;

  tft.text(4, 2, usbC ? "USB-C" : "USB-A", accent, COL_BLACK, 1);
  tft.text(w - 4, 2, SW3518::protocolName(snap.protocol), COL_DARKGREY, COL_BLACK, 1, false, true);

  char buf[32];
  snprintf(buf, sizeof(buf), "%.2f V", snap.vout_mv / 1000.0f);
  tft.text(4, 20, buf, COL_WHITE, COL_BLACK, 2);
  snprintf(buf, sizeof(buf), "%.2fA pk %.2fA", amps, peakA);
  tft.text(4, 52, buf, COL_WHITE, COL_BLACK, 1);
  snprintf(buf, sizeof(buf), "%.2fW", watts);
  tft.text(w / 2, 52, buf, COL_WHITE, COL_BLACK, 1);
  drawSparkline(4, 76, w - 8, 54, hist, accent);
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

static void ensureMqtt() {
#if HAS_WIFI && defined(MQTT_HOST)
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  mqtt.connect("sw3518-geek"
#if defined(MQTT_USER)
               ,
               MQTT_USER, MQTT_PASSWORD
#endif
  );
#endif
}

static void publishMqtt() {
#if HAS_WIFI && defined(MQTT_HOST)
  if (!mqtt.connected()) return;
#ifndef MQTT_BASE
#define MQTT_BASE "geek/sw3518"
#endif
  char topic[64], val[48];
  auto pub = [&](const char* leaf, const char* v) {
    snprintf(topic, sizeof(topic), "%s/%s", MQTT_BASE, leaf);
    mqtt.publish(topic, v, true);
  };
  snprintf(val, sizeof(val), "%.3f", snap.vin_mv / 1000.0f); pub("vin", val);
  snprintf(val, sizeof(val), "%.3f", snap.vout_mv / 1000.0f); pub("vout", val);
  snprintf(val, sizeof(val), "%.3f", snap.ic_ma / 1000.0f); pub("i_c", val);
  snprintf(val, sizeof(val), "%.3f", snap.ia_ma / 1000.0f); pub("i_a", val);
  snprintf(val, sizeof(val), "%.3f", snap.power_total_w); pub("power", val);
  pub("protocol", SW3518::protocolName(snap.protocol));
  snprintf(val, sizeof(val), "%.1f", session.mwh); pub("session_mwh", val);
  snprintf(val, sizeof(val), "%.2f", session.peakW); pub("session_peak_w", val);
#endif
}

static void setupSd() {
  // Create HSPI only after LCD owns FSPI — avoids S3 SPI bring-up fights.
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
  f.printf("%lu,%u,%u,%u,%u,%.3f,%s\n", (unsigned long)now, snap.vin_mv, snap.vout_mv,
           snap.ic_ma, snap.ia_ma, snap.power_total_w, SW3518::protocolName(snap.protocol));
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
  const int rr = (int)esp_reset_reason();
  const char* rrs = "?";
  switch (rr) {
    case 1: rrs = "POWERON"; break;
    case 3: rrs = "SW"; break;
    case 4: rrs = "PANIC"; break;
    case 5: rrs = "INT_WDT"; break;
    case 6: rrs = "TASK_WDT"; break;
    case 7: rrs = "WDT"; break;
    case 9: rrs = "BROWNOUT"; break;
    default: break;
  }
  Serial.printf("CDC + UART0, reset %d (%s)\n", rr, rrs);
  UartDbg.printf("CDC + UART0, reset %d (%s)\n", rr, rrs);

  Serial.println("Adafruit ST7789 init...");
  Serial.flush();
  tft.beginPanel();
  Serial.println("panel ok");
  Serial.flush();

  tft.fillScreen(COL_RED);
  delay(250);
  tft.fillScreen(COL_GREEN);
  delay(250);
  tft.fillScreen(COL_BLACK);
  Serial.printf("TFT %dx%d\n", tft.W(), tft.H());
  Serial.flush();
  digitalWrite(PIN_TFT_BL, HIGH);

  bootBtn.attachClick(onBootClick);
  bootBtn.attachDoubleClick(onBootDouble);
  bootBtn.attachLongPressStart(onBootLong);
  lastNavMs = lastActivityMs = millis();

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
    Serial.printf("alive %lu tft=%dx%d\n", (unsigned long)now, tft.W(), tft.H());
    Serial.flush();
    UartDbg.printf("alive %lu tft=%dx%d\n", (unsigned long)now, tft.W(), tft.H());
  }

  if (wifiEnabled) {
    ensureMqtt();
    mqtt.loop();
  }

  if (page != Page::Main && (now - lastNavMs) >= kHomeTimeoutMs) goHome();

  if (!backlightForcedOff && !nightDim && (now - lastActivityMs) >= kNightIdleMs) {
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

  if (now - lastUiMs >= kUiMs) {
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
      if (page == Page::Main) drawMain(now);
      else if (page == Page::UsbC) drawPort(true);
      else drawPort(false);

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
