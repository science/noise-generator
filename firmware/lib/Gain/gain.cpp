#include "gain.h"

#include <math.h>

namespace {

constexpr float kRangeDb = 50.0f;  // volume 1% = -49.5 dB, 100% = 0 dB

int clampPercent(int p) { return p < 0 ? 0 : (p > 100 ? 100 : p); }

int16_t toInt16(float v) {
    float s = v * 32767.0f;
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32767.0f) s = -32767.0f;
    return static_cast<int16_t>(lrintf(s));
}

}  // namespace

Gain::Gain(int ceilingPercent) : ceiling_(clampPercent(ceilingPercent)) {}

float Gain::gainForPercent(int percent) {
    const int p = clampPercent(percent);
    if (p == 0) return 0.0f;
    if (p == 100) return 1.0f;
    return powf(10.0f, (kRangeDb * (p / 100.0f) - kRangeDb) / 20.0f);
}

float Gain::targetGain() const {
    if (!playing_) return 0.0f;
    return gainForPercent(volume_ < ceiling_ ? volume_ : ceiling_);
}

void Gain::rampTo(float target, uint32_t frames) {
    target_ = target;
    if (frames == 0 || current_ == target) {
        current_ = target;
        step_ = 0.0f;
        remaining_ = 0;
        return;
    }
    step_ = (target - current_) / static_cast<float>(frames);
    remaining_ = frames;
}

void Gain::setVolume(int percent) {
    volume_ = clampPercent(percent);
    if (playing_) rampTo(targetGain(), kVolumeRampFrames);
}

void Gain::setCeiling(int percent) {
    ceiling_ = clampPercent(percent);
    if (playing_) rampTo(targetGain(), kVolumeRampFrames);
}

void Gain::fadeIn(uint32_t frames) {
    playing_ = true;
    rampTo(targetGain(), frames);
}

void Gain::fadeOut(uint32_t frames) {
    playing_ = false;
    rampTo(0.0f, frames);
}

bool Gain::silent() const { return current_ == 0.0f && target_ == 0.0f; }

void Gain::process(const float* in, int16_t* out, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        if (remaining_ > 0) {
            current_ += step_;
            if (--remaining_ == 0) current_ = target_;  // land exactly, no drift
        }
        out[2 * i] = toInt16(in[2 * i] * current_);
        out[2 * i + 1] = toInt16(in[2 * i + 1] * current_);
    }
}
