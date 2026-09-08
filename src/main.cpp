#include <Arduino.h>
#include <TFT_eSPI.h>
#include <OneButton.h>

#include "pins.h"
#include "sw3518.h"

enum class Page : uint8_t { Main = 0, UsbC = 1, UsbA = 2 };

static constexpr size_t kHist = 48;
static constexpr uint32_t kUiMs = 250;
static constexpr uint32_t kHomeTimeoutMs = 60000;

TFT_eSPI tft;
TFT_eSprite spr(&tft);
SW3518 charger;
OneButton bootBtn(PIN_BOOT_BTN, true, true);

bool backlightOn = true;
Page page = Page::Main;
uint32_t lastUiMs = 0;
uint32_t lastProbeMs = 0;
uint32_t lastNavMs = 0;
SW3518::Snapshot snap;

float histC[kHist] = {};
float histA[kHist] = {};
size_t histIdx = 0;
size_t histCount = 0;

static void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(PIN_TFT_BL, on ? HIGH : LOW);
}

static void goHome() {
  page = Page::Main;
  lastNavMs = millis();
}

static void onBootClick() {
  // Main -> USB-C -> USB-A -> Main
  if (page == Page::Main) page = Page::UsbC;
  else if (page == Page::UsbC) page = Page::UsbA;
  else page = Page::Main;
  lastNavMs = millis();
}

static void onBootDouble() {
  goHome();
}

static void onBootLong() {
  setBacklight(!backlightOn);
  lastNavMs = millis();
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
  spr.drawRect(x, y, w, h, TFT_DARKGREY);
  int prevX = x + 1;
  int prevY = y + h - 2;
  for (size_t i = 0; i < histCount; i++) {
    const size_t idx = (histIdx + kHist - histCount + i) % kHist;
    const float v = data[idx];
    const int px = x + 1 + (int)((w - 3) * i / (histCount - 1));
    const int py = y + h - 2 - (int)((h - 4) * (v / mx));
    if (i > 0) spr.drawLine(prevX, prevY, px, py, color);
    prevX = px;
    prevY = py;
  }
}

static void drawMissing() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TFT_ORANGE, TFT_BLACK);
  spr.drawString("SW3518 not found", spr.width() / 2, spr.height() / 2 - 12, 2);
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.drawString("I2C 0x3C  SDA16/SCL17", spr.width() / 2, spr.height() / 2 + 12, 2);
  spr.pushSprite(0, 0);
}

static void drawMain() {
  const int w = spr.width();
  spr.fillSprite(TFT_BLACK);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.drawString("CHARGER", 4, 2, 2);

  const bool charging = snap.ia_ma > 50 || snap.ic_ma > 50;
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(charging ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  spr.drawString(charging ? "CHG" : "IDLE", w - 4, 2, 2);

  // Protocol
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  char proto[24];
  if (snap.protocol == SW3518::Protocol::PdFix || snap.protocol == SW3518::Protocol::PdPps) {
    if (snap.pd_ver == 1) snprintf(proto, sizeof(proto), "%s (PD2)", SW3518::protocolName(snap.protocol));
    else if (snap.pd_ver == 2) snprintf(proto, sizeof(proto), "%s (PD3)", SW3518::protocolName(snap.protocol));
    else snprintf(proto, sizeof(proto), "%s", SW3518::protocolName(snap.protocol));
  } else {
    snprintf(proto, sizeof(proto), "%s", SW3518::protocolName(snap.protocol));
  }
  spr.drawString(proto, w / 2, 20, 2);

  // Big watts
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f W", snap.power_total_w);
  spr.drawString(buf, w / 2, 40, 4);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.2fV in", snap.vin_mv / 1000.0f);
  spr.drawString(buf, 4, 78, 2);
  snprintf(buf, sizeof(buf), "%.2fV out", snap.vout_mv / 1000.0f);
  spr.drawString(buf, w / 2 + 4, 78, 2);

  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  spr.drawString("C", 4, 100, 2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.2fA %.1fW", snap.ic_ma / 1000.0f, snap.power_c_w);
  spr.drawString(buf, 20, 100, 2);

  spr.setTextColor(TFT_MAGENTA, TFT_BLACK);
  spr.drawString("A", 4, 118, 2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.2fA %.1fW", snap.ia_ma / 1000.0f, snap.power_a_w);
  spr.drawString(buf, 20, 118, 2);

  spr.pushSprite(0, 0);
}

static void drawPort(bool usbC) {
  const int w = spr.width();
  spr.fillSprite(TFT_BLACK);

  const uint16_t accent = usbC ? TFT_YELLOW : TFT_MAGENTA;
  const float amps = (usbC ? snap.ic_ma : snap.ia_ma) / 1000.0f;
  const float watts = usbC ? snap.power_c_w : snap.power_a_w;
  const float* hist = usbC ? histC : histA;

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(accent, TFT_BLACK);
  spr.drawString(usbC ? "USB-C" : "USB-A", 4, 2, 2);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
  spr.drawString(SW3518::protocolName(snap.protocol), w - 4, 2, 2);

  char buf[32];
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.2f V", snap.vout_mv / 1000.0f);
  spr.drawString(buf, 4, 22, 4);
  snprintf(buf, sizeof(buf), "%.2f A", amps);
  spr.drawString(buf, 4, 52, 2);
  snprintf(buf, sizeof(buf), "%.2f W", watts);
  spr.drawString(buf, w / 2, 52, 2);

  drawSparkline(4, 78, w - 8, 50, hist, accent);

  spr.pushSprite(0, 0);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32-S3-GEEK SW3518 stats");

  pinMode(PIN_TFT_BL, OUTPUT);
  setBacklight(true);

  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);

  spr.setColorDepth(16);
  spr.createSprite(tft.width(), tft.height());
  spr.setTextFont(2);

  bootBtn.attachClick(onBootClick);
  bootBtn.attachDoubleClick(onBootDouble);
  bootBtn.attachLongPressStart(onBootLong);
  lastNavMs = millis();

  if (!charger.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000)) {
    Serial.println("SW3518 not found at 0x3C");
  } else {
    Serial.println("SW3518 OK");
  }
}

void loop() {
  bootBtn.tick();
  const uint32_t now = millis();

  if (page != Page::Main && (now - lastNavMs) >= kHomeTimeoutMs) {
    goHome();
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
      pushHistory();
      Serial.printf("P=%s Vin=%.2f Vout=%.2f Ic=%.3f Ia=%.3f Pt=%.2f\n",
                    SW3518::protocolName(snap.protocol),
                    snap.vin_mv / 1000.0f, snap.vout_mv / 1000.0f,
                    snap.ic_ma / 1000.0f, snap.ia_ma / 1000.0f,
                    snap.power_total_w);
      if (page == Page::Main) drawMain();
      else if (page == Page::UsbC) drawPort(true);
      else drawPort(false);
    } else {
      Serial.println("SW3518 read failed");
      drawMissing();
    }
  }
}
