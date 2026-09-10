// Isolated BleCombo translation unit (avoids KEY_*/Mouse clashes with USBHID*).
#include "features.h"
#include <Arduino.h>

#if HAS_HID && __has_include(<BleCombo.h>)
#include <BleCombo.h>
#include <BLEDevice.h>
#define HID_BLE 1
#else
#define HID_BLE 0
#endif

bool hidBleBegin() {
#if HID_BLE
  Keyboard.begin();
  Mouse.begin();
  return true;
#else
  return false;
#endif
}

void hidBleEnd() {
#if HID_BLE
  Keyboard.releaseAll();
  BLEDevice::deinit(false);
#endif
}

bool hidBleConnected() {
#if HID_BLE
  return Keyboard.isConnected();
#else
  return false;
#endif
}

void hidBleWrite(uint8_t k) {
#if HID_BLE
  if (Keyboard.isConnected()) Keyboard.write(k);
#else
  (void)k;
#endif
}

void hidBlePress(uint8_t k) {
#if HID_BLE
  if (Keyboard.isConnected()) Keyboard.press(k);
#else
  (void)k;
#endif
}

void hidBleReleaseAll() {
#if HID_BLE
  if (Keyboard.isConnected()) Keyboard.releaseAll();
#endif
}

void hidBleMouseMove(int8_t x, int8_t y, int8_t wheel) {
#if HID_BLE
  if (Keyboard.isConnected()) Mouse.move(x, y, wheel);
#else
  (void)x;
  (void)y;
  (void)wheel;
#endif
}

void hidBleMouseClick(uint8_t btn) {
#if HID_BLE
  if (Keyboard.isConnected()) Mouse.click(btn);
#else
  (void)btn;
#endif
}
