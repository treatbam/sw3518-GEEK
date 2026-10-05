#include <Arduino.h>
#include <OneButton.h>
#include <SPI.h>
#include "app_state.h"
#include "features.h"
#include "net.h"
#include "radio_tools.h"
#include "totp_tools.h"
#include "ui.h"

static OneButton bootBtn(PIN_BOOT_BTN, true, true);

static void syncRadioFocus() {
  if (app.radioPage == RadioPage::BleScan) RadioTools::setFocus(RadioTools::Focus::Ble);
  else if (app.radioPage == RadioPage::Waterfall)
    RadioTools::setFocus(RadioTools::Focus::Waterfall);
  else
    RadioTools::setFocus(RadioTools::Focus::Wifi);
}

static void enterRadioMode() {
  app.mode = Mode::Radio;
  app.radioPage = RadioPage::WifiScan;
  app.anim.kind = Anim::Idle;
  uiShowModeToast("RADIO");
  syncRadioFocus();
  RadioTools::enter();
}

static void hapticPulse(uint32_t ms = 40, int strength = 200) {
  static uint32_t hapticUntil = 0;
  analogWrite(PIN_HAPTIC, strength);
  hapticUntil = millis() + ms;
}

static void hapticService() {
  static uint32_t hapticUntil = 0;
  if (hapticUntil && millis() > hapticUntil) {
    analogWrite(PIN_HAPTIC, 0);
    hapticUntil = 0;
  }
}

static void cycleAppMode() {
  if (app.mode == Mode::Radio) {
    RadioTools::leave();
    app.mode = Mode::Totp;
    uiShowModeToast("2FA");
  } else {
    enterRadioMode();
  }
}

static void onBootClick() {
  uiTouchActivity();
  if (app.mode == Mode::Radio) {
    app.radioPage = static_cast<RadioPage>((static_cast<uint8_t>(app.radioPage) + 1) %
                                           static_cast<uint8_t>(RadioPage::Count));
    syncRadioFocus();
    return;
  }
#if HAS_TOTP
  if (app.mode == Mode::Totp) {
    TotpTools::nextAccount();
    return;
  }
#endif
}

static void onBootLong() {
  uiTouchActivity();
  if (app.mode == Mode::Radio) {
    RadioTools::requestScan();
    uiShowModeToast("RESCAN");
    return;
  }
}

static void onBootDouble() {
  uiTouchActivity();
  if (app.mode == Mode::Radio) {
    uint8_t i = static_cast<uint8_t>(app.radioPage);
    i = (i == 0) ? (static_cast<uint8_t>(RadioPage::Count) - 1) : (i - 1);
    app.radioPage = static_cast<RadioPage>(i);
    syncRadioFocus();
    return;
  }
#if HAS_TOTP
  if (app.mode == Mode::Totp) {
    TotpTools::typeCurrentCode();
    return;
  }
#endif
}

static void onBootMulti() {
  uiTouchActivity();
  if (bootBtn.getNumberClicks() < 3) return;
  cycleAppMode();
}

static void serviceSideButtons() {
  static bool prevL = true, prevR = true;
  static uint32_t lastL = 0, lastR = 0;
  const uint32_t now = millis();
  const bool l = digitalRead(PIN_BTN_LEFT);
  const bool r = digitalRead(PIN_BTN_RIGHT);
  if (l != prevL) {
    prevL = l;
    if (!l && now - lastL > 40) {
      lastL = now;
      uiTouchActivity();
#if HAS_TOTP
      if (app.mode == Mode::Totp) {
        TotpTools::prevAccount();
      }
#endif
    }
  }
  if (r != prevR) {
    prevR = r;
    if (!r && now - lastR > 40) {
      lastR = now;
      uiTouchActivity();
#if HAS_TOTP
      if (app.mode == Mode::Totp) {
        TotpTools::nextAccount();
      }
#endif
    }
  }
}

void setup() {
  pinMode(PIN_TFT_BL, OUTPUT);
  pinMode(PIN_HAPTIC, OUTPUT);
  analogWrite(PIN_HAPTIC, 0);
  for (int i = 0; i < 4; ++i) {
    digitalWrite(PIN_TFT_BL, HIGH);
    delay(80);
    digitalWrite(PIN_TFT_BL, LOW);
    delay(80);
  }
  digitalWrite(PIN_TFT_BL, HIGH);

  Serial.begin(115200);
  app.uartDbg.begin(115200, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
  const uint32_t serialDeadline = millis() + 2000;
  while (!Serial && millis() < serialDeadline) delay(10);

  Serial.println("Adafruit ST7789 init...");
  Serial.flush();
  uiBeginPanel();
  Serial.println("panel ok");
  Serial.flush();

  app.tft.fillScreen(COL_RED);
  delay(200);
  app.tft.fillScreen(COL_GREEN);
  delay(200);
  app.tft.fillScreen(COL_BLACK);

  pinMode(PIN_BTN_LEFT, INPUT_PULLUP);
  pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);

  RadioTools::begin();

  bootBtn.attachClick(onBootClick);
  bootBtn.attachDoubleClick(onBootDouble);
  bootBtn.attachMultiClick(onBootMulti);
  bootBtn.attachLongPressStart(onBootLong);
  bootBtn.setLongPressIntervalMs(800);
  bootBtn.setClickMs(450);
  app.lastActivityMs = millis();
  app.ipShowUntilMs = millis() + 90000;

  netSetup();
  TotpTools::begin();
  app.mode = Mode::Totp;

  hapticPulse(30, 180);
}

void loop() {
  const uint32_t loopT0 = micros();
  bootBtn.tick();
  serviceSideButtons();
  hapticService();
  const uint32_t now = millis();
  RadioTools::tick(now);
  TotpTools::tick(now);
  netTick();

  if (now - app.lastBeatMs >= 2000) {
    app.lastBeatMs = now;
    Serial.printf("alive %lu page=%u anim=%u\n", (unsigned long)now, (unsigned)app.page,
                  (unsigned)app.anim.kind);
    Serial.flush();
  }

  if (!app.nightDim && (now - app.lastActivityMs) >= kNightIdleMs) {
    app.nightDim = true;
    app.blLevel = kBlDim;
    uiApplyBacklight();
  }

  const uint32_t uiPeriod = app.anim.busy() ? 33 : kUiMs;
  if (now - app.lastUiMs >= uiPeriod) {
    app.lastUiMs = now;
    uiDraw(now);
  }

  app.lastLoopUs = micros() - loopT0;
  static uint32_t lpsCount = 0;
  static uint32_t lpsWindowMs = 0;
  lpsCount++;
  if (lpsWindowMs == 0) lpsWindowMs = now;
  if (now - lpsWindowMs >= 1000) {
    app.loopsPerSec = (uint16_t)(lpsCount > 65535 ? 65535 : lpsCount);
    lpsCount = 0;
    lpsWindowMs = now;
  }
}
