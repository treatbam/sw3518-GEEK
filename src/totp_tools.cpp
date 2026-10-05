#include "features.h"
#if HAS_TOTP

#include "totp_tools.h"
#include <TOTP.h>
#include "app_state.h"
#include "ui.h"

// Hardcoded default secret: JBSWY3DPEHPK3PXP
static uint8_t hmacKey[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x21, 0xde, 0xad, 0xbe, 0xef};
static TOTP totp = TOTP(hmacKey, 10);

static char currentCode[7] = "000000";
static uint32_t lastCodeMs = 0;
static uint32_t simulatedTime = 0;

void TotpTools::begin() {
  simulatedTime = 1672531200; // Jan 1 2023
  strncpy(currentCode, totp.getCode(simulatedTime), 6);
}

void TotpTools::tick(uint32_t now) {
  static uint32_t lastNow = 0;
  if (lastNow == 0) lastNow = now;

  if (now - lastNow >= 1000) {
    simulatedTime += (now - lastNow) / 1000;
    lastNow = now;
  }

  if (now - lastCodeMs >= 1000) {
    lastCodeMs = now;
    char* newCode = totp.getCode(simulatedTime);
    strncpy(currentCode, newCode, 6);
  }
}

void TotpTools::draw(GFXcanvas16& canvas) {
  canvas.fillScreen(0x0000);

  // Title
  gfxText(canvas, canvas.width() / 2, 25, "GitHub", COL_WHITE, 0x0000, 2, true);

  // Big TOTP Code
  gfxText(canvas, canvas.width() / 2, canvas.height() / 2 + 5, currentCode, COL_CYAN, 0x0000, 4, true);

  // Expiration bar
  int remaining = 30 - (simulatedTime % 30);
  int barWidth = (canvas.width() - 20) * remaining / 30;

  uint16_t barCol = (remaining < 5) ? COL_RED : COL_GREEN;

  canvas.drawRect(10, canvas.height() - 25, canvas.width() - 20, 10, 0xFFFF);
  canvas.fillRect(10, canvas.height() - 25, barWidth, 10, barCol);
}

#endif
