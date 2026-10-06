#include "features.h"
#if HAS_TOTP

#include "totp_tools.h"
#include <TOTP.h>
#include "app_state.h"
#include "ui.h"
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <time.h>
#include <USB.h>
#include <USBHIDKeyboard.h>

static USBHIDKeyboard Keyboard;

struct TotpAccount {
  char name[32];
  uint8_t hmacKey[32];
  uint8_t hmacLen;
};

static TotpAccount accounts[10];
static int numAccounts = 0;
static int currentAccountIdx = 0;

static char currentCode[7] = "000000";
static uint32_t lastCodeMs = 0;
static bool timeSynced = false;

// We need base32 decoding for standard secrets.
// Minimal base32 decoder:
static int base32Decode(const char* encoded, uint8_t* decoded, int maxLen) {
  int buffer = 0;
  int bitsLeft = 0;
  int count = 0;
  for (const char* ptr = encoded; *ptr; ++ptr) {
    uint8_t ch = *ptr;
    if (ch == ' ' || ch == '-') continue;
    if (ch >= 'A' && ch <= 'Z') ch -= 'A';
    else if (ch >= 'a' && ch <= 'z') ch -= 'a';
    else if (ch >= '2' && ch <= '7') ch -= '2' - 26;
    else if (ch == '=') break; // padding
    else continue;

    buffer <<= 5;
    buffer |= ch;
    bitsLeft += 5;

    if (bitsLeft >= 8) {
      if (count < maxLen) {
        decoded[count++] = buffer >> (bitsLeft - 8);
      }
      bitsLeft -= 8;
    }
  }
  return count;
}

void TotpTools::begin() {
  Keyboard.begin();
  USB.begin();

  // Load from SD if available
  if (app.sdOk) {
    File f = SD.open("/totp.csv");
    if (f) {
      numAccounts = 0;
      while (f.available() && numAccounts < 10) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("#")) continue;
        int comma = line.indexOf(',');
        if (comma > 0) {
          String name = line.substring(0, comma);
          String secret = line.substring(comma + 1);
          strncpy(accounts[numAccounts].name, name.c_str(), 31);
          accounts[numAccounts].name[31] = '\0';
          accounts[numAccounts].hmacLen = base32Decode(secret.c_str(), accounts[numAccounts].hmacKey, 32);
          if (accounts[numAccounts].hmacLen > 0) {
            numAccounts++;
          }
        }
      }
      f.close();
    }
  }

  // Fallback default if empty
  if (numAccounts == 0) {
    strcpy(accounts[0].name, "Demo GitHub");
    // Secret: JBSWY3DPEHPK3PXP -> 48 65 6c 6c 6f 21 0xde 0xad 0xbe 0xef
    uint8_t defKey[] = {0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x21, 0xde, 0xad, 0xbe, 0xef};
    memcpy(accounts[0].hmacKey, defKey, 10);
    accounts[0].hmacLen = 10;
    numAccounts = 1;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

bool TotpTools::hasTimeSync() {
  return timeSynced;
}

void TotpTools::nextAccount() {
  currentAccountIdx = (currentAccountIdx + 1) % numAccounts;
  lastCodeMs = 0; // force redraw
}

void TotpTools::prevAccount() {
  currentAccountIdx = (currentAccountIdx - 1 + numAccounts) % numAccounts;
  lastCodeMs = 0; // force redraw
}

void TotpTools::typeCurrentCode() {
  if (!timeSynced) return;
  for (int i=0; i<6; i++) {
    Keyboard.print(currentCode[i]);
  }
  Keyboard.write('\n'); // press enter
  uiShowModeToast("TYPED");
}

void TotpTools::tick(uint32_t now) {
  if (!timeSynced) {
    time_t t;
    time(&t);
    if (t > 1600000000) { // arbitrary recent timestamp
      timeSynced = true;
      Serial.println("Time synced from NTP.");
    }
  }

  if (timeSynced) {
    if (now - lastCodeMs >= 1000) {
      lastCodeMs = now;
      time_t t;
      time(&t);
      TOTP totp = TOTP(accounts[currentAccountIdx].hmacKey, accounts[currentAccountIdx].hmacLen);
      strncpy(currentCode, totp.getCode(t), 6);
    }
  }
}

void TotpTools::draw(GFXcanvas16& canvas) {
  canvas.fillScreen(0x0000);

  // Title
  gfxText(canvas, canvas.width() / 2, 25, accounts[currentAccountIdx].name, COL_WHITE, 0x0000, 2, true);

  if (!timeSynced) {
    gfxText(canvas, canvas.width() / 2, canvas.height() / 2 + 5, "Syncing time...", COL_DIM, 0x0000, 2, true);
    return;
  }

  // Big TOTP Code
  gfxText(canvas, canvas.width() / 2, canvas.height() / 2 + 5, currentCode, COL_CYAN, 0x0000, 4, true);

  // Expiration bar
  time_t t;
  time(&t);
  int remaining = 30 - (t % 30);
  int barWidth = (canvas.width() - 20) * remaining / 30;

  uint16_t barCol = (remaining < 5) ? COL_RED : COL_GREEN;

  canvas.drawRect(10, canvas.height() - 25, canvas.width() - 20, 10, 0xFFFF);
  canvas.fillRect(10, canvas.height() - 25, barWidth, 10, barCol);

  // Dots for accounts
  if (numAccounts > 1) {
    int startX = (canvas.width() - (numAccounts * 8)) / 2;
    for (int i=0; i<numAccounts; i++) {
       canvas.fillCircle(startX + i * 8, canvas.height() - 5, 2, (i == currentAccountIdx) ? COL_WHITE : COL_DIM);
    }
  }
}

#endif
