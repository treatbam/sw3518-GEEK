#pragma once

#include <cstdint>

// SW3518 ADC latch scaling (RG003). Host-tested; used by the I2C driver.

inline uint16_t sw3518VinMv(uint16_t raw12) { return static_cast<uint16_t>(raw12 * 10u); }
inline uint16_t sw3518VoutMv(uint16_t raw12) { return static_cast<uint16_t>(raw12 * 6u); }
inline uint16_t sw3518IoutMa(uint16_t raw12) { return static_cast<uint16_t>((raw12 * 25u) / 10u); }

inline uint16_t sw3518PackAdc(uint8_t hi, uint8_t lo) {
  return (static_cast<uint16_t>(hi) << 4) | static_cast<uint16_t>(lo & 0x0F);
}

inline uint16_t sw3518PackVinCont(uint8_t vin_h, uint8_t vin_vout_l) {
  return (static_cast<uint16_t>(vin_h) << 4) | static_cast<uint16_t>(vin_vout_l >> 4);
}
