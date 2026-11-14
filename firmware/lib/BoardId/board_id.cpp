#include "board_id.h"

namespace {

bool contains(const uint8_t* addrs, size_t n, uint8_t want) {
    if (addrs == nullptr) return false;
    for (size_t i = 0; i < n; ++i) {
        if (addrs[i] == want) return true;
    }
    return false;
}

}  // namespace

BoardVariant classifyBoard(const uint8_t* busV1, size_t nV1,
                           const uint8_t* busV2, size_t nV2) {
    const bool es8388OnV1 = contains(busV1, nV1, kEs8388Addr);
    const bool es8388OnV2 = contains(busV2, nV2, kEs8388Addr);
    const bool ac101OnV1 = contains(busV1, nV1, kAc101Addr);

    const int codecHits = (es8388OnV1 ? 1 : 0) + (es8388OnV2 ? 1 : 0) + (ac101OnV1 ? 1 : 0);
    if (codecHits > 1) return BoardVariant::Ambiguous;
    if (es8388OnV1) return BoardVariant::Es8388V1;
    if (es8388OnV2) return BoardVariant::Es8388V2;
    if (ac101OnV1) return BoardVariant::Ac101;
    return BoardVariant::Unknown;
}

const char* usableKeys(BoardVariant v, bool sdCardInUse) {
    // KEY2 is GPIO13 = SD DAT3. On v2, KEY4/5/6 are GPIO 23/18/5 = codec
    // I2C SCL / SDA / I2S BCK. The AC101 board shares v1's key wiring.
    switch (v) {
        case BoardVariant::Es8388V1:
        case BoardVariant::Ac101:
            return sdCardInUse ? "1,3,4,5,6" : "1,2,3,4,5,6";
        case BoardVariant::Es8388V2:
            return sdCardInUse ? "1,3" : "1,2,3";
        default:
            return "";
    }
}

const char* variantName(BoardVariant v) {
    switch (v) {
        case BoardVariant::Es8388V1: return "ES8388 v1 (I2C 33/32)";
        case BoardVariant::Es8388V2: return "ES8388 v2 (I2C 18/23)";
        case BoardVariant::Ac101: return "AC101 (end-of-life codec)";
        case BoardVariant::Ambiguous: return "ambiguous";
        default: return "unknown";
    }
}
