#include "noise_synth.h"

#include <math.h>

namespace {

constexpr double kSampleRate = 44100.0;
constexpr double kTwoPi = 6.283185307179586;

// Brown: white noise through a one-pole low-pass ("leaky integrator").
// Above the corner the spectrum falls 6 dB/octave; below it, it flattens
// out instead of running off to DC. 140 Hz matches the widely used
// b = (b + 0.02 w) / 1.02 recipe.
constexpr double kBrownCornerHz = 140.0;
// Deep brown: brown plus a second low-pass at 500 Hz, so the top end falls
// ~12 dB/octave. Darker, with less hiss.
constexpr double kDeepCornerHz = 500.0;

// Paul Kellet's "refined" pink filter (musicdsp.org): six one-pole sections
// plus a direct and a one-sample-delayed white term. Accurate to ±0.05 dB
// from 9.2 Hz up at 44.1 kHz.
constexpr int kPinkPoles = 6;
constexpr double kPinkP[kPinkPoles] = {0.99886, 0.99332, 0.96900, 0.86650, 0.55000, -0.7616};
constexpr double kPinkG[kPinkPoles] = {0.0555179, 0.0750759, 0.1538520,
                                       0.3104856, 0.5329522, -0.0168980};
constexpr double kPinkDirect = 0.5362;
constexpr double kPinkDelayed = 0.115926;

// Variance of uniform white noise in [-1, 1).
constexpr double kWhiteVar = 1.0 / 3.0;

double poleFor(double cornerHz) { return exp(-kTwoPi * cornerHz / kSampleRate); }

// Output variance of each filter for unit-variance white input, derived in
// closed form so the normalising gain is exact rather than measured.

// y = a*y + w  =>  var = 1 / (1 - a^2)
double onePoleVar(double a) { return 1.0 / (1.0 - a * a); }

// y1 = a1*y1 + w ; y2 = a2*y2 + (1 - a2)*y1. The impulse response is
// h[n] = (1-a2) (a1^(n+1) - a2^(n+1)) / (a1 - a2); this is sum(h^2).
double twoPoleVar(double a1, double a2) {
    const double k = (1.0 - a2) / (a1 - a2);
    return k * k *
           (a1 * a1 / (1.0 - a1 * a1) - 2.0 * a1 * a2 / (1.0 - a1 * a2) +
            a2 * a2 / (1.0 - a2 * a2));
}

// For b_i = p_i b_i + g_i w all driven by the same w:
// E[b_i b_j] = g_i g_j / (1 - p_i p_j), E[b_i[n] w[n]] = g_i,
// E[b_i[n] w[n-1]] = p_i g_i.
double pinkVar() {
    double v = kPinkDirect * kPinkDirect + kPinkDelayed * kPinkDelayed;
    for (int i = 0; i < kPinkPoles; ++i) {
        for (int j = 0; j < kPinkPoles; ++j) {
            v += kPinkG[i] * kPinkG[j] / (1.0 - kPinkP[i] * kPinkP[j]);
        }
        v += 2.0 * kPinkDirect * kPinkG[i] + 2.0 * kPinkDelayed * kPinkP[i] * kPinkG[i];
    }
    return v;
}

const double kBrownA = poleFor(kBrownCornerHz);
const double kDeepA = poleFor(kDeepCornerHz);

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

}  // namespace

const char* noiseTypeName(NoiseType t) {
    switch (t) {
        case NoiseType::White: return "white";
        case NoiseType::Pink: return "pink";
        case NoiseType::Brown: return "brown";
        case NoiseType::DeepBrown: return "deep brown";
        default: return "?";
    }
}

NoiseType nextNoiseType(NoiseType t) {
    const int n = static_cast<int>(NoiseType::Count);
    return static_cast<NoiseType>((static_cast<int>(t) + 1) % n);
}

NoiseSynth::NoiseSynth(NoiseType type, uint32_t seed) : type_(type) {
    // Two independent xorshift64 streams (period 2^64 - 1: centuries at
    // 44.1 kHz, so the noise never audibly repeats). splitmix64 spreads a
    // small seed into a well-mixed, non-zero state.
    for (int c = 0; c < 2; ++c) {
        uint64_t s = splitmix64(static_cast<uint64_t>(seed) * 2 + c + 1);
        ch_[c].rng = s ? s : 0x2545F4914F6CDD1DULL;
    }
    setType(type);
}

void NoiseSynth::setType(NoiseType type) {
    type_ = type;
    resetState();
    double var = kWhiteVar;
    switch (type) {
        case NoiseType::White: break;
        case NoiseType::Pink: var *= pinkVar(); break;
        case NoiseType::Brown: var *= onePoleVar(kBrownA); break;
        case NoiseType::DeepBrown: var *= twoPoleVar(kBrownA, kDeepA); break;
        default: break;
    }
    gain_ = static_cast<float>(kNoiseTargetRms / sqrt(var));
}

void NoiseSynth::resetState() {
    for (Channel& c : ch_) {
        for (float& s : c.s) s = 0.0f;
    }
}

// Uniform white noise in [-1, 1).
float NoiseSynth::white(Channel& c) {
    uint64_t x = c.rng;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    c.rng = x;
    return static_cast<float>(static_cast<int32_t>(x >> 32)) * (1.0f / 2147483648.0f);
}

float NoiseSynth::next(Channel& c) {
    const float w = white(c);
    float* s = c.s;
    switch (type_) {
        case NoiseType::White:
            return w;
        case NoiseType::Pink: {
            float acc = 0.0f;
            for (int i = 0; i < kPinkPoles; ++i) {
                s[i] = static_cast<float>(kPinkP[i]) * s[i] + static_cast<float>(kPinkG[i]) * w;
                acc += s[i];
            }
            acc += static_cast<float>(kPinkDirect) * w + s[6];  // s[6] = delayed term
            s[6] = static_cast<float>(kPinkDelayed) * w;
            return acc;
        }
        case NoiseType::Brown:
            s[0] = static_cast<float>(kBrownA) * s[0] + w;
            return s[0];
        case NoiseType::DeepBrown:
            s[0] = static_cast<float>(kBrownA) * s[0] + w;
            s[1] = static_cast<float>(kDeepA) * s[1] + static_cast<float>(1.0 - kDeepA) * s[0];
            return s[1];
        default:
            return 0.0f;
    }
}

void NoiseSynth::render(float* out, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            float v = next(ch_[c]) * gain_;
            if (v > 1.0f) {
                v = 1.0f;
                ++clipped_;
            } else if (v < -1.0f) {
                v = -1.0f;
                ++clipped_;
            }
            out[2 * i + c] = v;
        }
    }
}
