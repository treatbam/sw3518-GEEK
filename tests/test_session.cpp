// Host tests for Session + SW3518 ADC scale (no Arduino).
// Build: make -C tests test

#include <cmath>
#include <cstdio>
#include <cstdint>

#include "session.h"
#include "sw3518_scale.h"

static int g_fail = 0;
static int g_pass = 0;

#define EXPECT(cond, msg)                           \
  do {                                              \
    if (cond) {                                     \
      g_pass++;                                     \
    } else {                                        \
      g_fail++;                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); \
    }                                               \
  } while (0)

#define EXPECT_NEAR(a, b, eps, msg) EXPECT(std::fabs((double)(a) - (double)(b)) < (eps), msg)

static ChargeSample load10W() {
  ChargeSample s;
  s.vout_mv = 5000;
  s.ic_ma = 1000;
  s.ia_ma = 1000;
  s.power_c_w = 5.f;
  s.power_a_w = 5.f;
  s.power_total_w = 10.f;
  return s;
}

static ChargeSample idle() {
  ChargeSample s;
  s.vout_mv = 5000;
  return s;
}

static void test_scale() {
  EXPECT(sw3518VinMv(1000) == 10000, "vin 10mV/LSB");
  EXPECT(sw3518VoutMv(1000) == 6000, "vout 6mV/LSB");
  EXPECT(sw3518IoutMa(400) == 1000, "iout 2.5mA/LSB");
  EXPECT(sw3518PackAdc(0x12, 0x0F) == 0x12F, "adc latch pack");
  EXPECT(sw3518PackVinCont(0x12, 0xF0) == 0x12F, "vin continuous pack");
}

static void test_energy_dt() {
  Session s;
  const ChargeSample sample = load10W();
  s.onTick(0, &sample, Link::Ok);
  s.onTick(3600, &sample, Link::Ok);
  EXPECT(s.phase == Phase::Running, "running after load");
  EXPECT_NEAR(s.mwh, 10.0, 0.01, "10W * 3600ms = 10 mWh");
  EXPECT(s.chargedMs == 3600, "chargedMs tracks load dt");
  EXPECT_NEAR(s.avgW(), 10.0, 0.01, "avg W = 10");
}

static void test_lost_link_does_not_integrate_gap() {
  Session s;
  const ChargeSample sample = load10W();
  s.onTick(0, &sample, Link::Ok);
  s.onTick(3600, &sample, Link::Ok);
  EXPECT_NEAR(s.mwh, 10.0, 0.01, "pre-loss energy");

  s.onTick(3600, nullptr, Link::Lost);
  EXPECT(s.phase == Phase::Paused, "lost I2C pauses immediately");

  // 100 s outage at the same 10 W would add ~278 mWh if the gap were integrated.
  s.onTick(3600 + 100000, nullptr, Link::Lost);
  EXPECT_NEAR(s.mwh, 10.0, 0.01, "outage must not add energy");

  s.onTick(3600 + 100000, &sample, Link::Ok);
  EXPECT(s.phase == Phase::Running, "resume on load after link ok");
  s.onTick(3600 + 100000 + 3600, &sample, Link::Ok);
  EXPECT_NEAR(s.mwh, 20.0, 0.01, "only the two loaded intervals, not the gap");
  EXPECT(s.chargedMs == 7200, "chargedMs skips outage");
}

static void test_unplug_debounce() {
  Session s;
  const ChargeSample on = load10W();
  const ChargeSample off = idle();
  s.onTick(0, &on, Link::Ok);
  s.onTick(1000, &on, Link::Ok);
  s.onTick(1000, &off, Link::Ok);
  EXPECT(s.phase == Phase::Running, "still running during debounce");
  s.onTick(1000 + 1499, &off, Link::Ok);
  EXPECT(s.phase == Phase::Running, "debounce not elapsed");
  s.onTick(1000 + 1500, &off, Link::Ok);
  EXPECT(s.phase == Phase::Paused, "paused after 1500ms unplug");
  EXPECT(s.hasData(), "energy retained across pause");
}

static void test_lost_skips_unplug_debounce() {
  Session s;
  const ChargeSample on = load10W();
  s.onTick(0, &on, Link::Ok);
  s.onTick(500, &on, Link::Ok);
  s.onTick(500, nullptr, Link::Lost);
  EXPECT(s.phase == Phase::Paused, "link lost does not wait for unplug debounce");
}

static void test_idle_gap_on_resume_skipped() {
  Session s;
  const ChargeSample on = load10W();
  const ChargeSample off = idle();
  s.onTick(0, &on, Link::Ok);
  s.onTick(3600, &on, Link::Ok);
  s.onTick(3600, &off, Link::Ok);             // start unplug debounce
  s.onTick(3600 + 1500, &off, Link::Ok);      // elapsed
  EXPECT(s.phase == Phase::Paused, "paused");
  const double mwhPaused = s.mwh;
  s.onTick(3600 + 1500 + 60000, &on, Link::Ok);
  s.onTick(3600 + 1500 + 60000 + 3600, &on, Link::Ok);
  EXPECT_NEAR(s.mwh, mwhPaused + 10.0, 0.05, "paused idle gap not billed");
}

static void test_peaks() {
  Session s;
  ChargeSample sample = load10W();
  sample.ic_ma = 1500;
  sample.ia_ma = 500;
  sample.power_c_w = 7.5f;
  sample.power_a_w = 2.5f;
  sample.power_total_w = 10.f;
  s.onTick(0, &sample, Link::Ok);
  EXPECT_NEAR(s.portC.peakA, 1.5, 0.001, "C peak A");
  EXPECT_NEAR(s.portA.peakA, 0.5, 0.001, "A peak A");
  EXPECT_NEAR(s.portC.peakW, 7.5, 0.001, "C peak W");
  EXPECT_NEAR(s.portC.ampsAtPeakW, 1.5, 0.001, "amps at C peak W");
  EXPECT_NEAR(s.peakW, 10.0, 0.001, "total peak W");
}

static void test_clear() {
  Session s;
  const ChargeSample sample = load10W();
  s.onTick(0, &sample, Link::Ok);
  s.onTick(3600, &sample, Link::Ok);
  s.pushHistory(5.f, 5.f);
  s.clear();
  EXPECT(s.phase == Phase::Idle, "idle after clear");
  EXPECT(s.mwh == 0, "mwh cleared");
  EXPECT(s.histCount == 0, "hist cleared");
  EXPECT(!s.hasData(), "no data");
}

static void test_history_downsample() {
  Session s;
  for (size_t i = 0; i < Session::kHistMax + 1; i++) {
    s.pushHistory(2.f, 4.f);
  }
  EXPECT(s.histCount == Session::kHistMax / 2 + 1, "rebins then appends");
  EXPECT(s.histPeriodMs == 500, "period doubles");
}

static void test_adopt_persisted_does_not_bill_gap() {
  Session s;
  const ChargeSample sample = load10W();
  s.onTick(0, &sample, Link::Ok);
  s.onTick(3600, &sample, Link::Ok);
  Session blob = s;
  blob.adoptPersisted();
  EXPECT(blob.phase == Phase::Paused, "restored session is paused");
  blob.onTick(100000, &sample, Link::Ok);
  blob.onTick(100000 + 3600, &sample, Link::Ok);
  EXPECT_NEAR(blob.mwh, 20.0, 0.05, "restore must not integrate time since reboot");
}

int main() {
  test_scale();
  test_energy_dt();
  test_lost_link_does_not_integrate_gap();
  test_unplug_debounce();
  test_lost_skips_unplug_debounce();
  test_idle_gap_on_resume_skipped();
  test_peaks();
  test_clear();
  test_history_downsample();
  test_adopt_persisted_does_not_bill_gap();
  std::printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
