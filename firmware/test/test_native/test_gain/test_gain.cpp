// Native unit tests for Gain: volume, fades and the volume ceiling.
//
// Bedroom safety rules (docs/decisions.md D8) live here:
//   - output starts silent and only gets louder through a fade;
//   - the ceiling is never exceeded, not even mid-ramp;
//   - volume changes ramp smoothly (no clicks), so a held volume key never
//     causes a jump.

#include <unity.h>

#include <cmath>
#include <vector>

#include "gain.h"

void setUp() {}
void tearDown() {}

namespace {

// Run `frames` frames of a constant input through g; return the per-frame gain
// trace (read back from the left-channel output).
std::vector<float> trace(Gain& g, size_t frames, float in = 1.0f) {
    std::vector<float> x(2 * frames, in);
    std::vector<int16_t> y(2 * frames);
    g.process(x.data(), y.data(), frames);
    std::vector<float> out(frames);
    for (size_t i = 0; i < frames; ++i) out[i] = y[2 * i] / 32767.0f;
    return out;
}

}  // namespace

static void test_gain_curve_endpoints_and_midpoint() {
    TEST_ASSERT_EQUAL_FLOAT(0.0f, Gain::gainForPercent(0));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, Gain::gainForPercent(100));
    // 50% = -25 dB on a 50 dB range.
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, std::pow(10.0f, -25.0f / 20.0f), Gain::gainForPercent(50));
}

static void test_gain_curve_is_strictly_increasing() {
    for (int p = 1; p <= 100; ++p) {
        TEST_ASSERT_TRUE(Gain::gainForPercent(p) > Gain::gainForPercent(p - 1));
    }
}

static void test_starts_silent() {
    Gain g;
    g.setVolume(80);
    TEST_ASSERT_TRUE(g.silent());
    for (float v : trace(g, 1000)) TEST_ASSERT_EQUAL_FLOAT(0.0f, v);
}

static void test_fade_in_ramps_monotonically_to_the_volume() {
    Gain g;
    g.setVolume(100);
    g.fadeIn(1000);
    const auto t = trace(g, 1500);
    TEST_ASSERT_TRUE(t[0] > 0.0f && t[0] < 0.01f);
    for (size_t i = 1; i < t.size(); ++i) TEST_ASSERT_TRUE(t[i] >= t[i - 1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, t[999]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, t[1499]);
    TEST_ASSERT_FALSE(g.silent());
}

static void test_fade_out_reaches_exact_silence() {
    Gain g;
    g.setVolume(70);
    g.fadeIn(0);
    g.fadeOut(500);
    const auto t = trace(g, 800);
    for (size_t i = 1; i < t.size(); ++i) TEST_ASSERT_TRUE(t[i] <= t[i - 1]);
    for (size_t i = 499; i < t.size(); ++i) TEST_ASSERT_EQUAL_FLOAT(0.0f, t[i]);
    TEST_ASSERT_TRUE(g.silent());
}

static void test_ceiling_caps_the_volume() {
    Gain g(60);
    g.setVolume(100);
    g.fadeIn(0);
    TEST_ASSERT_EQUAL_FLOAT(Gain::gainForPercent(60), g.currentGain());
}

static void test_ceiling_is_never_exceeded_mid_ramp() {
    Gain g(40);
    g.setVolume(30);
    g.fadeIn(200);
    trace(g, 100);
    g.setVolume(100);  // asks for far more than the ceiling, mid-fade
    const float cap = Gain::gainForPercent(40) + 1e-6f;
    for (int i = 0; i < 2000; ++i) {
        trace(g, 1);
        TEST_ASSERT_TRUE(g.currentGain() <= cap);
    }
}

static void test_lowering_the_ceiling_while_playing_ramps_down() {
    Gain g;
    g.setVolume(90);
    g.fadeIn(0);
    g.setCeiling(20);
    trace(g, Gain::kVolumeRampFrames + 10);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, Gain::gainForPercent(20), g.currentGain());
}

static void test_ceiling_cannot_exceed_100() {
    Gain g(250);
    TEST_ASSERT_EQUAL(100, g.ceiling());
}

static void test_volume_is_clamped_to_0_100() {
    Gain g;
    g.setVolume(150);
    TEST_ASSERT_EQUAL(100, g.volume());
    g.setVolume(-5);
    TEST_ASSERT_EQUAL(0, g.volume());
}

static void test_volume_changes_ramp_without_jumps() {
    Gain g;
    g.setVolume(10);
    g.fadeIn(0);
    g.setVolume(100);
    const auto t = trace(g, Gain::kVolumeRampFrames + 10);
    const float maxStep = 1.0f / Gain::kVolumeRampFrames + 1e-3f;
    for (size_t i = 1; i < t.size(); ++i) TEST_ASSERT_TRUE(std::fabs(t[i] - t[i - 1]) <= maxStep);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, t.back());
}

static void test_volume_zero_while_playing_is_exact_silence() {
    Gain g;
    g.setVolume(50);
    g.fadeIn(0);
    g.setVolume(0);
    const auto t = trace(g, Gain::kVolumeRampFrames + 10);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, t.back());
}

static void test_process_scales_rounds_and_clamps() {
    Gain g;
    g.setVolume(100);
    g.fadeIn(0);
    float in[6] = {0.5f, -0.5f, 1.0f, -1.0f, 1.5f, -1.5f};
    int16_t out[6];
    g.process(in, out, 3);
    TEST_ASSERT_EQUAL_INT16(16384, out[0]);
    TEST_ASSERT_EQUAL_INT16(-16384, out[1]);
    TEST_ASSERT_EQUAL_INT16(32767, out[2]);
    TEST_ASSERT_EQUAL_INT16(-32767, out[3]);
    TEST_ASSERT_EQUAL_INT16(32767, out[4]);
    TEST_ASSERT_EQUAL_INT16(-32767, out[5]);
}

static void test_both_channels_get_the_same_gain() {
    Gain g;
    g.setVolume(50);
    g.fadeIn(100);
    std::vector<float> x(400, 0.8f);
    std::vector<int16_t> y(400);
    g.process(x.data(), y.data(), 200);
    for (int i = 0; i < 200; ++i) TEST_ASSERT_EQUAL_INT16(y[2 * i], y[2 * i + 1]);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_gain_curve_endpoints_and_midpoint);
    RUN_TEST(test_gain_curve_is_strictly_increasing);
    RUN_TEST(test_starts_silent);
    RUN_TEST(test_fade_in_ramps_monotonically_to_the_volume);
    RUN_TEST(test_fade_out_reaches_exact_silence);
    RUN_TEST(test_ceiling_caps_the_volume);
    RUN_TEST(test_ceiling_is_never_exceeded_mid_ramp);
    RUN_TEST(test_lowering_the_ceiling_while_playing_ramps_down);
    RUN_TEST(test_ceiling_cannot_exceed_100);
    RUN_TEST(test_volume_is_clamped_to_0_100);
    RUN_TEST(test_volume_changes_ramp_without_jumps);
    RUN_TEST(test_volume_zero_while_playing_is_exact_silence);
    RUN_TEST(test_process_scales_rounds_and_clamps);
    RUN_TEST(test_both_channels_get_the_same_gain);
    return UNITY_END();
}
