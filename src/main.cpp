#include <Arduino.h>
#include <TFT_eSPI.h>
#include <OneButton.h>

#include "pins.h"
#include "sw3518.h"

TFT_eSPI tft;
TFT_eSprite spr(&tft);
SW3518 charger;
OneButton bootBtn(PIN_BOOT_BTN, true, true);

bool backlightOn = true;
bool detailPage = false;
uint32_t lastUiMs = 0;
uint32_t lastProbeMs = 0;
SW3518::Snapshot snap;

static void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(PIN_TFT_BL, on ? HIGH : LOW);
}

static void onBootClick() {
  detailPage = !detailPage;
}

static void onBootLong() {
  setBacklight(!backlightOn);
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

static void drawOverview() {
  spr.fillSprite(TFT_BLACK);
  const int w = spr.width();

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.drawString("SW3518", 4, 2, 2);

  const bool charging = snap.ia_ma > 50 || snap.ic_ma > 50;
  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(charging ? TFT_GREEN : TFT_DARKGREY, TFT_BLACK);
  spr.drawString(charging ? "CHG" : "IDLE", w - 4, 2, 2);

  // Big total watts
  spr.setTextDatum(TC_DATUM);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  char buf[32];
  snprintf(buf, sizeof(buf), "%.1f W", snap.power_total_w);
  spr.drawString(buf, w / 2, 22, 4);

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  snprintf(buf, sizeof(buf), "Vin %.2fV", snap.vin_mv / 1000.0f);
  spr.drawString(buf, 4, 62, 2);
  snprintf(buf, sizeof(buf), "Vout %.2fV", snap.vout_mv / 1000.0f);
  spr.drawString(buf, w / 2, 62, 2);

  spr.setTextColor(TFT_YELLOW, TFT_BLACK);
  spr.drawString("C", 4, 88, 2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.2fA  %.1fW", snap.ic_ma / 1000.0f, snap.power_c_w);
  spr.drawString(buf, 22, 88, 2);

  spr.setTextColor(TFT_MAGENTA, TFT_BLACK);
  spr.drawString("A", 4, 110, 2);
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(buf, sizeof(buf), "%.2fA  %.1fW", snap.ia_ma / 1000.0f, snap.power_a_w);
  spr.drawString(buf, 22, 110, 2);

  spr.pushSprite(0, 0);
}

static void drawDetail() {
  spr.fillSprite(TFT_BLACK);
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TFT_CYAN, TFT_BLACK);
  spr.drawString("Detail", 4, 2, 2);

  char buf[40];
  spr.setTextColor(TFT_WHITE, TFT_BLACK);
  auto row = [&](int y, const char* label, const char* value) {
    spr.setTextColor(TFT_DARKGREY, TFT_BLACK);
    spr.drawString(label, 4, y, 2);
    spr.setTextColor(TFT_WHITE, TFT_BLACK);
    spr.drawString(value, 70, y, 2);
  };

  snprintf(buf, sizeof(buf), "%u mV", snap.vin_mv);
  row(24, "Vin", buf);
  snprintf(buf, sizeof(buf), "%u mV", snap.vout_mv);
  row(44, "Vout", buf);
  snprintf(buf, sizeof(buf), "%u mA", snap.ic_ma);
  row(64, "I-C", buf);
  snprintf(buf, sizeof(buf), "%u mA", snap.ia_ma);
  row(84, "I-A", buf);
  snprintf(buf, sizeof(buf), "%.2f W", snap.power_total_w);
  row(104, "Ptot", buf);

  spr.pushSprite(0, 0);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("ESP32-S3-GEEK SW3518 stats");

  pinMode(PIN_TFT_BL, OUTPUT);
  setBacklight(true);

  tft.init();
  tft.setRotation(1);  // landscape 240x135
  tft.fillScreen(TFT_BLACK);

  spr.setColorDepth(16);
  spr.createSprite(tft.width(), tft.height());
  spr.setTextFont(2);

  bootBtn.attachClick(onBootClick);
  bootBtn.attachLongPressStart(onBootLong);

  if (!charger.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000)) {
    Serial.println("SW3518 not found at 0x3C");
  } else {
    Serial.println("SW3518 OK");
  }
}

void loop() {
  bootBtn.tick();
  const uint32_t now = millis();

  if (!charger.present() && now - lastProbeMs > 1000) {
    lastProbeMs = now;
    if (charger.probe()) {
      charger.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
      Serial.println("SW3518 appeared");
    }
  }

  if (now - lastUiMs >= 250) {
    lastUiMs = now;
    if (!charger.present()) {
      drawMissing();
    } else if (charger.readSnapshot(snap)) {
      Serial.printf("Vin=%.2f Vout=%.2f Ic=%.3f Ia=%.3f P=%.2f\n",
                    snap.vin_mv / 1000.0f, snap.vout_mv / 1000.0f,
                    snap.ic_ma / 1000.0f, snap.ia_ma / 1000.0f,
                    snap.power_total_w);
      if (detailPage) drawDetail();
      else drawOverview();
    } else {
      Serial.println("SW3518 read failed");
      drawMissing();
    }
  }
}
