#pragma once
#include <Arduino.h>
#include <Wire.h>

// Minimal SW3518 / SW3518S I2C driver.
// Registers from iSmartWare RG003 / datasheet. Address 0x3C.
class SW3518 {
 public:
  static constexpr uint8_t kAddr = 0x3C;

  enum class Protocol : uint8_t {
    None = 0,
    QC2 = 1,
    QC3 = 2,
    FCP = 3,
    SCP = 4,
    PdFix = 5,
    PdPps = 6,
    PE11 = 7,
    PE20 = 8,
    LVDC = 9,
    SFCP = 10,
    AFC = 11,
    Unknown = 255,
  };

  explicit SW3518(TwoWire& wire = Wire) : wire_(wire) {}

  bool begin(int sda, int scl, uint32_t hz = 100000);
  bool present() const { return present_; }
  bool probe();

  bool readVinMv(uint16_t& out);
  bool readVoutMv(uint16_t& out);
  bool readIoutAMa(uint16_t& out);  // Type-A
  bool readIoutCMa(uint16_t& out);  // Type-C
  bool readProtocol(Protocol& out, uint8_t& pd_ver);

  static const char* protocolName(Protocol p);

  struct Snapshot {
    uint16_t vin_mv = 0;
    uint16_t vout_mv = 0;
    uint16_t ia_ma = 0;
    uint16_t ic_ma = 0;
    float power_a_w = 0;
    float power_c_w = 0;
    float power_total_w = 0;
    Protocol protocol = Protocol::None;
    uint8_t pd_ver = 0;  // 1=PD2.0, 2=PD3.0
    bool ok = false;
  };

  bool readSnapshot(Snapshot& s);

 private:
  TwoWire& wire_;
  bool present_ = false;

  bool writeReg(uint8_t reg, uint8_t val);
  bool readReg(uint8_t reg, uint8_t& val);
  bool readAdc(uint8_t type, uint16_t& raw);
  bool enableVinAdc();
};
