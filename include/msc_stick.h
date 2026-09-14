#pragma once

#include <Adafruit_GFX.h>
#include <stdint.h>
#include "features.h"

// USB MSC stick mode: expose TF/SD to host + MoveSpeed-inspired dashboard.
// Composite: CDC (serial) + MSC. No USB HID in this build.
namespace MscStick {

static constexpr size_t kSparkLen = 48;

bool begin();          // SD + USBMSC; safe if SD missing
void tick(uint32_t now);
bool sdOk();
bool usbMounted();     // host recently reading/writing

float tempC();         // MCU approx °C
float readMBps();      // recent host read throughput
float writeMBps();     // recent host write throughput
uint64_t cardBytes();
uint64_t usedBytes();
uint8_t usedPercent();
const char* linkRateLabel();  // honest FS rate, e.g. "12Mbps"

const float* sparkRead();
const float* sparkWrite();
uint8_t sparkCount();

void drawDashboard(Adafruit_GFX& g, uint16_t fg, uint16_t dim, uint16_t accent,
                   uint16_t warn, uint16_t bg);

}  // namespace MscStick
