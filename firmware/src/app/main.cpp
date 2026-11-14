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
//   - Bluetooth diagnostics (v0.3.0, after the first bedroom attempt failed
//     with no way to see why): every connect attempt logs its outcome in
//     words (no answer / refused / stale pairing key), with a periodic scan
//     that reports whether the speaker is visible and how strong it is.
//     Event lines go to a 4 KB log in RTC memory that survives a software
//     restart, so: hold KEY4 -> update window -> `nc <board-ip> 2323` dumps
//     it. (RST and power loss clear it.)
//   - Connect robustness (v0.3.0): legacy-PIN pairing answers 0000 then 1234
//     (the library default was 1234 only; Edifier documents 0000); a "key
//     missing" failure deletes our stale pairing key so the next attempt
//     re-pairs; page timeout 7.68 s instead of 5.12 s, for speakers that
//     listen for calls only every 2.56 s.
//
// Never calls a2dp.set_volume(): on a source that sends AVRCP absolute volume
// and changes the SPEAKER's own setting (decisions D2). Volume is Gain only.

#include <Arduino.h>
#include <ArduinoOTA.h>
#include <BluetoothA2DPSource.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <esp_gap_bt_api.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>

#include <atomic>

#include "bt_diag.h"
#include "controller.h"
#include "log_ring.h"
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

#define FIRMWARE_VERSION "0.3.1"

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
constexpr uint16_t kLogPort = 2323;                    // update window: TCP log dump
constexpr uint16_t kPageTimeoutSlots = 0x3000;         // 7.68 s (0.625 ms slots)
constexpr uint32_t kScanEveryNoAnswers = 6;            // inquiry after this many no-answers
const Window kNight{21 * 60, 9 * 60};

struct Key {
    int number;
    int gpio;
    int last;
    uint32_t changedAt;
};
Key keys[] = {{1, 36, HIGH, 0}, {2, 13, HIGH, 0}, {3, 19, HIGH, 0},
              {4, 23, HIGH, 0}, {5, 18, HIGH, 0}, {6, 5, HIGH, 0}};

// --- event log (Serial + restart-surviving RTC ring) -------------------------
RTC_NOINIT_ATTR LogRingStore rtcLog;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

bool clockValid() { return time(nullptr) > 1700000000; }  // after Nov 2023

// One event line: printed to Serial and kept in the RTC log. Prefixed with
// local time when the clock is known, else seconds since boot. Callable from
// any task (the Bluetooth callbacks run on the Bluetooth task).
void note(const char* fmt, ...) {
    char line[200];
    int n;
    if (clockValid()) {
        const time_t t = time(nullptr);
        struct tm lt;
        localtime_r(&t, &lt);
        n = snprintf(line, sizeof line, "%02d:%02d:%02d ", lt.tm_hour, lt.tm_min, lt.tm_sec);
    } else {
        n = snprintf(line, sizeof line, "+%lus ", (unsigned long)(millis() / 1000));
    }
    va_list ap;
    va_start(ap, fmt);
    const int m = vsnprintf(line + n, sizeof line - n - 1, fmt, ap);
    va_end(ap);
    n = m < 0 ? n : min<int>(n + m, sizeof line - 2);
    if (n == 0 || line[n - 1] != '\n') line[n++] = '\n';
    line[n] = 0;
    Serial.print(line);
    portENTER_CRITICAL(&logMux);
    logRingAppend(rtcLog, line, n);
    portEXIT_CRITICAL(&logMux);
}

// Copies the log out (oldest first). Returns a malloc'd buffer the caller
// frees, or nullptr.
char* copyLog(size_t& len) {
    char* buf = static_cast<char*>(malloc(kLogRingBytes));
    if (!buf) return nullptr;
    const char *a, *b;
    size_t al, bl;
    portENTER_CRITICAL(&logMux);
    logRingSpans(rtcLog, a, al, b, bl);
    memcpy(buf, a, al);
    memcpy(buf + al, b, bl);
    portEXIT_CRITICAL(&logMux);
    len = al + bl;
    return buf;
}

template <typename Out>
void dumpLog(Out& out) {
    size_t len = 0;
    char* buf = copyLog(len);
    if (!buf) return;
    for (size_t i = 0; i < len; i += 512) out.write(reinterpret_cast<uint8_t*>(buf + i), min<size_t>(512, len - i));
    free(buf);
}

// --- Bluetooth: the library plus diagnostics and connect fallbacks ----------
esp_bd_addr_t sinkAddr;

bool isSink(const uint8_t* bda) { return memcmp(bda, sinkAddr, ESP_BD_ADDR_LEN) == 0; }

bool isBonded(const uint8_t* bda) {
    esp_bd_addr_t list[8];
    int n = 8;
    if (esp_bt_gap_get_bond_device_list(&n, list) != ESP_OK) return false;
    for (int i = 0; i < n; ++i)
        if (memcmp(list[i], bda, ESP_BD_ADDR_LEN) == 0) return true;
    return false;
}

const char* statusText(int stat, char* buf, size_t len) {
    const char* name = btStatusName(stat);
    if (name) return name;
    snprintf(buf, len, "status 0x%x", stat);
    return buf;
}

// Wraps ESP32-A2DP's GAP event handler (it is virtual). Everything is passed
// on to the library except legacy PIN requests (answered here, rotating
// 0000/1234) and the results of our own diagnostic scans.
class NoiseA2DP : public BluetoothA2DPSource {
public:
    std::atomic<uint32_t> attempts{0}, noAnswer{0}, refused{0}, staleKeys{0};
    std::atomic<int> lastStatus{0};
    std::atomic<int> lastRssi{-127};  // speaker RSSI seen by the last scan (-127 = not seen)

protected:
    void app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) override {
        char tmp[24];
        switch (event) {
            case ESP_BT_GAP_ACL_CONN_CMPL_STAT_EVT: {
                const auto& p = param->acl_conn_cmpl_stat;
                if (!isSink(p.bda)) break;
                attempts.fetch_add(1);
                lastStatus.store(p.stat);
                const BtFailure f = classifyBtStatus(p.stat);
                if (f == BtFailure::None) {
                    note("[bt] speaker answered (link up after %u no-answers)", streak_);
                    streak_ = 0;
                } else if (f == BtFailure::NoAnswer) {
                    noAnswer.fetch_add(1);
                    ++streak_;
                    if (streak_ == 1 || streak_ % 30 == 0)
                        note("[bt] no answer from speaker (%s) x%u", statusText(p.stat, tmp, sizeof tmp), streak_);
                    if (streak_ % kScanEveryNoAnswers == 0) startScan();
                } else {
                    note("[bt] speaker %s: %s", btFailureName(f), statusText(p.stat, tmp, sizeof tmp));
                    if (f == BtFailure::Refused) refused.fetch_add(1);
                    if (f == BtFailure::StaleKey) forgetStaleKey(p.bda);
                }
                break;
            }
            case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT: {
                const auto& p = param->acl_disconn_cmpl_stat;
                if (isSink(p.bda))
                    note("[bt] link down: %s", statusText(p.reason, tmp, sizeof tmp));
                break;
            }
            case ESP_BT_GAP_AUTH_CMPL_EVT: {
                const auto& p = param->auth_cmpl;
                const BtFailure f = classifyBtStatus(p.stat);
                if (f == BtFailure::None) {
                    note("[bt] paired/authenticated with \"%s\"", (const char*)p.device_name);
                } else {
                    note("[bt] pairing failed (%s): %s", btFailureName(f), statusText(p.stat, tmp, sizeof tmp));
                    if (f == BtFailure::StaleKey) forgetStaleKey(p.bda);
                    else refused.fetch_add(1);
                }
                break;
            }
            case ESP_BT_GAP_PIN_REQ_EVT: {
                // Legacy (pre-2.1) pairing. Not passed on: the library would
                // answer with its single fixed PIN.
                const char* pin = legacyPinForAttempt(pinAttempt_++);
                note("[bt] speaker asked for a PIN (legacy pairing): trying %s", pin);
                esp_bt_pin_code_t code = {0};
                memcpy(code, pin, 4);
                esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, code);
                return;
            }
            case ESP_BT_GAP_CFM_REQ_EVT:
                note("[bt] pairing: confirming (secure simple pairing)");
                break;  // the library confirms
            case ESP_BT_GAP_SET_PAGE_TO_EVT:
                note("[bt] page timeout set: %s", statusText(param->set_page_timeout.stat, tmp, sizeof tmp));
                break;
            case ESP_BT_GAP_DISC_RES_EVT:
                if (scanning_) { onScanResult(param->disc_res); return; }
                break;
            case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
                if (scanning_) {
                    if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
                        scanning_ = false;
                        lastRssi.store(scanRssi_);
                        if (scanRssi_ > -127)
                            note("[scan] done: speaker VISIBLE rssi %d (in pairing mode); %d other devices",
                                 scanRssi_, others_);
                        else
                            note("[scan] done: speaker not visible (normal unless it is in pairing mode); "
                                 "%d other devices",
                                 others_);
                    }
                    return;
                }
                break;
            default:
                break;
        }
        BluetoothA2DPSource::app_gap_callback(event, param);
    }

private:
    // A "key missing" failure means the speaker forgot us but we still hold
    // the old key, so every attempt fails the same way. Forget it here; the
    // next attempt does a fresh pairing (the speaker must accept new pairings
    // then, e.g. in pairing mode). Only for this specific failure: deleting a
    // good key after a transient error would break a working setup.
    void forgetStaleKey(const uint8_t* bda) {
        if (!isBonded(bda)) return;
        staleKeys.fetch_add(1);
        esp_bt_gap_remove_bond_device(const_cast<uint8_t*>(bda));
        note("[bt] deleted our stale pairing key for the speaker; next attempt re-pairs");
    }

    // Short inquiry (6.4 s): is the speaker discoverable, and how strong?
    void startScan() {
        if (scanning_) return;
        scanning_ = true;
        scanRssi_ = -127;
        others_ = 0;
        edifiers_ = 0;
        if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 5, 0) != ESP_OK) scanning_ = false;
    }

    void onScanResult(const esp_bt_gap_cb_param_t::disc_res_param& r) {
        int rssi = -127;
        char name[ESP_BT_GAP_MAX_BDNAME_LEN + 1] = "";
        for (int i = 0; i < r.num_prop; ++i) {
            const esp_bt_gap_dev_prop_t& p = r.prop[i];
            if (p.type == ESP_BT_GAP_DEV_PROP_RSSI) rssi = *static_cast<int8_t*>(p.val);
            if (p.type == ESP_BT_GAP_DEV_PROP_BDNAME) {
                const int n = min<int>(p.len, ESP_BT_GAP_MAX_BDNAME_LEN);
                memcpy(name, p.val, n);
                name[n] = 0;
            }
            if (p.type == ESP_BT_GAP_DEV_PROP_EIR && !name[0]) {
                uint8_t len = 0;
                uint8_t* d = esp_bt_gap_resolve_eir_data(static_cast<uint8_t*>(p.val),
                                                         ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
                if (!d) d = esp_bt_gap_resolve_eir_data(static_cast<uint8_t*>(p.val),
                                                        ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
                if (d) {
                    const int n = min<int>(len, ESP_BT_GAP_MAX_BDNAME_LEN);
                    memcpy(name, d, n);
                    name[n] = 0;
                }
            }
        }
        if (isSink(r.bda)) {
            if (scanRssi_ == -127) note("[scan] speaker seen: \"%s\" rssi %d", name, rssi);
            scanRssi_ = rssi;
            return;
        }
        ++others_;
        // Another Edifier nearby (e.g. a different address than configured)?
        if (strcasestr(name, "edifier") && edifiers_ < 3) {
            ++edifiers_;
            note("[scan] other Edifier: \"%s\" %02x:%02x:%02x:%02x:%02x:%02x rssi %d", name, r.bda[0],
                 r.bda[1], r.bda[2], r.bda[3], r.bda[4], r.bda[5], rssi);
        }
    }

    uint32_t streak_ = 0;
    int pinAttempt_ = 0;
    volatile bool scanning_ = false;
    int scanRssi_ = -127, others_ = 0, edifiers_ = 0;
};

// --- shared state ----------------------------------------------------------
Preferences prefs;
NoiseA2DP a2dp;

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
    const bool match = isSink(addr);
    if (match) note("[scan] found target \"%s\" rssi=%d, connecting", name ? name : "", rssi);
    return match;
}

void onConnectionState(esp_a2d_connection_state_t state, void*) {
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        linkUp.store(true);
        connects.fetch_add(1);
        note("[a2dp] connected (connect #%u)", connects.load());
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        // Failed attempts also land here; only a real drop is an event.
        if (linkUp.exchange(false)) {
            disconnects.fetch_add(1);
            note("[a2dp] disconnected (will keep retrying)");
        }
    }
}

void onAudioState(esp_a2d_audio_state_t state, void*) {
    note("[a2dp] audio %s", state == ESP_A2D_AUDIO_STATE_STARTED ? "streaming" : "stopped");
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
        note("[bt] speaker config changed; clearing remembered speaker");
        a2dp.clean_last_connection();
        prefs.putString("sinkcfg", configured);
    }
    a2dp.set_local_name("bedroom-noise");
    a2dp.set_ssid_callback(onDeviceFound);
    a2dp.set_on_connection_state_changed(onConnectionState);
    a2dp.set_on_audio_state_changed(onAudioState);
    // Effectively infinite retries: the library resets the counter on every
    // successful connect, and never falls back to a scan that might miss a
    // speaker that is not discoverable after power-on.
    a2dp.set_auto_reconnect(sinkAddr, 1000000);
    a2dp.set_data_callback_in_frames(fillFrames);
    a2dp.start();
    esp_bt_gap_set_page_timeout(kPageTimeoutSlots);
    note("[bt] started; bonded devices: %d; speaker %s", esp_bt_gap_get_bond_device_num(),
         isBonded(sinkAddr) ? "already paired" : "NOT paired yet (needs it to accept a new pairing)");
}

// --- time / Wi-Fi ------------------------------------------------------------

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
            note("[wifi] connected, ip %s, rssi %d", WiFi.localIP().toString().c_str(), WiFi.RSSI());
            Serial.printf("[wifi] ssid %s\n", ssid);
            return true;
        }
        WiFi.disconnect(true);
    }
    note("[wifi] could not connect");
    return false;
}

void wifiOff() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    note("[wifi] off");
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
        note("[time] synced: %s", buf);
        return true;
    }
    note("[time] NTP sync failed");
    return false;
}

// Restart quietly to resync the clock (Wi-Fi and Bluetooth must not share
// the heap). Only called while nothing is playing. Settings are saved first.
void restartForResync(const char* why) {
    note("[time] %s: quiet restart to resync the clock", why);
    prefs.putInt("tone2", wantTone.load());
    prefs.putInt("volume", wantVolume.load());
    delay(200);
    ESP.restart();
}

// Restart straight into the OTA window (any time of day). The request is a
// one-shot NVS flag read and cleared at boot.
void restartIntoOtaWindow(const char* why) {
    note("[ota] %s: restarting into the update window", why);
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
    note("[settings] tone %d (%s), volume %d%%", wantTone.load(), toneName(wantTone.load()),
         wantVolume.load());
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
    note("[ota] running image marked valid");
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
    WiFiServer logServer(kLogPort);
    logServer.begin();
    note("[ota] window open for 10 min (KEY1 ends it and starts noise); log on tcp port %u", kLogPort);
    Serial.printf("[ota] address %s\n", WiFi.localIP().toString().c_str());
    const uint32_t t0 = millis();
    uint32_t lastNote = t0;
    while (millis() - t0 < kOtaWindowMs) {
        ArduinoOTA.handle();
        WiFiClient client = logServer.accept();
        if (client) {
            dumpLog(client);
            client.stop();
            Serial.println("[ota] log sent over tcp");
        }
        const int key = pollKeys();
        if (key == 1) {
            note("[ota] KEY1: closing window, starting noise");
            ArduinoOTA.end();
            logServer.end();
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
    logServer.end();
    note("[ota] window closed");
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
    char tmp[24];
    Serial.printf("[status] bt attempts %u: no-answer %u, refused %u, stale keys %u | last: %s | "
                  "last scan rssi %d\n",
                  a2dp.attempts.load(), a2dp.noAnswer.load(), a2dp.refused.load(), a2dp.staleKeys.load(),
                  statusText(a2dp.lastStatus.load(), tmp, sizeof tmp), a2dp.lastRssi.load());
}

}  // namespace

// Tell the Arduino core we verify OTA images ourselves (mark valid after 60 s).
extern "C" bool verifyRollbackLater() { return true; }

void setup() {
    pinMode(kPinPaEnable, OUTPUT);
    digitalWrite(kPinPaEnable, LOW);  // analog amp off: Bluetooth-only build
    Serial.begin(115200);
    delay(300);
    Serial.println();
    setenv("TZ", kPacificTz, 1);  // before the first note(): the clock survives a restart
    tzset();
    const bool kept = logRingAttach(rtcLog);
    note("==== boot v%s (built %s %s), reset reason %d, log %s", FIRMWARE_VERSION, __DATE__, __TIME__,
         (int)esp_reset_reason(), kept ? "kept from before restart" : "new (power-on/RST)");
    for (Key& k : keys) pinMode(k.gpio, k.gpio >= 34 ? INPUT : INPUT_PULLUP);
    if (!parseAddr(BT_SINK_ADDR, sinkAddr)) {
        Serial.println("[boot] BT_SINK_ADDR malformed - halting");
        while (true) delay(1000);
    }
    loadSettings();
    engine = ToneEngine(wantTone.load(), esp_random());
    gain.setVolume(wantVolume.load());

    // 1. Clock (and maybe the OTA window) before Bluetooth: no radio sharing.
    const bool wifi = wifiConnect(12000);
    if (wifi) syncClock(10000);
    timeKnown = clockValid();

    int minute = 0, hour = 0, yday = 0;
    const bool night = timeKnown ? (localMinuteOfDay(minute, hour, yday), inWindow(kNight, minute))
                                 : Controller::kAssumeNightUntilClockKnown;
    if (!timeKnown) note("[boot] clock unknown: assuming night (fail toward noise)");

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
    else if (wifi && night) note("[boot] night window: no OTA window, noise first");
    else if (wifi && softRestart) note("[boot] software restart: no OTA window");
    else if (otaRequested) note("[ota] window requested but Wi-Fi failed");
    if (wifi) wifiOff();

    // 3. Controller, then Bluetooth.
    controller = new Controller(night, millis());
    // KEY1 closing the window means "noise now": only press START if the
    // schedule isn't already playing (at night it would otherwise STOP).
    if (startPressed && !controller->shouldPlay()) {
        controller->pressStartStop(millis());
        key1LockedUntil = millis() + kKey1LockoutMs;
        note("[key] start/stop -> daytime run (2 h); KEY1 locked until sound starts");
    } else if (startPressed) {
        key1LockedUntil = millis() + kKey1LockoutMs;
        note("[key] window closed; schedule already playing");
    }
    wantPlaying.store(controller->shouldPlay());
    note("[boot] state: %s | heap before Bluetooth %u KB", controller->stateName(), ESP.getFreeHeap() >> 10);
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
        note("[key] start/stop ignored: still connecting to the speaker");
    } else if (key == 1) {
        controller->pressStartStop(now);
        note("[key] start/stop -> %s", controller->stateName());
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
                note("[schedule] window %s -> %s", controller->inWindow() ? "start" : "end", controller->stateName());
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
            note("[audio] %s", play ? "fade in" : "fade out");
        }
    }

    if (Serial.available()) {
        const int c = Serial.read();
        if (c == 'r') printStatus();
        if (c == 's' && !controller->shouldPlay()) restartForResync("serial 's'");
        if (c == 'o') restartIntoOtaWindow("serial 'o'");
        if (c == 'l') {
            Serial.println("---- log ----");
            dumpLog(Serial);
            Serial.println("---- end ----");
        }
    }

    saveSettingsIfDue();

    markValidIfDue();

    if (now - lastStatus >= 60000) {
        lastStatus = now;
        printStatus();
    }
    delay(5);
}
