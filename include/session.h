#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Portable charger session: energy, peaks, sparklines, link/load phase.
// No Arduino. Host-tested in tests/test_session.cpp.

struct ChargeSample {
  uint16_t vout_mv = 0;
  uint16_t ia_ma = 0;
  uint16_t ic_ma = 0;
  float power_a_w = 0;
  float power_c_w = 0;
  float power_total_w = 0;
};

enum class Link : uint8_t { Ok = 0, Lost = 1 };
enum class Phase : uint8_t { Idle = 0, Running = 1, Paused = 2 };

struct PortStats {
  float peakA = 0;
  float peakW = 0;
  float ampsAtPeakW = 0;
};

struct Session {
  static constexpr size_t kHistMax = 120;
  static constexpr float kLoadMa = 50.0f;
  static constexpr uint32_t kEndDebounceMs = 1500;
  static constexpr uint32_t kHistPeriodInitMs = 250;

  Phase phase = Phase::Idle;
  uint32_t startMs = 0;
  uint32_t lastSampleMs = 0;
  uint32_t chargedMs = 0;
  double mwh = 0;
  float peakW = 0;
  float peakA = 0;
  PortStats portC;
  PortStats portA;
  uint16_t peakVoutMv = 0;
  bool dirty = false;

  float histC[kHistMax]{};
  float histA[kHistMax]{};
  size_t histCount = 0;
  uint32_t histPeriodMs = kHistPeriodInitMs;

  void onTick(uint32_t now, const ChargeSample* sample, Link link);
  void pushHistory(float powerC, float powerA);
  void clear();
  float avgW() const;
  bool hasData() const { return phase != Phase::Idle || mwh > 0.01 || histCount > 0; }
  // After filling public fields from NVS: pause the clock, do not integrate a gap.
  void adoptPersisted();

 private:
  uint32_t loadGoneSince_ = 0;
  bool haveSample_ = false;

  static bool loaded(const ChargeSample& s) {
    return s.ia_ma > kLoadMa || s.ic_ma > kLoadMa;
  }
  void resetTotals();
  void notePeaks(const ChargeSample& s);
  void integrate(uint32_t now, const ChargeSample& s);
  void downsample();
};
