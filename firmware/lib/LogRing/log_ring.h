// LogRing — a small text log that survives a software restart (pure C++).
//
// The firmware places one LogRingStore in RTC memory that the boot code does
// not clear (RTC_NOINIT_ATTR). It survives ESP.restart(), a watchdog reset
// and a crash, but not power loss or the RST button. That is enough for the
// remote-debug loop: the board logs Bluetooth events overnight with Wi-Fi
// off; holding KEY4 restarts it into the update window, where the log is
// served over Wi-Fi.
//
// After power-on the memory holds garbage, so attach() checks a magic number
// and the bookkeeping fields, and starts empty if anything looks wrong.
#pragma once

#include <stddef.h>
#include <stdint.h>

constexpr size_t kLogRingBytes = 4096;
constexpr uint32_t kLogRingMagic = 0x4c4f4731;  // "LOG1"

struct LogRingStore {
    uint32_t magic;
    uint32_t head;  // next write position
    uint32_t used;  // bytes held, <= kLogRingBytes
    char data[kLogRingBytes];
};

// Returns true if the store already held a valid log (kept), false if it was
// reset to empty.
bool logRingAttach(LogRingStore& s);
void logRingClear(LogRingStore& s);
// Appends n bytes; when full, the oldest bytes are overwritten.
void logRingAppend(LogRingStore& s, const char* text, size_t n);
// The log, oldest first, as up to two pieces: a[0..alen) then b[0..blen).
void logRingSpans(const LogRingStore& s, const char*& a, size_t& alen, const char*& b,
                  size_t& blen);
