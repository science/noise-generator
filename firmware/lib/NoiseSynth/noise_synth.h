// NoiseSynth — white / pink / brown / deep-brown noise generator.
//
// Pure C++ (no Arduino), so it runs in the native unit tests. Produces
// interleaved stereo float samples in [-1, 1] at a fixed 44.1 kHz (the A2DP
// rate). Left and right come from independent generators, for a wide stereo
// image. Every type is normalised to the same RMS level (kNoiseTargetRms),
// so switching type does not jump in loudness. Volume is applied later, by
// Gain.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class NoiseType : uint8_t { White, Pink, Brown, DeepBrown, Count };

// RMS of every type's output: 0.18 ≈ -15 dBFS. That leaves ~5.5 standard
// deviations of headroom before ±1.0, so clipping is statistically
// negligible (the tests check < 1 in a million samples).
constexpr float kNoiseTargetRms = 0.18f;

const char* noiseTypeName(NoiseType t);
NoiseType nextNoiseType(NoiseType t);

class NoiseSynth {
public:
    explicit NoiseSynth(NoiseType type = NoiseType::Brown, uint32_t seed = 1);

    void setType(NoiseType type);
    NoiseType type() const { return type_; }

    // Fills `frames` stereo frames (2 * frames floats, L R L R ...).
    void render(float* interleaved, size_t frames);

    // Samples that had to be clamped to ±1.0 since construction.
    uint64_t clippedSamples() const { return clipped_; }

private:
    struct Channel {
        uint64_t rng = 0;
        float s[7] = {0, 0, 0, 0, 0, 0, 0};  // filter state, meaning depends on type
    };

    float next(Channel& c);
    float white(Channel& c);
    void resetState();

    NoiseType type_;
    Channel ch_[2];
    float gain_ = 1.0f;  // normalises the current type to kNoiseTargetRms
    uint64_t clipped_ = 0;
};
