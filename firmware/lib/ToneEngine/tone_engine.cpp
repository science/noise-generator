#include "tone_engine.h"

#include <math.h>

namespace {

constexpr double kFs = 44100.0;
constexpr double kTwoPi = 6.283185307179586;
constexpr double kWhiteVar = 1.0 / 3.0;  // uniform white in [-1, 1)

double poleFor(double hz) { return exp(-kTwoPi * hz / kFs); }

// --- grey: inverse A-weighting --------------------------------------------
// A-weighting's poles (20.6 x2, 107.7, 737.9, 12194 x2 Hz) become zeros; its
// four zeros at DC become poles at 25 Hz, which caps the bass boost (pure
// inverse-A would boost without limit toward DC). Matched-z first-order
// sections: y = x - z*x[n-1] + p*y[n-1]. Prototyped against the analog
// curve: within 0.2 dB from 31 Hz to 4 kHz, 0.8 dB at 8 kHz.
constexpr int kGreySections = 6;
constexpr double kGreyZeroHz[kGreySections] = {20.6, 20.6, 107.7, 737.9, 12194.0, 12194.0};
constexpr double kGreyPoleHz = 25.0;
constexpr int kGreyPoles = 4;  // sections 0..3 carry a pole, 4..5 are zero-only

// --- brown family ------------------------------------------------------------
constexpr double kBrownHz = 140.0;

// Ladder v2: second low-pass corner per step (see tone_engine.h). Spacing
// checked analytically: centroid 225, 193, 162, 136, 117, 101, 88, 77, 67 Hz.
constexpr double kLadderLp2Hz[kToneSteps] = {1000, 640, 400, 260, 180, 125, 90, 65, 45};

// Grey/brown blend shares that were evenly spaced on ladder v1 (kept for a
// future grey family): 1.0, 0.9295, 0.7441, 0.

struct Coeffs {
    float gz[kGreySections], gp;  // grey zeros / pole
    float greyGain;               // normalises grey to kToneTargetRms
    double a1;                    // brown integrator pole
    float a1f;
};

double onePoleVar(double a) { return 1.0 / (1.0 - a * a); }

// y1 = a1 y1 + w ; y2 = a2 y2 + (1-a2) y1  ->  sum of squared impulse response.
double twoPoleVar(double a1, double a2) {
    const double k = (1.0 - a2) / (a1 - a2);
    return k * k *
           (a1 * a1 / (1.0 - a1 * a1) - 2.0 * a1 * a2 / (1.0 - a1 * a2) +
            a2 * a2 / (1.0 - a2 * a2));
}

// Grey's gain has no tidy closed form, so sum its squared impulse response
// (in double, once). The slowest pole (25 Hz) decays by e every ~280
// samples; by 20k samples the tail is below 1e-25 (cheap on the ESP32, whose
// doubles are software-emulated).
double greyVar(const double* z, double p) {
    double xPrev[kGreySections] = {0}, yPrev[kGreySections] = {0}, energy = 0;
    for (int n = 0; n < 20000; ++n) {
        double x = (n == 0) ? 1.0 : 0.0;
        for (int s = 0; s < kGreySections; ++s) {
            const double y = x - z[s] * xPrev[s] + (s < kGreyPoles ? p * yPrev[s] : 0.0);
            xPrev[s] = x;
            yPrev[s] = y;
            x = y;
        }
        energy += x * x;
    }
    return energy;
}

const Coeffs& coeffs() {
    static Coeffs c;
    static bool ready = false;
    if (ready) return c;
    double z[kGreySections];
    for (int s = 0; s < kGreySections; ++s) {
        z[s] = poleFor(kGreyZeroHz[s]);
        c.gz[s] = static_cast<float>(z[s]);
    }
    const double p = poleFor(kGreyPoleHz);
    c.gp = static_cast<float>(p);
    c.greyGain = static_cast<float>(kToneTargetRms / sqrt(kWhiteVar * greyVar(z, p)));
    c.a1 = poleFor(kBrownHz);
    c.a1f = static_cast<float>(c.a1);
    ready = true;
    return c;
}

uint64_t splitmix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

inline float white(uint64_t& s) {
    uint64_t x = s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s = x;
    return static_cast<float>(static_cast<int32_t>(x >> 32)) * (1.0f / 2147483648.0f);
}

constexpr int kPrerollFrames = 4096;  // settle a new voice's filters before it is heard

}  // namespace

const ToneRung kGreyRung{1.0f, 0.0f};
const ToneRung kBrownRung{0.0f, 0.0f};

const char* toneName(int step) {
    static const char* const names[kToneSteps] = {
        "lightest", "lighter", "light", "slightly light", "middle",
        "slightly dark", "dark", "darker", "darkest"};
    if (step < 0 || step >= kToneSteps) return "?";
    return names[step];
}

void crossfadeGains(uint32_t pos, uint32_t len, float& outGain, float& inGain) {
    if (len == 0 || pos >= len) {
        outGain = 0.0f;
        inGain = 1.0f;
        return;
    }
    const float theta = 1.5707963267948966f * static_cast<float>(pos) / static_cast<float>(len);
    outGain = cosf(theta);
    inGain = sinf(theta);
}

static int clampStep(int s) { return s < 0 ? 0 : (s >= kToneSteps ? kToneSteps - 1 : s); }

static ToneRung ladderRung(int step) { return ToneRung{0.0f, static_cast<float>(kLadderLp2Hz[step])}; }

ToneEngine::ToneEngine(int step, uint32_t seed)
    : requested_(clampStep(step)), seedCounter_(static_cast<uint64_t>(seed) << 20) {
    startVoice(voices_[cur_], requested_, ladderRung(requested_));
}

ToneEngine::ToneEngine(const ToneRung& custom, uint32_t seed)
    : requested_(-1), seedCounter_(static_cast<uint64_t>(seed) << 20) {
    startVoice(voices_[cur_], -1, custom);
}

void ToneEngine::startVoice(Voice& v, int step, const ToneRung& rung) {
    const Coeffs& k = coeffs();
    v.step = step;
    // Normalise each part to the target RMS; independent grey and brown
    // sources add in power, so the weights are square roots of the shares.
    double brownVar = onePoleVar(k.a1);
    v.mix.a2 = 0.0f;
    if (rung.lp2Hz > 0) {
        const double a2 = poleFor(rung.lp2Hz);
        v.mix.a2 = static_cast<float>(a2);
        brownVar = twoPoleVar(k.a1, a2);
    }
    const double share = rung.greyShare < 0 ? 0 : (rung.greyShare > 1 ? 1 : rung.greyShare);
    v.mix.grey = static_cast<float>(sqrt(share)) * k.greyGain;
    v.mix.brown = static_cast<float>(sqrt(1.0 - share) * kToneTargetRms / sqrt(kWhiteVar * brownVar));
    for (Channel& c : v.ch) {
        c = Channel();
        c.rngGrey = splitmix64(++seedCounter_) | 1;
        c.rngBrown = splitmix64(++seedCounter_) | 1;
    }
    for (int i = 0; i < kPrerollFrames; ++i) {
        next(v, v.ch[0]);
        next(v, v.ch[1]);
    }
}

float ToneEngine::next(Voice& v, Channel& c) {
    const Coeffs& k = coeffs();
    float out = 0.0f;
    if (v.mix.grey > 0.0f) {
        float x = white(c.rngGrey);
        for (int i = 0; i < kGreySections; ++i) {
            const float y = x - k.gz[i] * c.gx[i] + (i < kGreyPoles ? k.gp * c.gy[i] : 0.0f);
            c.gx[i] = x;
            c.gy[i] = y;
            x = y;
        }
        out += v.mix.grey * x;
    }
    if (v.mix.brown > 0.0f) {
        c.s0 = k.a1f * c.s0 + white(c.rngBrown);
        float b = c.s0;
        if (v.mix.a2 > 0.0f) {
            c.s1 = v.mix.a2 * c.s1 + (1.0f - v.mix.a2) * c.s0;
            b = c.s1;
        }
        out += v.mix.brown * b;
    }
    return out;
}

void ToneEngine::beginFade(int step) {
    Voice& incoming = voices_[1 - cur_];
    startVoice(incoming, step, ladderRung(step));
    fading_ = true;
    fadePos_ = 0;
}

void ToneEngine::setStep(int step) {
    requested_ = clampStep(step);
    // During a crossfade the request just waits; render() starts the next
    // fade once the current one finishes (no abrupt jumps).
    if (!fading_ && requested_ != voices_[cur_].step) beginFade(requested_);
}

void ToneEngine::render(float* out, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        float g0 = 1.0f, g1 = 0.0f;
        if (fading_) crossfadeGains(fadePos_, kToneCrossfadeFrames, g0, g1);
        for (int c = 0; c < 2; ++c) {
            Voice& a = voices_[cur_];
            float v = next(a, a.ch[c]);
            if (fading_) {
                Voice& b = voices_[1 - cur_];
                v = g0 * v + g1 * next(b, b.ch[c]);
            }
            if (v > 1.0f) {
                v = 1.0f;
                ++clipped_;
            } else if (v < -1.0f) {
                v = -1.0f;
                ++clipped_;
            }
            out[2 * i + c] = v;
        }
        if (fading_ && ++fadePos_ >= kToneCrossfadeFrames) {
            cur_ = 1 - cur_;
            fading_ = false;
            if (requested_ != voices_[cur_].step) beginFade(requested_);
        }
    }
}
