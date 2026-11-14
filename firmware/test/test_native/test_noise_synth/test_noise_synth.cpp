// Native unit tests for NoiseSynth — the noise generator itself.
//
// The properties that matter for how it sounds:
//   - each colour has the right spectral slope (measured with an FFT here on
//     the host; the device never runs an FFT);
//   - every type plays at the same RMS level, so switching type does not
//     jump in loudness;
//   - no DC drift, and clipping is (statistically) never needed;
//   - left and right are independent (a wide stereo image, not mono);
//   - output is deterministic for a seed, so these tests are repeatable.

#include <unity.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "noise_synth.h"

namespace {

constexpr double kFs = 44100.0;
constexpr int kFftN = 4096;

void fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Averaged periodogram (Welch, Hann window, no overlap) of the left channel.
std::vector<double> psd(NoiseType type, int segments) {
    NoiseSynth synth(type, 12345);
    std::vector<float> buf(kFftN * 2);
    std::vector<double> win(kFftN), acc(kFftN / 2 + 1, 0.0);
    for (int i = 0; i < kFftN; ++i) win[i] = 0.5 - 0.5 * std::cos(2.0 * M_PI * i / kFftN);
    std::vector<std::complex<double>> x(kFftN);
    synth.render(buf.data(), kFftN);  // discard the start-up transient
    for (int s = 0; s < segments; ++s) {
        synth.render(buf.data(), kFftN);
        for (int i = 0; i < kFftN; ++i) x[i] = {buf[2 * i] * win[i], 0.0};
        fft(x);
        for (int k = 0; k <= kFftN / 2; ++k) acc[k] += std::norm(x[k]);
    }
    return acc;
}

double bandMean(const std::vector<double>& p, double f0, double f1) {
    const double df = kFs / kFftN;
    const int k0 = static_cast<int>(std::ceil(f0 / df));
    const int k1 = static_cast<int>(std::floor(f1 / df));
    double sum = 0.0;
    for (int k = k0; k < k1; ++k) sum += p[k];
    return sum / (k1 - k0);
}

// PSD slope in dB per octave, measured between bands [f,2f) and [2f,4f).
// For a power law PSD ~ f^-a the band means differ by exactly 2^-a.
double slopeDb(const std::vector<double>& p, double f) {
    return 10.0 * std::log10(bandMean(p, 2 * f, 4 * f) / bandMean(p, f, 2 * f));
}

struct Stats {
    double meanL = 0, rmsL = 0, rmsR = 0, corrLR = 0;
    float peak = 0;
};

Stats measure(NoiseType type, double seconds, uint32_t seed = 7) {
    NoiseSynth synth(type, seed);
    const size_t frames = static_cast<size_t>(seconds * kFs);
    std::vector<float> buf(4096 * 2);
    double sumL = 0, sumLL = 0, sumRR = 0, sumLR = 0;
    Stats st;
    size_t done = 0;
    while (done < frames) {
        const size_t n = std::min<size_t>(4096, frames - done);
        synth.render(buf.data(), n);
        for (size_t i = 0; i < n; ++i) {
            const double l = buf[2 * i], r = buf[2 * i + 1];
            sumL += l;
            sumLL += l * l;
            sumRR += r * r;
            sumLR += l * r;
            st.peak = std::max(st.peak, std::max(std::fabs(buf[2 * i]), std::fabs(buf[2 * i + 1])));
        }
        done += n;
    }
    st.meanL = sumL / frames;
    st.rmsL = std::sqrt(sumLL / frames);
    st.rmsR = std::sqrt(sumRR / frames);
    st.corrLR = sumLR / std::sqrt(sumLL * sumRR);
    return st;
}

const NoiseType kAllTypes[] = {NoiseType::White, NoiseType::Pink, NoiseType::Brown,
                               NoiseType::DeepBrown};

}  // namespace

void setUp() {}
void tearDown() {}

// --- determinism -----------------------------------------------------------

static void test_same_seed_gives_identical_output() {
    NoiseSynth a(NoiseType::Pink, 42), b(NoiseType::Pink, 42);
    std::vector<float> x(2048), y(2048);
    a.render(x.data(), 1024);
    b.render(y.data(), 1024);
    TEST_ASSERT_EQUAL_FLOAT_ARRAY(x.data(), y.data(), 2048);
}

static void test_different_seeds_give_different_output() {
    NoiseSynth a(NoiseType::White, 1), b(NoiseType::White, 2);
    std::vector<float> x(2048), y(2048);
    a.render(x.data(), 1024);
    b.render(y.data(), 1024);
    int same = 0;
    for (int i = 0; i < 2048; ++i) same += (x[i] == y[i]);
    TEST_ASSERT_LESS_THAN(10, same);
}

// --- level -----------------------------------------------------------------

static void test_every_type_plays_at_the_target_rms() {
    for (NoiseType t : kAllTypes) {
        const Stats st = measure(t, 20.0);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05 * kNoiseTargetRms, kNoiseTargetRms, st.rmsL,
                                         noiseTypeName(t));
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05 * kNoiseTargetRms, kNoiseTargetRms, st.rmsR,
                                         noiseTypeName(t));
    }
}

static void test_no_dc_offset() {
    for (NoiseType t : kAllTypes) {
        const Stats st = measure(t, 60.0);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01, 0.0, st.meanL, noiseTypeName(t));
    }
}

static void test_output_bounded_and_clipping_is_vanishingly_rare() {
    for (NoiseType t : kAllTypes) {
        NoiseSynth synth(t, 99);
        std::vector<float> buf(4096 * 2);
        const size_t frames = static_cast<size_t>(600 * kFs);  // 10 minutes
        for (size_t done = 0; done < frames; done += 4096) {
            synth.render(buf.data(), 4096);
            for (float v : buf) {
                if (v > 1.0f || v < -1.0f) TEST_FAIL_MESSAGE(noiseTypeName(t));
            }
        }
        const double clipRate = static_cast<double>(synth.clippedSamples()) / (2.0 * frames);
        TEST_ASSERT_TRUE_MESSAGE(clipRate < 1e-6, noiseTypeName(t));
    }
}

// --- stereo ----------------------------------------------------------------

static void test_left_and_right_are_independent() {
    for (NoiseType t : kAllTypes) {
        const Stats st = measure(t, 20.0);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05, 0.0, st.corrLR, noiseTypeName(t));
    }
}

// --- spectrum --------------------------------------------------------------

static void test_white_is_flat() {
    const auto p = psd(NoiseType::White, 200);
    for (double f : {250.0, 1000.0, 4000.0}) TEST_ASSERT_FLOAT_WITHIN(0.5, 0.0, slopeDb(p, f));
}

static void test_pink_falls_3db_per_octave() {
    const auto p = psd(NoiseType::Pink, 200);
    for (double f : {125.0, 500.0, 2000.0, 4000.0}) {
        TEST_ASSERT_FLOAT_WITHIN(0.5, -3.01, slopeDb(p, f));
    }
}

static void test_brown_falls_6db_per_octave() {
    const auto p = psd(NoiseType::Brown, 200);
    for (double f : {500.0, 1000.0, 2000.0}) TEST_ASSERT_FLOAT_WITHIN(0.7, -6.02, slopeDb(p, f));
}

static void test_deep_brown_falls_12db_per_octave_up_top() {
    const auto p = psd(NoiseType::DeepBrown, 200);
    TEST_ASSERT_FLOAT_WITHIN(1.5, -12.0, slopeDb(p, 2000.0));
}

static void test_deep_brown_is_darker_than_brown() {
    const auto brown = psd(NoiseType::Brown, 100);
    const auto deep = psd(NoiseType::DeepBrown, 100);
    // Same total power, so "darker" means less of it above 1 kHz.
    const double hiBrown = bandMean(brown, 1000, 8000) / bandMean(brown, 50, 400);
    const double hiDeep = bandMean(deep, 1000, 8000) / bandMean(deep, 50, 400);
    TEST_ASSERT_TRUE(10.0 * std::log10(hiBrown / hiDeep) > 6.0);
}

// --- type handling ---------------------------------------------------------

static void test_set_type_switches_the_generator() {
    NoiseSynth synth(NoiseType::White, 3);
    TEST_ASSERT_EQUAL(NoiseType::White, synth.type());
    synth.setType(NoiseType::Brown);
    TEST_ASSERT_EQUAL(NoiseType::Brown, synth.type());
}

static void test_next_type_cycles_through_all_types() {
    NoiseType t = NoiseType::White;
    for (int i = 0; i < static_cast<int>(NoiseType::Count); ++i) t = nextNoiseType(t);
    TEST_ASSERT_EQUAL(NoiseType::White, t);
    TEST_ASSERT_EQUAL(NoiseType::Pink, nextNoiseType(NoiseType::White));
    TEST_ASSERT_EQUAL(NoiseType::White, nextNoiseType(NoiseType::DeepBrown));
}

static void test_type_names() {
    TEST_ASSERT_EQUAL_STRING("white", noiseTypeName(NoiseType::White));
    TEST_ASSERT_EQUAL_STRING("pink", noiseTypeName(NoiseType::Pink));
    TEST_ASSERT_EQUAL_STRING("brown", noiseTypeName(NoiseType::Brown));
    TEST_ASSERT_EQUAL_STRING("deep brown", noiseTypeName(NoiseType::DeepBrown));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_same_seed_gives_identical_output);
    RUN_TEST(test_different_seeds_give_different_output);
    RUN_TEST(test_every_type_plays_at_the_target_rms);
    RUN_TEST(test_no_dc_offset);
    RUN_TEST(test_output_bounded_and_clipping_is_vanishingly_rare);
    RUN_TEST(test_left_and_right_are_independent);
    RUN_TEST(test_white_is_flat);
    RUN_TEST(test_pink_falls_3db_per_octave);
    RUN_TEST(test_brown_falls_6db_per_octave);
    RUN_TEST(test_deep_brown_falls_12db_per_octave_up_top);
    RUN_TEST(test_deep_brown_is_darker_than_brown);
    RUN_TEST(test_set_type_switches_the_generator);
    RUN_TEST(test_next_type_cycles_through_all_types);
    RUN_TEST(test_type_names);
    return UNITY_END();
}
