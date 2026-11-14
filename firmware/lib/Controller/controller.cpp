#include "controller.h"

Controller::Controller(bool inWindow, uint32_t) : inWindow_(inWindow) {}

void Controller::setInWindow(bool in, uint32_t) {
    if (in == inWindow_) return;
    inWindow_ = in;
    // Window start: a fresh night (clears any earlier stop) and the window
    // takes over from a daytime run. Window end: everything resets, so the
    // day starts silent.
    nightStopped_ = false;
    runActive_ = false;
}

void Controller::pressStartStop(uint32_t nowMs) {
    if (inWindow_) {
        nightStopped_ = !nightStopped_;  // stop until next start / resume to window end
        return;
    }
    if (runActive_) {
        runActive_ = false;
    } else {
        runActive_ = true;
        runStartMs_ = nowMs;
    }
}

void Controller::tick(uint32_t nowMs) {
    if (runActive_ && !inWindow_ && (uint32_t)(nowMs - runStartMs_) >= kRunMs) runActive_ = false;
}

bool Controller::shouldPlay() const { return inWindow_ ? !nightStopped_ : runActive_; }

const char* Controller::stateName() const {
    if (inWindow_) return nightStopped_ ? "stopped until next window" : "night window";
    return runActive_ ? "daytime run (2 h)" : "idle (outside window)";
}

uint32_t Controller::runMinutesLeft(uint32_t nowMs) const {
    if (inWindow_ || !runActive_) return 0;
    const uint32_t elapsed = nowMs - runStartMs_;
    if (elapsed >= kRunMs) return 0;
    return (kRunMs - elapsed + 59999UL) / 60000UL;
}
