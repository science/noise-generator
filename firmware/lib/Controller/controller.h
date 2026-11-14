// Controller — the play/stop state machine (pure C++, host-testable).
//
// Rules (user, docs/plans/implementation-plan.md Q3 + F5), window 21:00-09:00:
//   inside the window : plays; KEY1 stops it until the next window start;
//                       KEY1 again resumes it until the window ends.
//   outside the window: KEY1 starts a 2-hour run; KEY1 again stops it.
//   a daytime run that reaches the window start hands over to the window.
// If the clock is unknown at boot, construct with inWindow = true ("assume
// night", fail toward noise) and call setInWindow() once the clock syncs.
//
// Times are millis()-style uint32_t; comparisons are wraparound-safe.
#pragma once

#include <stdint.h>

class Controller {
public:
    static constexpr uint32_t kRunMs = 2UL * 60UL * 60UL * 1000UL;  // 2 h daytime run
    static constexpr bool kAssumeNightUntilClockKnown = true;

    Controller(bool inWindow, uint32_t nowMs);

    // Feed the current window state; edges apply the start/end rules.
    void setInWindow(bool inWindow, uint32_t nowMs);
    void pressStartStop(uint32_t nowMs);
    void tick(uint32_t nowMs);  // ends a daytime run after 2 h

    bool shouldPlay() const;
    bool inWindow() const { return inWindow_; }
    const char* stateName() const;
    uint32_t runMinutesLeft(uint32_t nowMs) const;  // 0 unless a daytime run

private:
    bool inWindow_;
    bool nightStopped_ = false;  // KEY1 stop inside the window
    bool runActive_ = false;     // daytime 2-hour run
    uint32_t runStartMs_ = 0;
};
