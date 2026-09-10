#include "session.h"

void Session::resetTotals() {
  chargedMs = 0;
  mwh = 0;
  peakW = 0;
  peakA = 0;
  portC = PortStats{};
  portA = PortStats{};
  peakVoutMv = 0;
  histCount = 0;
  histPeriodMs = kHistPeriodInitMs;
  memset(histC, 0, sizeof(histC));
  memset(histA, 0, sizeof(histA));
}

void Session::notePeaks(const ChargeSample& s) {
  if (s.power_total_w > peakW) peakW = s.power_total_w;
  const float aTot = (s.ia_ma + s.ic_ma) / 1000.0f;
  if (aTot > peakA) peakA = aTot;
  const float cA = s.ic_ma / 1000.0f;
  const float aA = s.ia_ma / 1000.0f;
  if (cA > portC.peakA) portC.peakA = cA;
  if (aA > portA.peakA) portA.peakA = aA;
  if (s.power_c_w > portC.peakW) {
    portC.peakW = s.power_c_w;
    portC.ampsAtPeakW = cA;
  }
  if (s.power_a_w > portA.peakW) {
    portA.peakW = s.power_a_w;
    portA.ampsAtPeakW = aA;
  }
  if (s.vout_mv > peakVoutMv) peakVoutMv = s.vout_mv;
}

void Session::integrate(uint32_t now, const ChargeSample& s) {
  if (haveSample_ && now > lastSampleMs) {
    const uint32_t dt = now - lastSampleMs;
    mwh += static_cast<double>(s.power_total_w) * 1000.0 * (dt / 3600000.0);
    chargedMs += dt;
  }
  lastSampleMs = now;
  haveSample_ = true;
}

void Session::downsample() {
  const size_t half = kHistMax / 2;
  for (size_t i = 0; i < half; i++) {
    histC[i] = (histC[2 * i] + histC[2 * i + 1]) * 0.5f;
    histA[i] = (histA[2 * i] + histA[2 * i + 1]) * 0.5f;
  }
  histCount = half;
  histPeriodMs *= 2;
}

void Session::pushHistory(float powerC, float powerA) {
  if (histCount >= kHistMax) downsample();
  histC[histCount] = powerC;
  histA[histCount] = powerA;
  histCount++;
  dirty = true;
}

float Session::avgW() const {
  if (chargedMs < 50 || mwh <= 0) return 0.f;
  return static_cast<float>(mwh * 3600.0 / chargedMs);
}

void Session::adoptPersisted() {
  lastSampleMs = 0;
  loadGoneSince_ = 0;
  haveSample_ = false;
  dirty = false;
  if (mwh > 0.01 || histCount > 0 || chargedMs > 0) {
    phase = Phase::Paused;
    if (startMs == 0) startMs = 1;
  } else {
    phase = Phase::Idle;
  }
}

void Session::clear() {
  phase = Phase::Idle;
  startMs = 0;
  lastSampleMs = 0;
  loadGoneSince_ = 0;
  haveSample_ = false;
  dirty = false;
  resetTotals();
}

void Session::onTick(uint32_t now, const ChargeSample* sample, Link link) {
  if (link == Link::Lost) {
    // Freeze the clock. Do not bill the outage and do not treat this as unplug.
    if (phase == Phase::Running) phase = Phase::Paused;
    loadGoneSince_ = 0;
    return;
  }

  if (!sample) return;

  if (loaded(*sample)) {
    loadGoneSince_ = 0;
    if (phase == Phase::Idle) {
      startMs = now;
      lastSampleMs = now;
      haveSample_ = true;
      resetTotals();
    } else if (phase != Phase::Running) {
      lastSampleMs = now;
      haveSample_ = true;
    } else {
      integrate(now, *sample);
    }
    phase = Phase::Running;
    notePeaks(*sample);
    dirty = true;
    return;
  }

  if (phase != Phase::Running) return;

  integrate(now, *sample);
  if (loadGoneSince_ == 0) loadGoneSince_ = now;
  if (now - loadGoneSince_ >= kEndDebounceMs) {
    phase = Phase::Paused;
    dirty = true;
  }
}
