// ToneEngine — the 9-step grey -> brown tone ladder (pure C++, host-testable).
//
// Steps (plan "Sound engine", user Q1):
//   0 grey                 inverse A-weighting (sounds equally loud at every
//                          pitch); bass boost capped below ~25 Hz
//   1, 2 grey/brown blends shares chosen so steps 0-3 are evenly spaced in
//                          brightness (see kGreyShare)
//   3 brown                6 dB/octave above a 140 Hz corner
//   4..8 deeper browns     brown + a second low-pass at 2 k, 1 k, 500, 300,
//                          180 Hz. Step 6 (500 Hz) is "deep brown", the default.
// Every step plays at the same RMS. A step change is an equal-power
// crossfade over 200 ms between the old and new sound, so there is no click
// and no level dip. Requests made during a crossfade queue up and run next.
// Output: interleaved stereo float in [-1, 1], independent L/R.
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr int kToneSteps = 9;
constexpr int kDefaultTone = 6;                  // "deep brown", the user's pick
constexpr float kToneTargetRms = 0.18f;          // same level as NoiseSynth
constexpr uint32_t kToneCrossfadeFrames = 8820;  // 200 ms at 44.1 kHz

const char* toneName(int step);

// Equal-power crossfade gains at position pos of len: out^2 + in^2 == 1.
void crossfadeGains(uint32_t pos, uint32_t len, float& outGain, float& inGain);

class ToneEngine {
public:
    explicit ToneEngine(int step = kDefaultTone, uint32_t seed = 1);

    void setStep(int step);           // clamped to 0..kToneSteps-1
    int step() const { return requested_; }
    bool crossfading() const { return fading_; }

    void render(float* interleaved, size_t frames);
    uint64_t clippedSamples() const { return clipped_; }

private:
    struct Channel {
        uint64_t rngGrey = 0, rngBrown = 0;
        float gx[6] = {0, 0, 0, 0, 0, 0};  // grey: previous input of each section
        float gy[6] = {0, 0, 0, 0, 0, 0};  // grey: previous output of each section
        float s0 = 0, s1 = 0;              // brown: integrator and second low-pass
    };
    struct Voice {
        int step = kDefaultTone;
        Channel ch[2];
    };

    void startVoice(Voice& v, int step);
    float next(Voice& v, Channel& c);
    void beginFade(int step);

    Voice voices_[2];
    int cur_ = 0;
    int requested_;
    bool fading_ = false;
    uint32_t fadePos_ = 0;
    uint64_t seedCounter_;
    uint64_t clipped_ = 0;
};
