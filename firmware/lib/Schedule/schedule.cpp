#include "schedule.h"

const char* const kPacificTz = "PST8PDT,M3.2.0,M11.1.0";

bool inWindow(const Window& w, int minuteOfDay) {
    constexpr int kDay = 24 * 60;
    const int m = ((minuteOfDay % kDay) + kDay) % kDay;
    if (w.startMin == w.endMin) return false;                       // empty window
    if (w.startMin < w.endMin) return m >= w.startMin && m < w.endMin;  // same day
    return m >= w.startMin || m < w.endMin;                         // crosses midnight
}
