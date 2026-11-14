// bttest — stream synthesized noise over Bluetooth A2DP to one speaker.
//
// Bench test for Phase 2 (docs/plan.md): proves NoiseSynth + Gain over a
// real A2DP link before Wi-Fi is added. The target speaker's address comes
// from firmware/.env (BT_SINK_ADDR), never from tracked files.
//
// Behaviour:
//   - Scans for Bluetooth devices. Every device found is logged with its
//     signal strength (groundwork for a "pair with the strongest speaker"
//     button). Only BT_SINK_ADDR is accepted.
//   - Starts silent. Once connected, fades in over 5 s at a deliberately low
//     starting volume (D8, bedroom safety).
//   - KEY1 play/stop (fade), KEY3 next noise type (crossfaded through
//     silence), KEY5 / KEY6 volume down / up, KEY4 print status.
//   - Never calls BluetoothA2DPSource::set_volume(): on a source it also
//     sends an AVRCP absolute-volume command that changes the SPEAKER's own
//     volume. All volume is done in Gain, on the samples.
//
// The analog speaker amp (GPIO21) is held LOW and the codec is never set up.

#include <Arduino.h>
#include <BluetoothA2DPSource.h>

#include <atomic>

#include "gain.h"
#include "noise_synth.h"

#ifndef BT_SINK_ADDR
#error "BT_SINK_ADDR missing: copy firmware/.env.example to firmware/.env and set it"
#endif

namespace {

constexpr int kPinPaEnable = 21;
constexpr int kStartVolume = 35;   // percent; -32.5 dB on the Gain curve
constexpr int kCeiling = 85;       // this test build can never go louder
constexpr int kVolumeStep = 5;
constexpr uint32_t kFadeInFrames = 5 * 44100;
constexpr uint32_t kFadeOutFrames = 2 * 44100;
constexpr uint32_t kSwitchFrames = 4410;  // 100 ms out, 100 ms in on type change

struct Key {
    int number;
    int gpio;
    int last;
    uint32_t changedAt;
};
Key keys[] = {{1, 36, HIGH, 0}, {3, 19, HIGH, 0}, {4, 23, HIGH, 0},
              {5, 18, HIGH, 0}, {6, 5, HIGH, 0}};

BluetoothA2DPSource a2dp;
esp_bd_addr_t sinkAddr;

// Owned by the Bluetooth audio callback (its task only).
NoiseSynth synth(NoiseType::Brown, 1);
Gain gain(kCeiling);
bool switching = false;
bool playingNow = false;

// Requests from the main loop / Bluetooth events to the audio callback.
std::atomic<int> wantVolume{kStartVolume};
std::atomic<int> wantType{static_cast<int>(NoiseType::Brown)};
std::atomic<bool> wantPlaying{false};
std::atomic<uint32_t> framesDelivered{0};
std::atomic<bool> connected{false};

float renderBuf[2 * 256];

int32_t fillFrames(Frame* frames, int32_t count) {
    // Apply requests at callback boundaries, so the synth and gain are only
    // ever touched from this task.
    if (gain.volume() != wantVolume.load()) gain.setVolume(wantVolume.load());

    const NoiseType target = static_cast<NoiseType>(wantType.load());
    if (target != synth.type() && !switching) {
        switching = true;
        gain.fadeOut(kSwitchFrames);
    }
    if (switching && gain.silent()) {
        synth.setType(target);
        switching = false;
        if (wantPlaying.load()) gain.fadeIn(kSwitchFrames);
    }

    const bool want = wantPlaying.load();
    if (want != playingNow && !switching) {
        playingNow = want;
        if (want) gain.fadeIn(kFadeInFrames);
        else gain.fadeOut(kFadeOutFrames);
    }

    static_assert(sizeof(Frame) == 2 * sizeof(int16_t), "Frame must be packed L/R int16");
    int16_t* out = reinterpret_cast<int16_t*>(frames);
    int32_t done = 0;
    while (done < count) {
        const int32_t n = min<int32_t>(256, count - done);
        synth.render(renderBuf, n);
        gain.process(renderBuf, out + 2 * done, n);
        done += n;
    }
    framesDelivered.fetch_add(count);
    return count;
}

bool onDeviceFound(const char* name, esp_bd_addr_t addr, int rssi) {
    const bool match = memcmp(addr, sinkAddr, ESP_BD_ADDR_LEN) == 0;
    Serial.printf("[scan] %02x:%02x:%02x:%02x:%02x:%02x rssi=%d name=\"%s\"%s\n", addr[0],
                  addr[1], addr[2], addr[3], addr[4], addr[5], rssi, name ? name : "",
                  match ? "  <-- target, connecting" : "");
    return match;
}

void onConnectionState(esp_a2d_connection_state_t state, void*) {
    const char* names[] = {"disconnected", "connecting", "connected", "disconnecting"};
    Serial.printf("[a2dp] %s\n", state <= 3 ? names[state] : "?");
    const bool up = state == ESP_A2D_CONNECTION_STATE_CONNECTED;
    connected.store(up);
    wantPlaying.store(up);  // auto-play (with fade-in) once linked
}

bool parseAddr(const char* s, esp_bd_addr_t out) {
    unsigned b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(b[i]);
    return true;
}

void printStatus() {
    static uint32_t lastFrames = 0, lastMs = 0;
    const uint32_t now = millis(), f = framesDelivered.load();
    const float rate = lastMs ? (f - lastFrames) * 1000.0f / (now - lastMs) : 0.0f;
    lastFrames = f;
    lastMs = now;
    Serial.printf("[status] link=%s playing=%s type=%s vol=%d%% (ceiling %d%%) "
                  "rate=%.0f frames/s heap=%u KB min=%u KB\n",
                  connected.load() ? "up" : "down", wantPlaying.load() ? "yes" : "no",
                  noiseTypeName(static_cast<NoiseType>(wantType.load())), wantVolume.load(),
                  kCeiling, rate, ESP.getFreeHeap() >> 10, ESP.getMinFreeHeap() >> 10);
}

void onKey(int number) {
    switch (number) {
        case 1:
            wantPlaying.store(!wantPlaying.load());
            Serial.printf("[key] KEY1 -> %s\n", wantPlaying.load() ? "play" : "stop");
            break;
        case 3: {
            const NoiseType t = nextNoiseType(static_cast<NoiseType>(wantType.load()));
            wantType.store(static_cast<int>(t));
            Serial.printf("[key] KEY3 -> %s\n", noiseTypeName(t));
            break;
        }
        case 5:
            wantVolume.store(max(0, wantVolume.load() - kVolumeStep));
            Serial.printf("[key] KEY5 -> volume %d%%\n", wantVolume.load());
            break;
        case 6:
            wantVolume.store(min(kCeiling, wantVolume.load() + kVolumeStep));
            Serial.printf("[key] KEY6 -> volume %d%%\n", wantVolume.load());
            break;
        case 4:
            printStatus();
            break;
    }
}

void pollKeys() {
    const uint32_t now = millis();
    for (Key& k : keys) {
        const int level = digitalRead(k.gpio);
        if (level != k.last && now - k.changedAt > 30) {  // 30 ms debounce
            k.last = level;
            k.changedAt = now;
            if (level == LOW) onKey(k.number);
        }
    }
}

}  // namespace

void setup() {
    pinMode(kPinPaEnable, OUTPUT);
    digitalWrite(kPinPaEnable, LOW);  // analog amp off: this build is Bluetooth-only
    Serial.begin(115200);
    delay(500);
    for (Key& k : keys) pinMode(k.gpio, k.gpio >= 34 ? INPUT : INPUT_PULLUP);

    if (!parseAddr(BT_SINK_ADDR, sinkAddr)) {
        Serial.println("[bttest] BT_SINK_ADDR is not aa:bb:cc:dd:ee:ff — halting");
        while (true) delay(1000);
    }
    synth = NoiseSynth(NoiseType::Brown, esp_random());
    gain.setVolume(kStartVolume);

    Serial.printf("[bttest] target %s, start volume %d%%, ceiling %d%%\n", BT_SINK_ADDR,
                  kStartVolume, kCeiling);
    a2dp.set_local_name("bedroom-noise-test");
    a2dp.set_ssid_callback(onDeviceFound);
    a2dp.set_on_connection_state_changed(onConnectionState);
    a2dp.set_auto_reconnect(sinkAddr);
    a2dp.set_data_callback_in_frames(fillFrames);
    a2dp.start();  // no names: onDeviceFound picks the target
}

void loop() {
    static uint32_t lastStatus = 0;
    pollKeys();
    if (millis() - lastStatus > 10000) {
        lastStatus = millis();
        printStatus();
    }
    delay(5);
}
