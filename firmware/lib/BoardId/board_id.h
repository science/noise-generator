// board_id — pure logic for identifying the AI-Thinker ESP32-A1S Audio Kit
// variant from I2C scan results. No Arduino dependencies, so it runs in the
// native (host) unit tests. See docs/hardware.md for the pin maps.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class BoardVariant {
    Unknown,
    Es8388V1,   // codec I2C on SDA 33 / SCL 32 (all six keys wired)
    Es8388V2,   // codec I2C on SDA 18 / SCL 23, I2S BCK on 5 (KEY4-6 lost)
    Ac101,      // older, end-of-life codec at I2C 0x1A
    Ambiguous,  // a codec answered on both buses; refuse to guess
};

constexpr uint8_t kEs8388Addr = 0x10;
constexpr uint8_t kAc101Addr = 0x1A;

// busV1: addresses that ACKed on SDA 33 / SCL 32.
// busV2: addresses that ACKed on SDA 18 / SCL 23.
BoardVariant classifyBoard(const uint8_t* busV1, size_t nV1,
                           const uint8_t* busV2, size_t nV2);

// Comma-separated list of KEY numbers (1-6) usable as buttons on this
// variant, given whether the SD card is in use. Empty for unknown variants.
const char* usableKeys(BoardVariant v, bool sdCardInUse);

const char* variantName(BoardVariant v);
