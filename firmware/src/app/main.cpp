// noise-generator — the bedroom firmware (first-night build).
//
// What it does (docs/plans/implementation-plan.md; user answers Q1-Q11, F1-F9):
//   - Synthesizes the tone ladder (ToneEngine; default "deep brown") and
//     streams it over Bluetooth A2DP to one speaker (BT_SINK_ADDR, .env).
//   - Autoplay window 21:00-09:00 US/Pacific; KEY1 start/stop rules live in
//     Controller. 5 s fades for everything.
//   - Keys: KEY1 start/stop, KEY2 darker, KEY3 brighter, KEY5/6 volume.
//     Tone and volume persist in NVS (survive reboots and OTA updates).
//   - Bluetooth link is kept up at all times, streaming silence when off,
//     so the speaker stays awake (Q9). Reconnect retries never give up.
//   - Wi-Fi is on-demand (Q7): at boot to set the clock, and once a day at
//     15:00 for a resync while quiet. In the daytime, boot opens a 10-minute
//     OTA window; KEY1 ends it early and starts the noise (user F8). At
//     night, boot skips the window: noise first.
//   - Wi-Fi and Bluetooth never run at the same time. v0.2.0 tried starting
//     Bluetooth during the OTA window: free heap fell to 37 KB (min 17 KB),
//     too close to a crash. So the window runs first, then Bluetooth. The
//     library waits 10 s before its first connect, so after KEY1 closes the
//     window, further KEY1 presses are ignored until sound starts (max 30 s);
//     otherwise an impatient second press toggled it straight back off.
//   - Hold KEY4 for 3 s (or send 'o' over serial): save settings, fade out,
//     restart straight into the 10-minute OTA window at any time of day
//     (v0.2.2). Needed because a night boot skips the window and Wi-Fi is
//     otherwise off. When the window ends the schedule resumes; at night,
//     KEY1 closing the window does not count as "stop".
//   - Daily clock resync = a quiet software restart at 15:00 when nothing is
//     playing: it syncs NTP and goes straight to Bluetooth (no OTA window;
//     only RST / power-on opens the window). Same if the clock is unknown:
//     retry by restart every 30 min while idle.
//   - OTA rollback: a new image marks itself valid only after 60 s of
//     healthy running; a crash before that rolls back to the previous image.
//
// Never calls a2dp.set_volume(): on a source that sends AVRCP absolute volume
// and changes the SPEAKER's own setting (decisions D2). Volume is Gain only.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <BluetoothA2DPSource.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <time.h>

#include <atomic>

#include "controller.h"
#include "gain.h"
#include "schedule.h"
#include "tone_engine.h"

#ifndef BT_SINK_ADDR
#error "BT_SINK_ADDR missing: see firmware/.env.example"
#endif
#ifndef WIFI_SSID
#error "WIFI_SSID missing: see firmware/.env.example"
#endif
#ifndef WIFI_SSID_FALLBACK
#define WIFI_SSID_FALLBACK ""
#endif
#ifndef OTA_PASSWORD
#error "OTA_PASSWORD missing: see firmware/.env.example"
#endif

#define FIRMWARE_VERSION "0.2.2"

namespace {

// --- configuration ---------------------------------------------------------
constexpr int kPinPaEnable = 21;
constexpr int kCeiling = 90;           // never louder than this (D8)
constexpr int kDefaultVolume = 60;     // first boot only; then the saved level
constexpr int kVolumeStep = 5;
constexpr uint32_t kFadeFrames = 5 * 44100;            // 5 s (user Q4)
constexpr uint32_t kOtaWindowMs = 10UL * 60UL * 1000UL;  // 10 min (user F8)
constexpr uint32_t kSaveDelayMs = 3000;                // batch NVS writes
constexpr uint32_t kMarkValidAfterMs = 60000;
constexpr int kResyncHour = 15;                        // daily restart for clock resync, if quiet
constexpr uint32_t kKey1LockoutMs = 30000;             // after KEY1 closes the window
constexpr uint32_t kOtaHoldMs = 3000;                  // hold KEY4 this long -> OTA window
const Window kNight{21 * 60, 9 * 60};

struct Key {
    int number;
    int gpio;
    int last;
    uint32_t changedAt;
};
Key keys[] = {{1, 36, HIGH, 0}, {2, 13, HIGH, 0}, {3, 19, HIGH, 0},
              {4, 23, HIGH, 0}, {5, 18, HIGH, 0}, {6, 5, HIGH, 0}};

// --- shared state ----------------------------------------------------------
Preferences prefs;
BluetoothA2DPSource a2dp;
esp_bd_addr_t sinkAddr;

// Requests from the main loop to the audio callback (Bluetooth task).
std::atomic<int> wantVolume{kDefaultVolume};
std::atomic<int> wantTone{kDefaultTone};
std::atomic<bool> wantPlaying{false};
std::atomic<uint32_t> framesDelivered{0};
std::atomic<bool> linkUp{false};
std::atomic<uint32_t> connects{0}, disconnects{0};

// Owned by the audio callback only.
ToneEngine engine(kDefaultTone, 1);
Gain gain(kCeiling);
bool playingNow = false;
float renderBuf[2 * 256];

Controller* controller = nullptr;
bool timeKnown = false;
bool settingsDirty = false;
uint32_t lastSettingChange = 0;
bool markedValid = false;
int lastResyncDay = -1;
uint32_t key1LockedUntil = 0;  // ignore KEY1 until sound starts or this time

// --- audio -----------------------------------------------------------------
int32_t fillFrames(Frame* frames, int32_t count) {
    if (gain.volume() != wantVolume.load()) gain.setVolume(wantVolume.load());
    if (engine.step() != wantTone.load()) engine.setStep(wantTone.load());
    const bool want = wantPlaying.load();
    if (want != playingNow) {
        playingNow = want;
        if (want) gain.fadeIn(kFadeFrames);
        else gain.fadeOut(kFadeFrames);
    }
    int16_t* out = reinterpret_cast<int16_t*>(frames);
    if (gain.silent()) {
        memset(out, 0, sizeof(int16_t) * 2 * count);  // silence keeps the link active
    } else {
        int32_t done = 0;
        while (done < count) {
            const int32_t n = min<int32_t>(256, count - done);
            engine.render(renderBuf, n);
            gain.process(renderBuf, out + 2 * done, n);
            done += n;
        }
    }
    framesDelivered.fetch_add(count);
    return count;
}

bool onDeviceFound(const char* name, esp_bd_addr_t addr, int rssi) {
    const bool match = memcmp(addr, sinkAddr, ESP_BD_ADDR_LEN) == 0;
    if (match) Serial.printf("[scan] found target \"%s\" rssi=%d, connecting\n", name ? name : "", rssi);
    return match;
}

void onConnectionState(esp_a2d_connection_state_t state, void*) {
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        linkUp.store(true);
        connects.fetch_add(1);
        Serial.println("[a2dp] connected");
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        if (linkUp.exchange(false)) disconnects.fetch_add(1);
        Serial.println("[a2dp] disconnected (will keep retrying)");
    }
}

bool parseAddr(const char* s, esp_bd_addr_t out) {
    unsigned b[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) return false;
    for (int i = 0; i < 6; ++i) out[i] = static_cast<uint8_t>(b[i]);
    return true;
}

void startBluetooth() {
    // The library remembers the last speaker in NVS and that address wins on
    // boot. If the configured speaker changed (e.g. desk -> bedroom), forget
    // the old one so it cannot keep reconnecting to the wrong speaker.
    const String configured = BT_SINK_ADDR;
    if (prefs.getString("sinkcfg", "") != configured) {
        Serial.printf("[bt] speaker config changed -> %s; clearing remembered speaker\n",
                      configured.c_str());
        a2dp.clean_last_connection();
        prefs.putString("sinkcfg", configured);
    }
    a2dp.set_local_name("bedroom-noise");
    a2dp.set_ssid_callback(onDeviceFound);
    a2dp.set_on_connection_state_changed(onConnectionState);
    // Effectively infinite retries: the library resets the counter on every
    // successful connect, and never falls back to a scan that might miss a
    // speaker that is not discoverable after power-on.
    a2dp.set_auto_reconnect(sinkAddr, 1000000);
    a2dp.set_data_callback_in_frames(fillFrames);
    a2dp.start();
}

// --- time / Wi-Fi ------------------------------------------------------------
bool clockValid() { return time(nullptr) > 1700000000; }  // after Nov 2023

bool localMinuteOfDay(int& minute, int& hour, int& yday) {
    if (!clockValid()) return false;
    const time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    minute = t.tm_hour * 60 + t.tm_min;
    hour = t.tm_hour;
    yday = t.tm_yday;
    return true;
}

bool wifiConnect(uint32_t timeoutMs) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("bedroom-noise");
    const char* ssids[] = {WIFI_SSID, WIFI_SSID_FALLBACK};
    for (const char* ssid : ssids) {
        if (!ssid || !*ssid) continue;
        WiFi.begin(ssid, WIFI_PASSWORD);
        const uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(100);
        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[wifi] connected to %s, ip %s, rssi %d\n", ssid,
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            return true;
        }
        WiFi.disconnect(true);
    }
    Serial.println("[wifi] could not connect");
    return false;
}

void wifiOff() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("[wifi] off");
}

bool syncClock(uint32_t timeoutMs) {
    configTzTime(kPacificTz, "pool.ntp.org", "time.google.com");
    const uint32_t t0 = millis();
    while (!clockValid() && millis() - t0 < timeoutMs) delay(100);
    if (clockValid()) {
        const time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        char buf[40];
        strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %Z", &t);
        Serial.printf("[time] synced: %s\n", buf);
        return true;
    }
    Serial.println("[time] NTP sync failed");
    return false;
}

// Restart quietly to resync the clock (Wi-Fi and Bluetooth must not share
// the heap). Only called while nothing is playing. Settings are saved first.
void restartForResync(const char* why) {
    Serial.printf("[time] %s: quiet restart to resync the clock\n", why);
    prefs.putInt("tone2", wantTone.load());
    prefs.putInt("volume", wantVolume.load());
    delay(200);
    ESP.restart();
}

// Restart straight into the OTA window (any time of day). The request is a
// one-shot NVS flag read and cleared at boot.
void restartIntoOtaWindow(const char* why) {
    Serial.printf("[ota] %s: restarting into the update window\n", why);
    wantPlaying.store(false);
    delay(1200);  // let a short fade begin; the restart cuts the rest
    prefs.putInt("tone2", wantTone.load());
    prefs.putInt("volume", wantVolume.load());
    prefs.putBool("otareq", true);
    delay(100);
    ESP.restart();
}

// --- settings ----------------------------------------------------------------
void loadSettings() {
    prefs.begin("noisegen", false);
    // "tone2": ladder v2 (re-centred). The v1 key "tone" indexed a different
    // ladder, so it is deliberately not read: everyone starts mid-ladder.
    wantTone.store(constrain(prefs.getInt("tone2", kDefaultTone), 0, kToneSteps - 1));
    wantVolume.store(constrain(prefs.getInt("volume", kDefaultVolume), 0, kCeiling));
    Serial.printf("[settings] tone %d (%s), volume %d%%\n", wantTone.load(),
                  toneName(wantTone.load()), wantVolume.load());
}

void saveSettingsIfDue() {
    if (!settingsDirty || millis() - lastSettingChange < kSaveDelayMs) return;
    prefs.putInt("tone2", wantTone.load());
    prefs.putInt("volume", wantVolume.load());
    settingsDirty = false;
    Serial.printf("[settings] saved tone %d, volume %d%%\n", wantTone.load(), wantVolume.load());
}

void settingChanged() {
    settingsDirty = true;
    lastSettingChange = millis();
}

// --- keys --------------------------------------------------------------------
// Returns the number of a key that was just pressed (debounced), or 0.
int pollKeys() {
    const uint32_t now = millis();
    for (Key& k : keys) {
        const int level = digitalRead(k.gpio);
        if (level != k.last && now - k.changedAt > 30) {
            k.last = level;
            k.changedAt = now;
            if (level == LOW) return k.number;
        }
    }
    return 0;
}

void printStatus();

// Tone and volume keys work everywhere (including the OTA window).
// Returns true if the key was handled here.
bool handleSettingKey(int key) {
    switch (key) {
        case 2:  // darker
            wantTone.store(min(kToneSteps - 1, wantTone.load() + 1));
            Serial.printf("[key] darker -> %d %s\n", wantTone.load(), toneName(wantTone.load()));
            settingChanged();
            return true;
        case 3:  // brighter
            wantTone.store(max(0, wantTone.load() - 1));
            Serial.printf("[key] brighter -> %d %s\n", wantTone.load(), toneName(wantTone.load()));
            settingChanged();
            return true;
        case 5:
            wantVolume.store(max(0, wantVolume.load() - kVolumeStep));
            Serial.printf("[key] volume %d%%\n", wantVolume.load());
            settingChanged();
            return true;
        case 6:
            wantVolume.store(min(kCeiling, wantVolume.load() + kVolumeStep));
            Serial.printf("[key] volume %d%%\n", wantVolume.load());
            settingChanged();
            return true;
        case 4:
            printStatus();
            return true;
    }
    return false;
}

// A freshly pushed image is "pending verify" until it marks itself valid; a
// reset before that rolls back to the previous image. Called from BOTH the
// OTA-window loop and loop(): the boot window can last 10 minutes, and v0.1.1
// rolled back on the bench because only loop() made this call.
void markValidIfDue() {
    if (markedValid || millis() < kMarkValidAfterMs) return;
    markedValid = true;
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[ota] running image marked valid");
}

// --- OTA window ----------------------------------------------------------------
// Returns true if KEY1 ended the window (the press then counts as START).
bool runOtaWindow() {
    ArduinoOTA.setHostname("bedroom-noise");
    ArduinoOTA.setPassword(OTA_PASSWORD);
    ArduinoOTA.onStart([]() { Serial.println("[ota] receiving update..."); });
    ArduinoOTA.onEnd([]() { Serial.println("[ota] update received, rebooting"); });
    ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
    ArduinoOTA.begin();
    Serial.printf("[ota] window open for 10 min at %s (KEY1 ends it and starts noise)\n",
                  WiFi.localIP().toString().c_str());
    const uint32_t t0 = millis();
    uint32_t lastNote = t0;
    while (millis() - t0 < kOtaWindowMs) {
        ArduinoOTA.handle();
        const int key = pollKeys();
        if (key == 1) {
            Serial.println("[ota] KEY1: closing window, starting noise");
            ArduinoOTA.end();
            return true;
        }
        if (key) handleSettingKey(key);
        saveSettingsIfDue();
        markValidIfDue();
        if (millis() - lastNote > 60000) {
            lastNote = millis();
            Serial.printf("[ota] window: %lu min left | link=%s | heap %u KB (min %u)\n",
                          (unsigned long)((kOtaWindowMs - (millis() - t0)) / 60000 + 1),
                          linkUp.load() ? "up" : "down", ESP.getFreeHeap() >> 10,
                          ESP.getMinFreeHeap() >> 10);
        }
        delay(5);
    }
    ArduinoOTA.end();
    Serial.println("[ota] window closed");
    return false;
}

// --- status --------------------------------------------------------------------
void printStatus() {
    static uint32_t lastFrames = 0, lastMs = 0;
    const uint32_t now = millis(), f = framesDelivered.load();
    const float rate = lastMs ? (f - lastFrames) * 1000.0f / (now - lastMs) : 0.0f;
    lastFrames = f;
    lastMs = now;
    char clock[32] = "clock unknown";
    if (clockValid()) {
        const time_t t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        strftime(clock, sizeof clock, "%a %H:%M:%S %Z", &lt);
    }
    Serial.printf("[status] v%s %s | %s%s | link=%s (connects %u, drops %u) | tone %d %s | "
                  "vol %d%% | %.0f frames/s | heap %u KB (min %u)\n",
                  FIRMWARE_VERSION, clock, controller ? controller->stateName() : "-",
                  controller && controller->runMinutesLeft(now) ? " " : "",
                  linkUp.load() ? "up" : "down", connects.load(), disconnects.load(),
                  wantTone.load(), toneName(wantTone.load()), wantVolume.load(), rate,
                  ESP.getFreeHeap() >> 10, ESP.getMinFreeHeap() >> 10);
    if (controller && controller->runMinutesLeft(now)) {
        Serial.printf("[status] daytime run: %u min left\n", controller->runMinutesLeft(now));
    }
}

}  // namespace

// Tell the Arduino core we verify OTA images ourselves (mark valid after 60 s).
extern "C" bool verifyRollbackLater() { return true; }

void setup() {
    pinMode(kPinPaEnable, OUTPUT);
    digitalWrite(kPinPaEnable, LOW);  // analog amp off: Bluetooth-only build
    Serial.begin(115200);
    delay(300);
    Serial.printf("\n[boot] noise-generator v%s (built %s %s)\n", FIRMWARE_VERSION, __DATE__, __TIME__);
    for (Key& k : keys) pinMode(k.gpio, k.gpio >= 34 ? INPUT : INPUT_PULLUP);
    if (!parseAddr(BT_SINK_ADDR, sinkAddr)) {
        Serial.println("[boot] BT_SINK_ADDR malformed - halting");
        while (true) delay(1000);
    }
    loadSettings();
    engine = ToneEngine(wantTone.load(), esp_random());
    gain.setVolume(wantVolume.load());

    // 1. Clock (and maybe the OTA window) before Bluetooth: no radio sharing.
    setenv("TZ", kPacificTz, 1);
    tzset();
    const bool wifi = wifiConnect(12000);
    if (wifi) syncClock(10000);
    timeKnown = clockValid();

    int minute = 0, hour = 0, yday = 0;
    const bool night = timeKnown ? (localMinuteOfDay(minute, hour, yday), inWindow(kNight, minute))
                                 : Controller::kAssumeNightUntilClockKnown;
    if (!timeKnown) Serial.println("[boot] clock unknown: assuming night (fail toward noise)");

    lastResyncDay = timeKnown ? yday : -1;

    // 2. Daytime power-on / RST: OTA window first (Wi-Fi only). A software
    //    restart (the daily resync) skips it. KEY1 closes it = START.
    const esp_reset_reason_t reason = esp_reset_reason();
    const bool softRestart = reason == ESP_RST_SW;
    const bool otaRequested = prefs.getBool("otareq", false);
    if (otaRequested) prefs.putBool("otareq", false);  // one-shot
    bool startPressed = false;
    if (wifi && otaRequested) startPressed = runOtaWindow();
    else if (wifi && timeKnown && !night && !softRestart) startPressed = runOtaWindow();
    else if (wifi && night) Serial.println("[boot] night window: no OTA window, noise first");
    else if (wifi && softRestart) Serial.println("[boot] software restart: no OTA window");
    else if (otaRequested) Serial.println("[ota] window requested but Wi-Fi failed");
    if (wifi) wifiOff();

    // 3. Controller, then Bluetooth.
    controller = new Controller(night, millis());
    // KEY1 closing the window means "noise now": only press START if the
    // schedule isn't already playing (at night it would otherwise STOP).
    if (startPressed && !controller->shouldPlay()) {
        controller->pressStartStop(millis());
        key1LockedUntil = millis() + kKey1LockoutMs;
        Serial.println("[key] start/stop -> daytime run (2 h); KEY1 locked until sound starts");
    } else if (startPressed) {
        key1LockedUntil = millis() + kKey1LockoutMs;
        Serial.println("[key] window closed; schedule already playing");
    }
    wantPlaying.store(controller->shouldPlay());
    Serial.printf("[boot] state: %s | heap before Bluetooth %u KB\n", controller->stateName(),
                  ESP.getFreeHeap() >> 10);
    startBluetooth();
}

void loop() {
    static uint32_t lastSecond = 0, lastStatus = 0, lastRetry = 0;
    const uint32_t now = millis();

    // KEY1 lockout ends once sound is actually flowing (link up and playing)
    // or after kKey1LockoutMs, whichever comes first.
    if (key1LockedUntil && (int32_t)(now - key1LockedUntil) >= 0) key1LockedUntil = 0;
    if (key1LockedUntil && linkUp.load() && playingNow) key1LockedUntil = 0;

    // Hold KEY4 for 3 s -> restart into the OTA window.
    static uint32_t key4DownSince = 0;
    if (digitalRead(23) == LOW) {
        if (!key4DownSince) key4DownSince = now ? now : 1;
        else if (now - key4DownSince >= kOtaHoldMs) restartIntoOtaWindow("KEY4 held");
    } else {
        key4DownSince = 0;
    }

    const int key = pollKeys();
    if (key == 1 && key1LockedUntil) {
        Serial.println("[key] start/stop ignored: still connecting to the speaker");
    } else if (key == 1) {
        controller->pressStartStop(now);
        Serial.printf("[key] start/stop -> %s\n", controller->stateName());
    } else if (key) {
        handleSettingKey(key);
    }

    if (now - lastSecond >= 1000) {
        lastSecond = now;
        int minute, hour, yday;
        if (localMinuteOfDay(minute, hour, yday)) {
            const bool was = controller->inWindow();
            controller->setInWindow(inWindow(kNight, minute), now);
            if (was != controller->inWindow()) {
                Serial.printf("[schedule] window %s -> %s\n",
                              controller->inWindow() ? "start" : "end", controller->stateName());
            }
            // Daily resync at 15:00 while quiet (Wi-Fi briefly shares the radio
            // with an idle, silent Bluetooth link).
            if (hour == kResyncHour && yday != lastResyncDay && !controller->shouldPlay() &&
                gain.silent() && now > 10UL * 60UL * 1000UL) {
                lastResyncDay = yday;
                restartForResync("daily 15:00");
            }
        } else if (now - lastRetry > 30UL * 60UL * 1000UL) {
            lastRetry = now;  // clock never synced: retry by restart while idle
            if (!controller->shouldPlay() && gain.silent()) restartForResync("clock unknown");
        }
        controller->tick(now);
        const bool play = controller->shouldPlay();
        if (play != wantPlaying.load()) {
            wantPlaying.store(play);
            Serial.printf("[audio] %s\n", play ? "fade in" : "fade out");
        }
    }

    if (Serial.available()) {
        const int c = Serial.read();
        if (c == 'r') printStatus();
        if (c == 's' && !controller->shouldPlay()) restartForResync("serial 's'");
        if (c == 'o') restartIntoOtaWindow("serial 'o'");
    }

    saveSettingsIfDue();

    markValidIfDue();

    if (now - lastStatus >= 60000) {
        lastStatus = now;
        printStatus();
    }
    delay(5);
}
