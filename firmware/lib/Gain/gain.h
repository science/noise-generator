// Gain — volume, fade in/out and a volume ceiling, applied to interleaved
// stereo float samples and converted to int16 for the output (A2DP or I2S).
//
// Pure C++ (no Arduino) so it runs in the native unit tests. Bedroom safety
// (docs/decisions.md D8): output starts silent and only rises through a
// fade, the ceiling is never exceeded, and every level change ramps.
#pragma once

#include <stddef.h>
#include <stdint.h>

class Gain {
public:
    // Volume changes ramp over this many frames: 50 ms at 44.1 kHz. Short
    // enough to feel instant on a key press, long enough to avoid a click.
    static constexpr uint32_t kVolumeRampFrames = 2205;

    explicit Gain(int ceilingPercent = 100);

    // 0..100 (clamped). 0 is silence; 1..100 maps to -50..0 dB.
    static float gainForPercent(int percent);

    void setVolume(int percent);
    int volume() const { return volume_; }

    // The volume can never be louder than this (0..100, clamped).
    void setCeiling(int percent);
    int ceiling() const { return ceiling_; }

    // Start playing: ramp from the current level up to the volume.
    void fadeIn(uint32_t frames);
    // Stop playing: ramp down to exact silence.
    void fadeOut(uint32_t frames);

    // True when the output is exactly zero and will stay there.
    bool silent() const;
    float currentGain() const { return current_; }

    void process(const float* in, int16_t* out, size_t frames);

private:
    float targetGain() const;
    void rampTo(float target, uint32_t frames);

    int volume_ = 0;
    int ceiling_ = 100;
    bool playing_ = false;
    float current_ = 0.0f;
    float target_ = 0.0f;
    float step_ = 0.0f;
    uint32_t remaining_ = 0;
};
