// schedule — autoplay window maths on local wall-clock minutes (pure C++).
#pragma once

struct Window {
    int startMin;  // minutes since local midnight, inclusive
    int endMin;    // exclusive; may be earlier than start (crosses midnight)
};

// US/Pacific with automatic daylight saving, for setenv("TZ")/configTzTime.
extern const char* const kPacificTz;

bool inWindow(const Window& w, int minuteOfDay);
