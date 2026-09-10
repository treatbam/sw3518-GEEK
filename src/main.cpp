#include <Arduino.h>
#include <OneButton.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <string.h>

#include "app_state.h"
#include "features.h"
#include "hid_tools.h"
#include "net.h"
#include "pins.h"
#include "radio_tools.h"
#include "session_store.h"
#include "ui.h"

static OneButton bootBtn(PIN_BOOT_BTN, true, true);
static SPIClass* sdSpi = nullptr;

static uint32_t hapticUntil = 0;

static void logLine(const char* msg) {
  Serial.println(msg);
  Serial.flush();
  app.uartDbg.println(msg);
  app.uartDbg.flush();
}

static void hapticPulse(uint16_t ms = 40, uint8_t duty = 200) {
  analogWrite(PIN_HAPTIC, duty);
  hapticUntil = millis() + ms;
}

static void hapticService() {
  if (hapticUntil && (int32_t)(millis() - hapticUntil) >= 0) {
    analogWrite(PIN_HAPTIC, 0);
    hapticUntil = 0;
  }
}

static void clearSession() {
  sessionCaptureSaved();
  app.session.clear();
  sessionStoreErase();
  app.histFace = HistFace::Session;
  uiResetHistFaceTimer();
  hapticPulse(80, 220);
  Serial.println("Session cleared");
}

static void syncRadioFocus() {
#if HAS_RADIO
  if (app.radioPage == RadioPage::BleScan) RadioTools::setFocus(RadioTools::Focus::Ble);
  else if (app.radioPage == RadioPage::Waterfall)
    RadioTools::setFocus(RadioTools::Focus::Waterfall);
  else if (app.radioPage == RadioPage::WifiScan)
    RadioTools::setFocus(RadioTools::Focus::Wifi);
  else
    RadioTools::setFocus(RadioTools::Focus::Idle);
#endif
}

#if HAS_RADIO
static void enterRadioMode() {
  HidTools::leave();
  RadioTools::invalidateBle();
  app.mode = Mode::Radio;
  app.radioPage = RadioPage::WifiScan;
  app.anim.kind = Anim::Idle;
  if (WiFi.getMode() == WIFI_MODE_NULL) WiFi.mode(WIFI_STA);
  RadioTools::enter();
  RadioTools::requestScan();
  syncRadioFocus();
  uiShowModeToast("RADIO");
  Serial.println("Mode: RADIO");
  hapticPulse();
  uiDraw(millis());
}
#endif

static void enterChargerMode() {
  RadioTools::leave();
  HidTools::leave();
  RadioTools::invalidateBle();
  app.mode = Mode::Charger;
  app.page = Page::Main;
  app.nextFromMain = 0;
  app.anim.kind = Anim::Idle;
  uiShowModeToast("CHARGER");
  Serial.println("Mode: CHARGER");
  hapticPulse();
  uiDraw(millis());
}

#if HAS_HID
static void enterHidMode() {
  RadioTools::leave();
  app.mode = Mode::Hid;
  app.hidPage = HidTools::Page::Status;
  app.hidMacroIdx = 0;
  app.anim.kind = Anim::Idle;
  HidTools::enter();
  uiShowModeToast("HID");
  Serial.println("Mode: HID");
  hapticPulse();
  uiDraw(millis());
}
#endif

static void cycleAppMode() {
  if (app.mode == Mode::Charger) {
#if HAS_RADIO
    enterRadioMode();
#elif HAS_HID
    enterHidMode();
#endif
  } else if (app.mode == Mode::Radio) {
#if HAS_HID
    enterHidMode();
#else
    enterChargerMode();
#endif
  } else {
    enterChargerMode();
  }
}

static void onBootClick() {
  uiTouchActivity();
  if (app.anim.busy()) return;

  if (app.mode == Mode::Radio) {
    app.radioPage = static_cast<RadioPage>((static_cast<uint8_t>(app.radioPage) + 1) %
                                           static_cast<uint8_t>(RadioPage::Count));
    syncRadioFocus();
    return;
  }

  if (app.mode == Mode::Hid) {
    app.hidPage = static_cast<HidTools::Page>((static_cast<uint8_t>(app.hidPage) + 1) %
                                              static_cast<uint8_t>(HidTools::Page::Count));
    return;
  }

  if (app.page == Page::Main) {
    Page dest = Page::UsbC;
    if (app.nextFromMain == 1) dest = Page::UsbA;
    else if (app.nextFromMain == 2) dest = Page::History;
    uiStartZoom(Anim::ZoomIn, Page::Main, dest);
  } else {
    uiStartZoom(Anim::ZoomOut, app.page, Page::Main);
  }
}

static void onBootLong() {
  uiTouchActivity();
  if (app.mode == Mode::Radio) {
    RadioTools::requestScan();
    uiShowModeToast("RESCAN");
    return;
  }
  if (app.mode == Mode::Hid) {
    if (app.hidPage == HidTools::Page::Keys) HidTools::actionEnter();
    else if (app.hidPage == HidTools::Page::Mouse)
      HidTools::mouseClick(1);
    else if (app.hidPage == HidTools::Page::Macros) {
      HidTools::runMacro(app.hidMacroIdx);
      const uint8_t ran = app.hidMacroIdx;
      app.hidMacroIdx = (uint8_t)((app.hidMacroIdx + 1) % HidTools::macroCount());
      uiShowModeToast(HidTools::macroName(ran));
    } else if (app.hidPage == HidTools::Page::Status) {
      uiShowModeToast(HidTools::bleConnected() ? "BLE OK" : "BLE...");
    } else {
      HidTools::actionTab();
    }
    return;
  }
  clearSession();
  app.page = Page::Main;
  app.nextFromMain = 0;
  app.anim.kind = Anim::Idle;
}

static void onBootDouble() {
  uiTouchActivity();
  if (app.anim.busy()) return;

  if (app.mode == Mode::Radio) {
    uint8_t i = static_cast<uint8_t>(app.radioPage);
    i = (i == 0) ? (static_cast<uint8_t>(RadioPage::Count) - 1) : (i - 1);
    app.radioPage = static_cast<RadioPage>(i);
    syncRadioFocus();
    return;
  }

  if (app.mode == Mode::Hid) {
    if (app.hidPage == HidTools::Page::Keys) {
      HidTools::actionEsc();
      return;
    }
    if (app.hidPage == HidTools::Page::Mouse) {
      HidTools::mouseClick(2);
      return;
    }
    uint8_t i = static_cast<uint8_t>(app.hidPage);
    i = (i == 0) ? (static_cast<uint8_t>(HidTools::Page::Count) - 1) : (i - 1);
    app.hidPage = static_cast<HidTools::Page>(i);
    return;
  }

  if (app.page == Page::History) {
    uiStartZoom(Anim::ZoomOut, Page::History, Page::Main);
  } else if (app.page == Page::Main) {
    uiStartZoom(Anim::ZoomIn, Page::Main, Page::History);
  } else {
    app.anim.kind = Anim::Idle;
    app.page = Page::History;
    app.histFace = HistFace::Session;
    uiResetHistFaceTimer();
  }
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
      if (app.mode == Mode::Hid) {
        if (app.hidPage == HidTools::Page::Mouse) HidTools::mouseMove(-12, 0);
        else if (app.hidPage == HidTools::Page::Keys)
          HidTools::actionArrowLeft();
        else {
          uint8_t i = static_cast<uint8_t>(app.hidPage);
          i = (i == 0) ? (static_cast<uint8_t>(HidTools::Page::Count) - 1) : (i - 1);
          app.hidPage = static_cast<HidTools::Page>(i);
        }
      }
    }
  }
  if (r != prevR) {
    prevR = r;
    if (!r && now - lastR > 40) {
      lastR = now;
      uiTouchActivity();
      if (app.mode == Mode::Hid) {
        if (app.hidPage == HidTools::Page::Mouse) HidTools::mouseMove(12, 0);
        else if (app.hidPage == HidTools::Page::Keys)
          HidTools::actionArrowRight();
        else {
          app.hidPage = static_cast<HidTools::Page>(
              (static_cast<uint8_t>(app.hidPage) + 1) % static_cast<uint8_t>(HidTools::Page::Count));
        }
      }
    }
  }
}

static void setupSd() {
  sdSpi = new SPIClass(HSPI);
  sdSpi->begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  app.sdOk = SD.begin(PIN_SD_CS, *sdSpi, 20000000);
  Serial.println(app.sdOk ? "SD OK" : "SD not present");
  if (app.sdOk && !SD.exists("/sw3518.csv")) {
    File f = SD.open("/sw3518.csv", FILE_WRITE);
    if (f) {
      f.println("ms,vin_mv,vout_mv,ic_ma,ia_ma,power_w,protocol");
      f.close();
    }
  }
}

static void logSd(uint32_t now) {
  if (!app.sdOk) return;
  if (!(app.snap.ia_ma > Session::kLoadMa || app.snap.ic_ma > Session::kLoadMa)) return;
  File f = SD.open("/sw3518.csv", FILE_APPEND);
  if (!f) {
    app.sdOk = false;
    return;
  }
  f.printf("%lu,%u,%u,%u,%u,%.3f,%s\n", (unsigned long)now, app.snap.vin_mv, app.snap.vout_mv,
           app.snap.ic_ma, app.snap.ia_ma, app.snap.power_total_w,
           SW3518::protocolName(app.snap.protocol));
  f.close();
}

static void chargerTick(uint32_t now) {
  Link link = Link::Lost;
  if (app.charger.readSnapshot(app.snap)) {
    link = Link::Ok;
    if (app.snap.protocol != app.lastProtocol) {
      app.lastProtocol = app.snap.protocol;
      app.protoFlashUntil = now + 2000;
    }
    const ChargeSample sample = app.snap.sample();
    app.session.onTick(now, &sample, link);
    app.session.pushHistory(app.snap.power_c_w, app.snap.power_a_w);
    if (app.wifiEnabled && now - app.lastMqttMs >= kMqttMs) {
      app.lastMqttMs = now;
      netPublish();
    }
    if (now - app.lastSdMs >= kSdLogMs) {
      app.lastSdMs = now;
      logSd(now);
    }
  } else {
    app.snap = SW3518::Snapshot{};
    app.lastProtocol = SW3518::Protocol::None;
    if (now - app.lastProbeMs > 1000) {
      app.lastProbeMs = now;
      if (app.charger.probe()) app.charger.rearm();
    }
    app.session.onTick(now, nullptr, Link::Lost);
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

  logLine("");
  logLine("ESP32-S3-GEEK SW3518 stats");

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
  HidTools::begin();
  bootBtn.attachClick(onBootClick);
  bootBtn.attachDoubleClick(onBootDouble);
  bootBtn.attachMultiClick(onBootMulti);
  bootBtn.attachLongPressStart(onBootLong);
  bootBtn.setLongPressIntervalMs(800);
  bootBtn.setClickMs(450);
  app.lastActivityMs = millis();
  app.ipShowUntilMs = millis() + 90000;

  netSetup();
  setupSd();

  if (!app.charger.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000)) {
    Serial.println("SW3518 not found at 0x3C");
  } else {
    Serial.println("SW3518 OK");
  }

  sessionStoreLoad();
  hapticPulse(30, 180);
}

void loop() {
  const uint32_t loopT0 = micros();
  bootBtn.tick();
  serviceSideButtons();
  hapticService();
  const uint32_t now = millis();
  RadioTools::tick(now);
  HidTools::tick(now);
  uiTickHistFace(now);
  sessionStoreTick(now, false);
  netTick();

  if (now - app.lastBeatMs >= 2000) {
    app.lastBeatMs = now;
    Serial.printf("alive %lu page=%u anim=%u hist=%u\n", (unsigned long)now, (unsigned)app.page,
                  (unsigned)app.anim.kind, (unsigned)app.session.histCount);
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
    chargerTick(now);
    if (app.session.dirty && app.session.phase == Phase::Paused) {
      sessionStoreTick(now, true);
    }
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
