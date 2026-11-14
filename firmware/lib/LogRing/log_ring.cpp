#include "log_ring.h"

#include <string.h>

void logRingClear(LogRingStore& s) {
    s.magic = kLogRingMagic;
    s.head = 0;
    s.used = 0;
}

bool logRingAttach(LogRingStore& s) {
    if (s.magic == kLogRingMagic && s.head < kLogRingBytes && s.used <= kLogRingBytes) return true;
    logRingClear(s);
    return false;
}

void logRingAppend(LogRingStore& s, const char* text, size_t n) {
    if (n > kLogRingBytes) {  // only the tail can fit
        text += n - kLogRingBytes;
        n = kLogRingBytes;
    }
    const size_t first = n < kLogRingBytes - s.head ? n : kLogRingBytes - s.head;
    memcpy(s.data + s.head, text, first);
    memcpy(s.data, text + first, n - first);
    s.head = static_cast<uint32_t>((s.head + n) % kLogRingBytes);
    s.used = static_cast<uint32_t>(s.used + n > kLogRingBytes ? kLogRingBytes : s.used + n);
}

void logRingSpans(const LogRingStore& s, const char*& a, size_t& alen, const char*& b,
                  size_t& blen) {
    if (s.used < kLogRingBytes) {  // not wrapped yet: one piece ending at head
        a = s.data + s.head - s.used;
        alen = s.used;
        b = s.data;
        blen = 0;
        return;
    }
    a = s.data + s.head;  // full: oldest byte is at head
    alen = kLogRingBytes - s.head;
    b = s.data;
    blen = s.head;
}
