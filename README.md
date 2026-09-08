# sw3518-GEEK

Live USB charger stats on a [Waveshare ESP32-S3-GEEK](https://www.waveshare.com/wiki/ESP32-S3-GEEK) display, reading an ISmartWare **SW3518 / SW3518S** dual-port (USB-C + USB-A) module over I2C.

## What you get

- Overview page: total watts, Vin / Vout, Type-C and Type-A current & power
- Detail page: millivolt / milliamp numbers
- BOOT button: short press = switch pages, long press = backlight on/off
- Serial monitor at `115200` with the same readings

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

Driver lives in `include/sw3518.h` + `src/sw3518.cpp` (minimal ADC path; register notes from iSmartWare datasheet / RG003).

## Build & flash (PlatformIO)

```bash
pio run -e esp32-s3-geek
pio run -e esp32-s3-geek -t upload
pio device monitor -b 115200
```

If upload fails: hold **BOOT**, plug USB-A into the PC, release BOOT (download mode), then upload again. Enable USB CDC is already set in `platformio.ini`.

Board selection in Arduino IDE (if you prefer): **ESP32S3 Dev Module**, flash 16 MB, PSRAM enabled, **USB CDC On Boot = Enabled**.

## Controls

| Action | Result |
|--------|--------|
| BOOT short press | Toggle overview ↔ detail |
| BOOT long press | Toggle backlight |

## Repo status

Firmware is written for desk verification. After you solder/plug the SW3518, confirm `0x3C` shows up (overview should leave the “not found” screen) and spot-check Vin/Vout/current against a known load.

## License

Firmware in this repo: MIT (unless you later vendor GPL code — keep attributions). SW3518 is a product of Zhuhai iSmartWare; datasheets are theirs.
