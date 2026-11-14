#include "bt_diag.h"

namespace {
constexpr int kHciBase = 0x100;
}

const char* btStatusName(int status) {
    switch (status) {
        case 0: return "ok";
        case 1: return "failed";
        case 4: return "busy";
        case 9: return "authentication failure";
        case 10: return "remote device down";
        case 11: return "authentication rejected";
        case 16: return "timeout";
    }
    if (status < kHciBase || status > kHciBase + 0xFF) return nullptr;
    switch (status - kHciBase) {
        case 0x00: return "ok";
        case 0x02: return "no connection";
        case 0x04: return "page timeout";
        case 0x05: return "authentication failure";
        case 0x06: return "pin or key missing";
        case 0x08: return "connection timeout";
        case 0x09: return "too many connections";
        case 0x0B: return "connection already exists";
        case 0x0C: return "command disallowed";
        case 0x0D: return "rejected: limited resources";
        case 0x0E: return "rejected: security";
        case 0x0F: return "rejected: device";
        case 0x10: return "accept timeout";
        case 0x13: return "remote user ended";
        case 0x14: return "remote low resources";
        case 0x15: return "remote powering off";
        case 0x16: return "ended by local host";
        case 0x17: return "repeated attempts";
        case 0x18: return "pairing not allowed";
        case 0x22: return "lmp response timeout";
        case 0x29: return "unit-key pairing unsupported";
        case 0x37: return "simple pairing unsupported";
        case 0x38: return "host busy pairing";
    }
    return nullptr;
}

BtFailure classifyBtStatus(int status) {
    switch (status) {
        case 0: return BtFailure::None;
        case 9:
        case 11: return BtFailure::Refused;
        case 10:
        case 16: return BtFailure::NoAnswer;
    }
    switch (status - kHciBase) {
        case 0x00: return BtFailure::None;
        case 0x04:
        case 0x08:
        case 0x10:
        case 0x22: return BtFailure::NoAnswer;
        case 0x06: return BtFailure::StaleKey;
        case 0x05:
        case 0x0D:
        case 0x0E:
        case 0x0F:
        case 0x17:
        case 0x18:
        case 0x29:
        case 0x37:
        case 0x38: return BtFailure::Refused;
    }
    return BtFailure::Other;
}

const char* btFailureName(BtFailure f) {
    switch (f) {
        case BtFailure::None: return "ok";
        case BtFailure::NoAnswer: return "no answer";
        case BtFailure::Refused: return "refused";
        case BtFailure::StaleKey: return "stale pairing key";
        case BtFailure::Other: return "other";
    }
    return "other";
}

const char* legacyPinForAttempt(int attempt) {
    return (attempt > 0 && attempt % 2 == 1) ? "1234" : "0000";
}
