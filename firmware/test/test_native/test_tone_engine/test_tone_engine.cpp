// Native unit tests for ToneEngine — the 9-step tone ladder.
//
// Ladder v2 (user, desk test 2026-11-13): the old darkest rung ("deepest
// brown", brown + a 180 Hz low-pass) sounded best, so it is now the MIDDLE
// step (4), with four lighter and four darker rungs: second low-pass at
// 1000, 640, 400, 260, [180], 125, 90, 65, 45 Hz. Grey left the ladder but
// stays available as a custom rung (for a future "tone family" key).
// Properties under test:
//   - every step plays at the same RMS (no loudness jump between steps);
//   - every step is measurably darker than the one before (spectral
//     centroid at least 8% lower per step);
//   - grey (custom rung) follows inverse A-weighting (bass capped ~25 Hz);
//   - a step change crossfades at constant power (equal-power gains);
//   - deterministic for a seed; clipping vanishingly rare.

#include <unity.h>

#include <cmath>
#include <vector>

#include "../spectrum_helpers.h"
#include "tone_engine.h"

using spectrum::bandMean;
using spectrum::db;

void setUp() {}
void tearDown() {}

namespace {

double rmsOf(ToneEngine& e, size_t frames) {
    std::vector<float> buf(4096 * 2);
    double sum = 0;
    size_t done = 0;
    while (done < frames) {
        const size_t n = std::min<size_t>(4096, frames - done);
        e.render(buf.data(), n);
        for (size_t i = 0; i < 2 * n; ++i) sum += double(buf[i]) * buf[i];
        done += n;
    }
    return std::sqrt(sum / (2.0 * frames));
}

// Spectral centroid (power-weighted mean frequency, 20 Hz-8 kHz). Once the
// second corner drops below ~100 Hz a treble/mid ratio stops moving, but the
// centroid keeps falling, so it is the darkness measure. 16k-point FFT for
// enough resolution at the bottom of the range.
double centroid(int step) {
    ToneEngine e(step, 77);
    const auto p = spectrum::psd(e, 60, 16384);
    const double df = spectrum::kFs / 16384.0;
    double num = 0, den = 0;
    for (size_t k = 1; k < p.size(); ++k) {
        const double f = k * df;
        if (f < 20.0 || f > 8000.0) continue;
        num += f * p[k];
        den += p[k];
    }
    return num / den;
}

// Analog target for grey: inverse A-weighting with A's four DC zeros replaced
// by poles at 25 Hz (caps the bass boost). Magnitude squared, relative.
double greyTarget(double f) {
    const double zeros[] = {20.6, 20.6, 107.7, 737.9, 12194.0, 12194.0};
    double h = 1.0;
    for (double z : zeros) h *= f * f + z * z;
    for (int i = 0; i < 4; ++i) h /= f * f + 25.0 * 25.0;
    return h;
}

double greyTargetBand(double f0, double f1) {
    double s = 0;
    const int n = 200;
    for (int i = 0; i < n; ++i) s += greyTarget(f0 + (f1 - f0) * (i + 0.5) / n);
    return s / n;
}

}  // namespace

static void test_ladder_shape_and_names() {
    TEST_ASSERT_EQUAL(9, kToneSteps);
    TEST_ASSERT_EQUAL(4, kDefaultTone);
    TEST_ASSERT_EQUAL_STRING("lightest", toneName(0));
    TEST_ASSERT_EQUAL_STRING("middle", toneName(4));
    TEST_ASSERT_EQUAL_STRING("darkest", toneName(8));
}

static void test_step_is_clamped() {
    ToneEngine e(kDefaultTone, 1);
    e.setStep(-3);
    TEST_ASSERT_EQUAL(0, e.step());
    e.setStep(42);
    TEST_ASSERT_EQUAL(kToneSteps - 1, e.step());
}

static void test_every_step_plays_at_the_target_rms() {
    for (int s = 0; s < kToneSteps; ++s) {
        ToneEngine e(s, 5);
        const double r = rmsOf(e, 20 * 44100);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05 * kToneTargetRms, kToneTargetRms, r, toneName(s));
    }
}

static void test_each_step_is_darker_than_the_last() {
    double prev = centroid(0);
    for (int s = 1; s < kToneSteps; ++s) {
        const double c = centroid(s);
        TEST_ASSERT_TRUE_MESSAGE(c < 0.92 * prev, toneName(s));  // >= 8% lower per step
        prev = c;
    }
}

static void test_middle_step_is_the_old_favourite() {
    // The rung the user liked best on 2026-11-13: brown + 180 Hz low-pass,
    // centroid ~117 Hz (analytic). Guards against the ladder drifting.
    const double c = centroid(kDefaultTone);
    TEST_ASSERT_FLOAT_WITHIN(12.0, 117.0, c);
}

static void test_grey_follows_inverse_a_weighting() {
    ToneEngine e(kGreyRung, 9);
    const auto p = spectrum::psd(e, 300);
    const double ref = bandMean(p, 900, 1100) / greyTargetBand(900, 1100);
    for (double f : {63.0, 125.0, 250.0, 500.0, 2000.0, 4000.0}) {
        const double f0 = f / std::sqrt(2.0), f1 = f * std::sqrt(2.0);
        const double err = db(bandMean(p, f0, f1) / greyTargetBand(f0, f1) / ref);
        TEST_ASSERT_FLOAT_WITHIN(1.5, 0.0, err);
    }
}

static void test_plain_brown_rung_matches_noise_synth_brown_slope() {
    ToneEngine e(kBrownRung, 11);
    const auto p = spectrum::psd(e, 200);
    const double slope = db(bandMean(p, 2000, 4000) / bandMean(p, 1000, 2000));
    TEST_ASSERT_FLOAT_WITHIN(0.7, -6.02, slope);
}

static void test_custom_rungs_play_at_the_target_rms() {
    ToneEngine g(kGreyRung, 21), b(kBrownRung, 22);
    TEST_ASSERT_FLOAT_WITHIN(0.05 * kToneTargetRms, kToneTargetRms, rmsOf(g, 20 * 44100));
    TEST_ASSERT_FLOAT_WITHIN(0.05 * kToneTargetRms, kToneTargetRms, rmsOf(b, 20 * 44100));
}

static void test_crossfade_gains_are_equal_power_and_monotonic() {
    float a = 0, b = 0, prevA = 2, prevB = -1;
    for (uint32_t i = 0; i <= 1000; ++i) {
        crossfadeGains(i, 1000, a, b);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, a * a + b * b);
        TEST_ASSERT_TRUE(a <= prevA && b >= prevB);
        prevA = a;
        prevB = b;
    }
    crossfadeGains(0, 1000, a, b);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, a);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b);
    crossfadeGains(1000, 1000, a, b);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, a);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 1.0f, b);
}

static void test_crossfade_completes_and_level_holds() {
    // RMS across the crossfade, averaged over seeds, stays at the target.
    double sum = 0;
    const int seeds = 8;
    for (int s = 0; s < seeds; ++s) {
        ToneEngine e(kDefaultTone, 100 + s);
        rmsOf(e, 44100);
        e.setStep(0);  // a big jump on the ladder
        TEST_ASSERT_TRUE(e.crossfading());
        sum += rmsOf(e, kToneCrossfadeFrames);
        TEST_ASSERT_FALSE(e.crossfading());
    }
    TEST_ASSERT_FLOAT_WITHIN(0.08 * kToneTargetRms, kToneTargetRms, sum / seeds);
}

static void test_same_seed_is_deterministic() {
    ToneEngine a(2, 42), b(2, 42);
    std::vector<float> x(2048), y(2048);
    a.render(x.data(), 1024);
    b.render(y.data(), 1024);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(x.data(), y.data(), 2048);
}

static void test_bounded_and_clipping_vanishingly_rare() {
    for (int s : {0, 4, 8}) {
        ToneEngine e(s, 3);
        std::vector<float> buf(4096 * 2);
        const size_t frames = 300 * 44100;  // 5 minutes
        for (size_t done = 0; done < frames; done += 4096) {
            e.render(buf.data(), 4096);
            for (float v : buf) {
                if (v > 1.0f || v < -1.0f) TEST_FAIL_MESSAGE(toneName(s));
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(double(e.clippedSamples()) / (2.0 * frames) < 1e-6, toneName(s));
    }
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_ladder_shape_and_names);
    RUN_TEST(test_step_is_clamped);
    RUN_TEST(test_every_step_plays_at_the_target_rms);
    RUN_TEST(test_each_step_is_darker_than_the_last);
    RUN_TEST(test_grey_follows_inverse_a_weighting);
    RUN_TEST(test_middle_step_is_the_old_favourite);
    RUN_TEST(test_plain_brown_rung_matches_noise_synth_brown_slope);
    RUN_TEST(test_custom_rungs_play_at_the_target_rms);
    RUN_TEST(test_crossfade_gains_are_equal_power_and_monotonic);
    RUN_TEST(test_crossfade_completes_and_level_holds);
    RUN_TEST(test_same_seed_is_deterministic);
    RUN_TEST(test_bounded_and_clipping_vanishingly_rare);
    return UNITY_END();
}
