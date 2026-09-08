#include "sw3518.h"

namespace {
constexpr uint8_t REG_FCX_STATUS = 0x06;
constexpr uint8_t REG_I2C_CTRL = 0x13;
constexpr uint8_t REG_ADC_VIN_H = 0x30;
constexpr uint8_t REG_ADC_VOUT_H = 0x31;
constexpr uint8_t REG_ADC_VIN_VOUT_L = 0x32;
constexpr uint8_t REG_ADC_TYPE = 0x3A;
constexpr uint8_t REG_ADC_H = 0x3B;
constexpr uint8_t REG_ADC_L = 0x3C;

constexpr uint8_t ADC_VIN = 1;
constexpr uint8_t ADC_VOUT = 2;
// Datasheet labels vs this board: user observed A/C swapped at types 3/4.
constexpr uint8_t ADC_IOUT_A = 4;
constexpr uint8_t ADC_IOUT_C = 3;

// REG 0x13 bit1: Vin ADC must be enabled or Vin reads as 0 (RG003).
constexpr uint8_t I2C_CTRL_VIN_ADC_EN = 0x02;
}  // namespace

bool SW3518::begin(int sda, int scl, uint32_t hz) {
  wire_.begin(sda, scl, hz);
  delay(20);
  present_ = probe();
  if (present_) {
    enableVinAdc();
    delay(5);
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
  // Absolute write matches happyme531 / VFDclock; RMW can leave enable clear
  // if a prior read failed. Bit0 is one-shot PDO rebroadcast — leave it 0.
  return writeReg(REG_I2C_CTRL, I2C_CTRL_VIN_ADC_EN);
}

bool SW3518::readAdc(uint8_t type, uint16_t& raw) {
  if (!writeReg(REG_ADC_TYPE, type)) return false;
  delay(2);
  uint8_t hi = 0, lo = 0;
  if (!readReg(REG_ADC_H, hi)) return false;
  if (!readReg(REG_ADC_L, lo)) return false;
  raw = (static_cast<uint16_t>(hi) << 4) | (lo & 0x0F);
  return true;
}

bool SW3518::readVinMv(uint16_t& out) {
  // Datasheet: Vin ADC only updates while REG0x13 bit1 is set.
  // Re-arm every read — some modules drop the bit after latch/other writes.
  enableVinAdc();
  delay(2);

  // Prefer continuous Vin/Vout registers (default path in h1_SW35xx).
  uint8_t vin_h = 0, vin_vout_l = 0;
  if (readReg(REG_ADC_VIN_H, vin_h) && readReg(REG_ADC_VIN_VOUT_L, vin_vout_l)) {
    const uint16_t raw =
        (static_cast<uint16_t>(vin_h) << 4) | static_cast<uint16_t>(vin_vout_l >> 4);
    if (raw != 0) {
      out = raw * 10;  // 10 mV/LSB
      return true;
    }
  }

  // Fall back to type-latch buffer (ADC type 1).
  uint16_t raw = 0;
  if (!readAdc(ADC_VIN, raw)) return false;
  out = raw * 10;
  return true;
}

bool SW3518::readVoutMv(uint16_t& out) {
  uint16_t raw = 0;
  if (!readAdc(ADC_VOUT, raw)) return false;
  out = raw * 6;
  return true;
}

bool SW3518::readIoutAMa(uint16_t& out) {
  uint16_t raw = 0;
  if (!readAdc(ADC_IOUT_A, raw)) return false;
  out = (raw * 25) / 10;
  return true;
}

bool SW3518::readIoutCMa(uint16_t& out) {
  uint16_t raw = 0;
  if (!readAdc(ADC_IOUT_C, raw)) return false;
  out = (raw * 25) / 10;
  return true;
}

bool SW3518::readProtocol(Protocol& out, uint8_t& pd_ver) {
  uint8_t st = 0;
  if (!readReg(REG_FCX_STATUS, st)) return false;
  pd_ver = (st >> 4) & 0x03;
  const uint8_t ind = st & 0x0F;
  if (ind <= 0x0B) out = static_cast<Protocol>(ind);
  else out = Protocol::Unknown;
  return true;
}

const char* SW3518::protocolName(Protocol p) {
  switch (p) {
    case Protocol::None: return "5V/DCP";
    case Protocol::QC2: return "QC2.0";
    case Protocol::QC3: return "QC3.0";
    case Protocol::FCP: return "FCP";
    case Protocol::SCP: return "SCP";
    case Protocol::PdFix: return "PD FIX";
    case Protocol::PdPps: return "PD PPS";
    case Protocol::PE11: return "PE1.1";
    case Protocol::PE20: return "PE2.0";
    case Protocol::LVDC: return "LVDC";
    case Protocol::SFCP: return "SFCP";
    case Protocol::AFC: return "AFC";
    default: return "?";
  }
}

bool SW3518::readSnapshot(Snapshot& s) {
  s.ok = false;
  if (!readVinMv(s.vin_mv)) return false;
  if (!readVoutMv(s.vout_mv)) return false;
  if (!readIoutAMa(s.ia_ma)) return false;
  if (!readIoutCMa(s.ic_ma)) return false;
  if (!readProtocol(s.protocol, s.pd_ver)) {
    s.protocol = Protocol::Unknown;
    s.pd_ver = 0;
  }
  s.power_a_w = (s.vout_mv / 1000.0f) * (s.ia_ma / 1000.0f);
  s.power_c_w = (s.vout_mv / 1000.0f) * (s.ic_ma / 1000.0f);
  s.power_total_w = s.power_a_w + s.power_c_w;
  s.ok = true;
  return true;
}
