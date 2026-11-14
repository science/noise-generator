// BtDiag — turns Bluetooth status codes into words (pure C++).
//
// ESP-IDF reports link failures as esp_bt_status_t. Values 0..0xFF are the
// stack's own codes; 0x100 + n is HCI error n from the Bluetooth spec
// (Core spec, Vol 1 Part F). The firmware logs these so that a failed
// connect says WHY: no answer at all, or an answer followed by a refusal.
#pragma once

#include <stdint.h>

enum class BtFailure {
    None,       // success
    NoAnswer,   // the speaker never answered the call (off, out of range,
                // busy, or not accepting connections right now)
    Refused,    // it answered, then refused pairing or the connection
    StaleKey,   // pairing keys disagree: one side forgot the other
    Other,
};

// Short name, e.g. "page timeout", "pairing not allowed"; nullptr if unknown.
const char* btStatusName(int status);
BtFailure classifyBtStatus(int status);
const char* btFailureName(BtFailure f);

// Legacy (pre-2.1) pairing asks for a PIN. Edifier documents 0000; many
// other devices use 1234. Attempts alternate between them.
const char* legacyPinForAttempt(int attempt);
