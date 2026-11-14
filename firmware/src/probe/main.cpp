// Phase 0 bring-up probe for the AI-Thinker ESP32-A1S Audio Kit.
//
// What it does, in order, then repeats the summary every 30 s:
//   1. Chip report: model, revision, cores, Bluetooth Classic / BLE feature
//      bits, flash size, PSRAM size, MAC, SDK versions.
//   2. I2C scan on both candidate codec buses (v1: SDA 33/SCL 32, v2: SDA
//      18/SCL 23) and classification of the board variant (lib/BoardId).
//   3. SD card check in 1-bit SDMMC mode (CLK 14, CMD 15, D0 2). It only
//      mounts and reports. It NEVER formats — formatting is a human decision
//      (docs/runbooks/bring-up.md).
//   4. Key watch: prints every press/release on the keys that are safe to read
//      on this variant, plus headphone-detect (GPIO39) and aux-detect (GPIO12).
//
// What it deliberately does NOT do: make sound. The speaker power amplifier
// enable (GPIO21) is driven LOW at boot and the codec is never configured.
// Type 'r' in the serial monitor to reprint the report.

#include <Arduino.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <esp_chip_info.h>
#include <esp_mac.h>

#include "board_id.h"

namespace {

constexpr int kPinPaEnable = 21;
constexpr int kPinLed = 22;
constexpr int kPinHeadphoneDetect = 39;  // input-only, external pull-up
constexpr int kPinAuxDetect = 12;        // strapping pin (MTDI) — read only

struct Bus {
    const char* name;
    int sda;
    int scl;
};
constexpr Bus kBusV1{"v1 (SDA33/SCL32)", 33, 32};
constexpr Bus kBusV2{"v2 (SDA18/SCL23)", 18, 23};

struct Key {
    int number;
    int gpio;
};
constexpr Key kKeys[] = {{1, 36}, {2, 13}, {3, 19}, {4, 23}, {5, 18}, {6, 5}};

uint8_t foundV1[16];
size_t nFoundV1 = 0;
uint8_t foundV2[16];
size_t nFoundV2 = 0;
BoardVariant variant = BoardVariant::Unknown;

String sdSummary = "not checked";
bool watchKey[6] = {false};
int lastLevel[6] = {0};
int lastHp = -1;
int lastAux = -1;

size_t scanBus(const Bus& bus, uint8_t* out, size_t cap) {
    size_t n = 0;
    Wire.begin(bus.sda, bus.scl, 100000);
    for (uint8_t addr = 0x08; addr < 0x78; ++addr) {
        Wire.beginTransmission(addr);
        if (Wire.endTransmission() == 0 && n < cap) out[n++] = addr;
    }
    Wire.end();
    // Leave both pins as plain inputs so key reads afterwards are clean.
    pinMode(bus.sda, INPUT);
    pinMode(bus.scl, INPUT);
    return n;
}

void printAddrs(const Bus& bus, const uint8_t* a, size_t n) {
    Serial.printf("  I2C %s: ", bus.name);
    if (n == 0) Serial.print("(nothing)");
    for (size_t i = 0; i < n; ++i) Serial.printf("0x%02X ", a[i]);
    Serial.println();
}

void checkSd() {
    if (!SD_MMC.begin("/sdcard", /*mode1bit=*/true, /*format_if_mount_failed=*/false)) {
        sdSummary = "mount FAILED (no card, DIP switches DATA3/CMD off, or "
                    "unsupported filesystem e.g. exFAT) — see bring-up runbook";
        return;
    }
    const char* type = "unknown";
    switch (SD_MMC.cardType()) {
        case CARD_NONE: type = "none"; break;
        case CARD_MMC: type = "MMC"; break;
        case CARD_SD: type = "SDSC"; break;
        case CARD_SDHC: type = "SDHC/SDXC"; break;
        default: break;
    }
    char buf[160];
    snprintf(buf, sizeof buf, "mounted: type=%s card=%.1f GB fs_total=%.1f GB fs_used=%.1f GB",
             type, SD_MMC.cardSize() / 1e9, SD_MMC.totalBytes() / 1e9,
             SD_MMC.usedBytes() / 1e9);
    sdSummary = buf;
    SD_MMC.end();  // release GPIO13 (KEY2) and GPIO2 for the key watch
}

void printReport() {
    esp_chip_info_t info;
    esp_chip_info(&info);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    Serial.println();
    Serial.println("================ noise-generator probe ================");
    Serial.printf("  chip: %s rev %d, %d cores, %lu MHz\n", ESP.getChipModel(),
                  ESP.getChipRevision(), info.cores, (unsigned long)ESP.getCpuFreqMHz());
    Serial.printf("  features: WiFi=%s BT-Classic=%s BLE=%s embedded-flash=%s\n",
                  (info.features & CHIP_FEATURE_WIFI_BGN) ? "yes" : "no",
                  (info.features & CHIP_FEATURE_BT) ? "yes" : "no",
                  (info.features & CHIP_FEATURE_BLE) ? "yes" : "no",
                  (info.features & CHIP_FEATURE_EMB_FLASH) ? "yes" : "no");
#if defined(CONFIG_BT_CLASSIC_ENABLED)
    Serial.println("  firmware: Bluetooth Classic compiled in (A2DP source possible)");
#else
    Serial.println("  firmware: Bluetooth Classic NOT compiled in this core build");
#endif
    Serial.printf("  flash: %lu MB   PSRAM: %lu KB (found=%s)   free heap: %lu KB\n",
                  (unsigned long)(ESP.getFlashChipSize() >> 20),
                  (unsigned long)(ESP.getPsramSize() >> 10), psramFound() ? "yes" : "no",
                  (unsigned long)(ESP.getFreeHeap() >> 10));
    Serial.printf("  MAC (wifi sta): %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2],
                  mac[3], mac[4], mac[5]);
    Serial.printf("  SDK: %s   Arduino core: %d.%d.%d\n", ESP.getSdkVersion(),
                  ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR,
                  ESP_ARDUINO_VERSION_PATCH);
    printAddrs(kBusV1, foundV1, nFoundV1);
    printAddrs(kBusV2, foundV2, nFoundV2);
    Serial.printf("  board variant: %s\n", variantName(variant));
    Serial.printf("  usable keys: with SD card [%s], without [%s]\n", usableKeys(variant, true),
                  usableKeys(variant, false));
    Serial.printf("  SD card: %s\n", sdSummary.c_str());
    Serial.printf("  headphone-detect GPIO39=%d  aux-detect GPIO12=%d (0 = plugged, usually)\n",
                  digitalRead(kPinHeadphoneDetect), digitalRead(kPinAuxDetect));
    Serial.println("  press keys now — each press/release is logged. 'r' reprints.");
    Serial.println("========================================================");
}

void setupKeyWatch() {
    for (int i = 0; i < 6; ++i) {
        const Key& k = kKeys[i];
        // On v2, 23/18/5 belong to the codec — never treat them as buttons.
        const bool codecPin = (variant == BoardVariant::Es8388V2) &&
                              (k.gpio == 23 || k.gpio == 18 || k.gpio == 5);
        watchKey[i] = !codecPin;
        if (!watchKey[i]) continue;
        // GPIO36 is input-only and has no internal pull-up; the board has one.
        pinMode(k.gpio, k.gpio >= 34 ? INPUT : INPUT_PULLUP);
        lastLevel[i] = digitalRead(k.gpio);
    }
}

void pollKeys() {
    for (int i = 0; i < 6; ++i) {
        if (!watchKey[i]) continue;
        const int level = digitalRead(kKeys[i].gpio);
        if (level != lastLevel[i]) {
            lastLevel[i] = level;
            Serial.printf("[key] KEY%d (GPIO%d) %s\n", kKeys[i].number, kKeys[i].gpio,
                          level == LOW ? "PRESSED" : "released");
        }
    }
    const int hp = digitalRead(kPinHeadphoneDetect);
    const int aux = digitalRead(kPinAuxDetect);
    if (hp != lastHp) {
        Serial.printf("[jack] headphone-detect GPIO39 -> %d\n", hp);
        lastHp = hp;
    }
    if (aux != lastAux) {
        Serial.printf("[jack] aux-detect GPIO12 -> %d\n", aux);
        lastAux = aux;
    }
}

}  // namespace

void setup() {
    // Speaker amp OFF before anything else: this firmware must stay silent.
    pinMode(kPinPaEnable, OUTPUT);
    digitalWrite(kPinPaEnable, LOW);

    Serial.begin(115200);
    delay(1500);  // give a freshly opened monitor time to attach

    pinMode(kPinLed, OUTPUT);
    digitalWrite(kPinLed, LOW);  // one short blink = probe alive (bench only)
    delay(150);
    digitalWrite(kPinLed, HIGH);

    pinMode(kPinHeadphoneDetect, INPUT);
    pinMode(kPinAuxDetect, INPUT);

    nFoundV1 = scanBus(kBusV1, foundV1, sizeof foundV1);
    nFoundV2 = scanBus(kBusV2, foundV2, sizeof foundV2);
    variant = classifyBoard(foundV1, nFoundV1, foundV2, nFoundV2);

    checkSd();
    setupKeyWatch();
    printReport();
}

void loop() {
    static uint32_t lastReport = millis();
    pollKeys();
    if (Serial.available() && Serial.read() == 'r') printReport();
    if (millis() - lastReport > 30000) {
        lastReport = millis();
        printReport();
    }
    delay(10);
}
