# sw3518-GEEK

Live USB charger stats on a [Waveshare ESP32-S3-GEEK](https://www.waveshare.com/wiki/ESP32-S3-GEEK) display, reading an ISmartWare **SW3518 / SW3518S** dual-port (USB-C + USB-A) module over I2C.

## What you get

- **Main**: total W, Vin/Vout, C/A amps, active **protocol** (reg `0x06`), session strip (duration / peak W / mWh), dual C/A power sparklines
- **USB-C / USB-A pages**: big V/A/W, per-port peak A, sparkline
- Protocol chip **flashes** for 2s when the negotiated protocol changes
- **Night mode**: backlight dims after 45s idle; any button wakes it
- Optional **Wi‑Fi → MQTT** for Home Assistant (topics under `geek/sw3518/...`)
- Optional **TF card** CSV logger (`/sw3518.csv`) while a load is present
- Serial monitor at `115200`

## Wiring (GEEK 4-pin I2C header → SW3518)

| GEEK header | GPIO | SW3518 |
|-------------|------|--------|
| 3V3         | —    | VDD / logic 3.3 V (if exposed) |
| GND         | —    | GND (common ground required) |
| SDA         | **16** | SDA |
| SCL         | **17** | SCK / SCL |

Notes:

- Use the **onboard I2C port**, not the UART or GPIO ADC header.
- SW3518 I2C is documented at **100k / 400k**; firmware uses 100 kHz.
- If your module’s I2C lines are 5 V only, add a **level shifter**. Do not drive 5 V into the ESP32-S3.
- Shared ground between charger module and GEEK is mandatory.

### Why SDA=16 / SCL=17?

The PCB schematic breaks the I2C header out on **GPIO16 / GPIO17**. Some Waveshare Arduino BME demos list `PIN_SDA=7` / `PIN_SCL=8`, but **GPIO8 is the LCD DC pin**, so those demo defines conflict with the display. This project follows the schematic / Espressif board support map.

## Display pins (onboard ST7789)

| Signal | GPIO |
|--------|------|
| MOSI   | 11 |
| SCLK   | 12 |
| CS     | 10 |
| DC     | 8 |
| RST    | 9 |
| BL     | 7 (active high) |

## SW3518 I2C cheat sheet

- Slave address: **`0x3C`**
- Select ADC channel → write register **`0x3A`**:
  - `1` Vin (10 mV/step)
  - `2` Vout (6 mV/step)
  - `3` Type-A current (2.5 mA/step)
  - `4` Type-C current (2.5 mA/step)
- Read 12-bit latch: **`0x3B`** (high 8) + **`0x3C`** (low 4) → `raw = (H << 4) | (L & 0x0F)`
- Protocol status: **`0x06`** `fcx_ind` (QC2/QC3/FCP/SCP/PD FIX/PD PPS/…); bits 5-4 = PD 2.0/3.0 when applicable

Driver lives in `include/sw3518.h` + `src/sw3518.cpp` (minimal ADC path; register notes from iSmartWare datasheet / RG003).

## Blank screen after flash?

Rebuild with latest `main` (needs `CGRAM_OFFSET` + inversion for the 135×240 ST7789). You should see a brief red→green flash on boot. If still black: hold BOOT, reflash, and watch serial at 115200.

## Build & flash (PlatformIO)

```bash
git pull
pio run -e esp32-s3-geek -t upload
pio device monitor -b 115200
```

Or build only: `pio run -e esp32-s3-geek`. Flash reminder: `git pull && pio run -e esp32-s3-geek -t upload`.

If upload fails: hold **BOOT**, plug USB-A into the PC, release BOOT (download mode), then upload again. Enable USB CDC is already set in `platformio.ini`.

Board selection in Arduino IDE (if you prefer): **ESP32S3 Dev Module**, flash 16 MB, PSRAM enabled, **USB CDC On Boot = Enabled**.

## Controls (single BOOT button)

Top **status bar** (always on): `CHG`/`RAD` mode chip, page crumbs with active underline, and Wi‑Fi/MQTT/web icons. Main also shows a **C/A load-share** bar.

Reserved GPIOs for a future case: `PIN_BTN_LEFT=1`, `PIN_BTN_RIGHT=2`, `PIN_HAPTIC=13`.


**Charger mode** (default)

- **Short:** zoom cycle — Main → USB-C → Main → USB-A → Main → Session → Main
- **Double:** Session stats (again returns to Main)
- **Triple:** enter **Radio** mode
- **Long:** clear session (new connection)
- **Idle dim:** after 90s, backlight to 50% (any press restores)

**Radio mode** (Wi-Fi tools - beacon scan / channel heat; not an attack suite)

- **Short:** next page - Wi-Fi APs -> Waterfall -> BLE -> System -> Help -> ...
- **Double:** previous radio page
- **Triple:** back to **Charger**
- **Long:** force Wi-Fi rescan


**HID mode** (KeyMod-inspired USB + BLE keyboard/mouse — not video KVM)

- **Triple BOOT** cycles **Charger → Radio → HID → Charger**
- USB-A enumerates as **CDC + HID** keyboard/mouse (replug host after flash if needed)
- BLE advertises as a KeyboardMouse combo (pair in OS Bluetooth settings)
- Pages: Status · Keys · Mouse · Macros · Help
- **Short:** next page · **Double:** prev (or Esc / right-click on Keys/Mouse) · **Long:** page action (Enter / left-click / run macro)
- Side buttons (GPIO1/2 if wired): arrows or mouse nudge

**System page** is a compact dashboard under the status bar: Wi-Fi SSID/RSSI bar/channel/IP, MQTT + web server status, heap (free + min) and PSRAM bars, uptime, real loop load (last loop us + loops/s - not fake CPU%), and AP/BLE scan counts. Cyan/yellow/magenta accents match the charger UI.

**Waterfall** rolls channels 1-13 (~600 ms dwell) with per-channel `WiFi.scanNetworks` when the API allows (beacon/scan only - no promiscuous sniff). Heat columns scroll each dwell; color by RSSI intensity (cool dim -> hot magenta/cyan) with open-vs-encrypted tint from scan `encryptionType`. Caption is **ASCII-only** (Adafruit font). Shows dwell CH and hottest AP on that channel.

Web: `/` charger - `/radio` AP JSON view - `/help` button map - `/api` metrics

SSI bar/channel/IP, MQTT + web server status, heap (free + min) and PSRAM bars, uptime, real loop load (last loop us + loops/s - not fake CPU%), and AP/BLE scan counts. Cyan/yellow/magenta accents match the charger UI.

**Waterfall** rolls channels 1-13 (~600 ms dwell) with per-channel `WiFi.scanNetworks` when the API allows (beacon/scan only - no promiscuous sniff). Heat columns scroll each dwell; color by RSSI intensity (cool dim -> hot magenta/cyan) with open-vs-encrypted tint from scan `encryptionType`. Caption is **ASCII-only** (Adafruit font). Shows dwell CH and hottest AP on that channel.

Reserved GPIOs for a future case: `PIN_BTN_LEFT=1`, `PIN_BTN_RIGHT=2`, `PIN_HAPTIC=13` (also noted under Controls).

Web: `/` charger - `/radio` AP JSON view - `/help` button map - `/api` metrics

USB-C/A pages show a session-length sparkline (grows / rebins to fit) with a time span label.

Session History (energy, peaks, sparklines) **persists across reboot** in flash (NVS). Long-hold clear wipes it.
While on Session, live SESSION shows for **60s**, then SAVED for **15s**, then repeats (no progress bar). Main/C/A are unchanged.


## Wi‑Fi / MQTT (Home Assistant)

1. `cp include/secrets.h.example include/secrets.h`
2. Fill `WIFI_*` and `MQTT_*` (broker can be HA’s Mosquitto / Tailscale IP)
3. Rebuild & flash

Not ESPHome — after Mosquitto is up, the GEEK publishes **Home Assistant MQTT discovery** and should appear as device **SW3518 GEEK** under Settings → Devices & services → MQTT.

State topics (retained) under `MQTT_BASE` (default `geek/sw3518`):

`vin`, `vout`, `i_c`, `i_a`, `power`, `power_c`, `power_a`, `protocol`, `session_mwh`, `session_wh` (Wh for HA Energy), `session_peak_w`, `charging`, `status` (`online`/`offline` LWT)

Discovery prefix defaults to `homeassistant` (override with `MQTT_DISCOVERY_PREFIX` in `secrets.h`).

## Web UI

With Wi‑Fi up, open `http://<device-ip>/` (JSON at `/api`). While a browser is hitting it, the on-screen **web** icon goes bright (Wi‑Fi bars + MQTT diamond sit beside it).

## TF card logging

Insert a FAT32 card. While charging, appends to `/sw3518.csv`:

`ms,vin_mv,vout_mv,ic_ma,ia_ma,power_w,protocol`

No card = silent skip.

## Repo status

Firmware is written for desk verification. After you solder/plug the SW3518, confirm `0x3C` shows up (overview should leave the “not found” screen) and spot-check Vin/Vout/current against a known load.

## License

Firmware in this repo: MIT (unless you later vendor GPL code — keep attributions). SW3518 is a product of Zhuhai iSmartWare; datasheets are theirs.

## Blank screen / silent serial

1. Hold **BOOT**, plug USB-A, release BOOT, then `pio run -e esp32-s3-geek -t upload`.
2. **Unplug and replug** after upload (Waveshare CDC tip) so the app USB device re-enumerates.
3. Monitor: `pio device monitor -b 115200` — you should see `ESP32-S3-GEEK SW3518 stats`.
4. On boot the backlight should **blink 4 times** even before the LCD init. No blink ⇒ firmware not running / wrong board / flash failed.
5. This build uses TinyUSB CDC (`ARDUINO_USB_MODE=0`) on the USB-A port (GPIO19/20).


## Case (3D print)

Parametric STLs for a GEEK shell with button / haptic / WS2812 pockets live in [`case/`](case/). Dry-fit before final print.
