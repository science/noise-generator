// ToneEngine — the 9-step tone ladder (pure C++, host-testable).
//
// Ladder v2 (user desk test, 2026-11-13). Every rung is brown noise (6 dB
// per octave above a 140 Hz corner) plus a second low-pass. The rung the
// user liked best (180 Hz) is the MIDDLE step, so there is room both ways:
//   step:    0     1    2    3   [4]   5    6    7    8
//   corner: 1000  640  400  260  180  125   90   65   45 Hz
// Lower corner = darker (centroid falls ~12-16% per step).
//
// Grey (inverse A-weighting, bass capped below ~25 Hz) is not on the ladder
// any more but remains available as a custom rung (kGreyRung), as groundwork
// for a "tone family" key (grey / brown / deep brown) suggested by the user.
//
// Every rung plays at the same RMS. A step change is an equal-power
// crossfade over 200 ms between the old and new sound, so there is no click
// and no level dip. Requests made during a crossfade queue up and run next.
// Output: interleaved stereo float in [-1, 1], independent L/R.
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr int kToneSteps = 9;
constexpr int kDefaultTone = 4;                  // the user's favourite, mid-ladder
constexpr float kToneTargetRms = 0.18f;          // same level as NoiseSynth
constexpr uint32_t kToneCrossfadeFrames = 8820;  // 200 ms at 44.1 kHz

// A sound recipe: the share of power from grey (rest is brown) and the
// corner of the second low-pass on the brown part (0 = none).
struct ToneRung {
    float greyShare;
    float lp2Hz;
};
extern const ToneRung kGreyRung;   // pure grey
extern const ToneRung kBrownRung;  // plain brown, no second low-pass

const char* toneName(int step);

// Equal-power crossfade gains at position pos of len: out^2 + in^2 == 1.
void crossfadeGains(uint32_t pos, uint32_t len, float& outGain, float& inGain);

class ToneEngine {
public:
    explicit ToneEngine(int step = kDefaultTone, uint32_t seed = 1);
    // Plays a fixed custom rung; setStep() moves back onto the ladder.
    ToneEngine(const ToneRung& custom, uint32_t seed);

    void setStep(int step);           // clamped to 0..kToneSteps-1
    int step() const { return requested_; }
    bool crossfading() const { return fading_; }

    void render(float* interleaved, size_t frames);
    uint64_t clippedSamples() const { return clipped_; }

private:
    struct Mix {
        float grey = 0, brown = 1;  // amplitude weights (already include gains)
        float a2 = 0;               // second low-pass pole (0 = none)
    };
    struct Channel {
        uint64_t rngGrey = 0, rngBrown = 0;
        float gx[6] = {0, 0, 0, 0, 0, 0};  // grey: previous input of each section
        float gy[6] = {0, 0, 0, 0, 0, 0};  // grey: previous output of each section
        float s0 = 0, s1 = 0;              // brown: integrator and second low-pass
    };
    struct Voice {
        int step = -1;  // -1 = custom rung
        Mix mix;
        Channel ch[2];
    };

    void startVoice(Voice& v, int step, const ToneRung& rung);
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
