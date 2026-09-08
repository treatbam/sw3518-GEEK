#include "sw3518.h"

namespace {
constexpr uint8_t REG_I2C_CTRL = 0x13;
constexpr uint8_t REG_ADC_TYPE = 0x3A;
constexpr uint8_t REG_ADC_H = 0x3B;
constexpr uint8_t REG_ADC_L = 0x3C;

constexpr uint8_t ADC_VIN = 1;
constexpr uint8_t ADC_VOUT = 2;
constexpr uint8_t ADC_IOUT_A = 3;
constexpr uint8_t ADC_IOUT_C = 4;
}  // namespace

bool SW3518::begin(int sda, int scl, uint32_t hz) {
  wire_.begin(sda, scl, hz);
  delay(20);
  present_ = probe();
  if (present_) {
    enableVinAdc();
  }
  return present_;
}

bool SW3518::probe() {
  wire_.beginTransmission(kAddr);
  return wire_.endTransmission() == 0;
}

bool SW3518::writeReg(uint8_t reg, uint8_t val) {
  wire_.beginTransmission(kAddr);
  wire_.write(reg);
  wire_.write(val);
  return wire_.endTransmission() == 0;
}

bool SW3518::readReg(uint8_t reg, uint8_t& val) {
  wire_.beginTransmission(kAddr);
  wire_.write(reg);
  if (wire_.endTransmission(false) != 0) return false;
  if (wire_.requestFrom(static_cast<int>(kAddr), 1) != 1) return false;
  val = wire_.read();
  return true;
}

bool SW3518::enableVinAdc() {
  // Bit 1 of I2C_CTRL: reg_adc_vin_enable (per RG003). Read-modify-write.
  uint8_t ctrl = 0;
  if (!readReg(REG_I2C_CTRL, ctrl)) return false;
  ctrl |= 0x02;
  return writeReg(REG_I2C_CTRL, ctrl);
}

bool SW3518::readAdc(uint8_t type, uint16_t& raw) {
  if (!writeReg(REG_ADC_TYPE, type)) return false;
  delay(2);  // latch settle
  uint8_t hi = 0, lo = 0;
  if (!readReg(REG_ADC_H, hi)) return false;
  if (!readReg(REG_ADC_L, lo)) return false;
  raw = (static_cast<uint16_t>(hi) << 4) | (lo & 0x0F);
  return true;
}

bool SW3518::readVinMv(uint16_t& out) {
  uint16_t raw = 0;
  if (!readAdc(ADC_VIN, raw)) return false;
  out = raw * 10;  // 10 mV/step
  return true;
}

bool SW3518::readVoutMv(uint16_t& out) {
  uint16_t raw = 0;
  if (!readAdc(ADC_VOUT, raw)) return false;
  out = raw * 6;  // 6 mV/step
  return true;
}

bool SW3518::readIoutAMa(uint16_t& out) {
  uint16_t raw = 0;
  if (!readAdc(ADC_IOUT_A, raw)) return false;
  out = (raw * 25) / 10;  // 2.5 mA/step
  return true;
}

bool SW3518::readIoutCMa(uint16_t& out) {
  uint16_t raw = 0;
  if (!readAdc(ADC_IOUT_C, raw)) return false;
  out = (raw * 25) / 10;
  return true;
}

bool SW3518::readSnapshot(Snapshot& s) {
  s.ok = false;
  if (!readVinMv(s.vin_mv)) return false;
  if (!readVoutMv(s.vout_mv)) return false;
  if (!readIoutAMa(s.ia_ma)) return false;
  if (!readIoutCMa(s.ic_ma)) return false;
  s.power_a_w = (s.vout_mv / 1000.0f) * (s.ia_ma / 1000.0f);
  s.power_c_w = (s.vout_mv / 1000.0f) * (s.ic_ma / 1000.0f);
  s.power_total_w = s.power_a_w + s.power_c_w;
  s.ok = true;
  return true;
}
