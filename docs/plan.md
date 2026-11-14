# Implementation plan — bedroom noise generator

**Goal:** retire the Pixel 3 that plays brown noise to the bedroom Bluetooth
speaker (about 21:00–09:00), and the Kasa "Tablet charger" plug that exists
only to cycle its battery. The replacement is an ESP32-Audio-Kit that:

- generates several kinds of noise;
- runs its own schedule;
- has physical buttons (noise type, nap timer, volume);
- is controlled from Home Assistant on t480-server;
- updates over the air.

Design choices and their reasons are in `docs/decisions.md` (D1–D8). This file
is the order of work. Board facts are in `docs/hardware.md`. Open questions for
the user are in `QUESTIONS.md`.

Every phase follows the repo's red/green rule. Pure logic is written test-first
in `firmware/lib/` and runs on the host (`./scripts/test.sh native`). Hardware
glue is kept thin and verified on the device. Each phase ends with a commit on
`main`, and with a git tag when firmware was flashed.

## Architecture (target)

```
                     ┌────────────── ESP32-A1S ──────────────────────────────┐
 buttons ─► Buttons ─┤                                                       │
 (KEY1..6)  (debounce,│  Controller  ◄── Schedule (NVS, NTP, POSIX TZ)        │
            long-press)  (state machine:  ◄── HaBridge (MQTT discovery) ◄─────┼── Wi-Fi ── Mosquitto ── HA (t480)
                     │   off / scheduled / manual / nap / temp-type)          │
                     │        │                                              │
                     │        ▼                                              │
                     │   Source: NoiseSynth (white/pink/brown/…)             │
                     │           or SdPlayer (WAV / minimp3)  [Phase 5]      │
                     │        │   fade + volume + ceiling (Gain)             │
                     │        ▼                                              │
                     │   AudioSink:  A2dpSink (Bluetooth → bedroom speaker)  │
                     │           or  I2sCodecSink (ES8388 → 3.5 mm jack)     │
                     └───────────────────────────────────────────────────────┘
```

Pure, host-testable modules (in `firmware/lib/`): `NoiseSynth`, `Gain`
(fades, volume curve, ceiling), `Schedule` (is-it-on, next boundary, DST via
injected time), `Controller` (the state machine), `Buttons` (debounce and
press classification from raw samples), `HaDiscovery` (builds the discovery
JSON). Hardware glue (in `firmware/src/app/`): the I2S/ES8388 driver, the A2DP
wrapper, Wi-Fi/MQTT/OTA, GPIO and NVS.

## Known constraint: flash budget

Measured 2026-11-13 with the `libcheck` env (compile only). Bluetooth A2DP
source + Wi-Fi + MQTT + ArduinoJson come to **1.67 MB, 85% of one 1.9 MB OTA
slot** in `min_spiffs.csv`. That leaves about 290 KB for our own code.
Turning core logging off (`CORE_DEBUG_LEVEL=0`) saves only about 17 KB,
because the bulk is the precompiled Bluedroid and Wi-Fi stacks. Our own code
(synth, controller, MQTT glue) should be well under 100 KB, so this fits. But
watch the `Flash:` line that `./scripts/test.sh` prints. Ways to get more room,
cheapest first:

1. **Check the real flash size in Phase 0.** If the module has 8 MB, use a
   partition table with two 3 MB+ slots and the problem disappears.
2. A custom 4 MB partition table with no SPIFFS: two app slots of about
   2.0 MB each (+98 KB). Nothing in the plan needs SPIFFS, because the SD card
   holds the files.
3. pioarduino's hybrid `framework = arduino, espidf` mode with our own
   `sdkconfig`. It drops BLE (we use only Bluetooth Classic) and other unused
   components. This is a larger change (decision D1, "revisit if").

## Phase 0 — Bring-up and identification (USB, silent)

Runbook: `docs/runbooks/bring-up.md`.

1. Move the USB cable to the board's **UART** micro-USB port. Confirm that
   `/dev/ttyUSB0` appears.
2. Run `esptool chip-id` and `esptool flash-id`. Record the chip revision,
   flash size and MAC in `docs/hardware.md`.
3. **Back up the stock firmware** (full 4 MB read) into `backup/`, before
   anything is written to the board.
4. Flash the `probe` env. Record:
   - board variant (result 2026-11-13: ES8388 **v1**, against the schematic's v2);
   - PSRAM size;
   - which keys produce events (press each one in turn);
   - headphone-detect behaviour;
   - DIP switch positions.
5. ~~SD card~~ — skipped. The SD card is optional (`docs/hardware.md`), and
   the card reader on bambam never enumerated. Revisit only if Phase 5
   happens.

**Done when** `docs/hardware.md` has no unverified item that later phases
depend on, and the stock image is safely in `backup/`.

## Phase 1 — Noise you can hear: synth → ES8388 → headphone jack (no network)

1. **`NoiseSynth`** (TDD, native). White/pink/brown generators with a seedable
   PRNG. Tests:
   - deterministic output for a fixed seed;
   - output stays within ±1.0 (no clipping) over 10 minutes of samples;
   - no DC drift (the mean over 60 s stays close to 0);
   - **spectral slope**: band-energy ratios across octaves match −6 dB/octave
     for brown and −3 dB/octave for pink, within tolerance. This uses a small
     host-side FFT or octave filter bank in the test, not on the device.
2. **`Gain`** (TDD). Volume 0–100 mapped onto a perceptual (dB) curve, the
   ceiling clamp, linear fade-in/out over N samples, mute. Tests check fade
   endpoints, monotonic ramps, and that the ceiling is never exceeded even
   mid-fade.
3. **ES8388 + I2S driver** (on-device). Configure the codec over I2C (v1 pins,
   confirmed: SDA 33, SCL 32). Enable DAC → OUT1 and OUT2. Run I2S at
   44.1 kHz, 16-bit stereo (v1: MCLK 0, BCLK 27, WS 25, DOUT 26). Leave the speaker amp (GPIO21) off;
   only the headphone jack is used. Listen with headphones at low volume, with
   the user's go-ahead (D8).
4. A minimal local UI on the safe keys: play/stop, next noise type, volume
   up/down.

**Done when** brown/pink/white play cleanly from the jack, and the volume keys
and play/stop work. A 1-hour run shows no underruns (the audio task counts
them and logs the count).

## Phase 2 — Bluetooth A2DP source + the Wi-Fi coexistence go/no-go

**Progress 2026-11-13:** the `bttest` env streams `NoiseSynth` → `Gain` to a
desk test speaker ("TP-Link_Music", address in `firmware/.env`). Results:

- connected and authenticated about 2 s after the reconnect attempt;
- media started about 10 s after boot;
- steady 44,049 frames/s delivered (the 44.1 kHz rate);
- free heap flat at 123 KB, minimum 115 KB.

That was Bluetooth only, with no Wi-Fi yet. Not done yet: a listening check,
the coexistence soak, and the R2000DB.


1. `A2dpSink` built on ESP32-A2DP `BluetoothA2DPSource`. The data callback pulls
   from the same `Source → Gain` chain. It connects to the speaker by name or
   MAC (stored in NVS) and reconnects automatically.
2. Pairing and handover. Disconnect the speaker from the Pixel 3 first: turn
   off the Pixel's Bluetooth, or unpair it. A speaker usually accepts only one
   source and will re-grab the phone otherwise.
3. Characterise the speaker. Does it auto-off when it gets silence or no link?
   Does the ESP32 reconnect after the speaker power-cycles? How long does
   reconnect take?
4. **Coexistence soak (the go/no-go in D2).** Add Wi-Fi STA plus an MQTT
   heartbeat every 30 s (a throwaway broker is fine for the spike, e.g.
   Mosquitto in a container on bambam). Stream for one full night. Log
   underruns, A2DP disconnects, Wi-Fi drops and the free-heap minimum, both to
   serial and in the heartbeat payload.
5. Decide. If it passes, Bluetooth is the primary output. If it fails, pick a
   fallback from D2, record it in `docs/decisions.md`, and continue. Phases 3–6
   do not change shape either way.

**Done when** the D2 decision is closed with soak numbers recorded in
`docs/decisions.md`.

## Discussion before Phase 3: how Home Assistant talks to the device

Decision D4 is deferred (user, 2026-11-13). The user does not think a broker
or a web API is needed. Settle this together once the board is
communicating, before starting Phase 3. Steps 5 and 6 below assume MQTT and
will change with that discussion. Wi-Fi, NTP and OTA (steps 1–4) are needed
either way: OTA updates are a stated goal.

## Phase 3 — Network: Wi-Fi, NTP, OTA, Home Assistant link

1. **Secrets at build time** via `firmware/.env` and a `load_env.py` extra
   script, the same pattern as hot-tub. Keys: Wi-Fi SSIDs (the IoT SSID
   preferred, the main SSID as fallback, like the ESPHome devices), Wi-Fi password,
   MQTT host/user/password, OTA password. `.env.example` is committed.
2. **Wi-Fi**: try the IoT SSID first, fall back to the main SSID, and reconnect with
   backoff. Ask for a DHCP reservation in `~/dev/network/docs/inventory.md`
   (via the network repo, not here). The hostname is `bedroom-noise`.
3. **NTP** with a POSIX TZ string. The device's time is only trusted after the
   first sync (see D6).
4. **OTA**: ArduinoOTA (espota) with a password, pushed from a dev box through
   `./scripts/pio.sh run -e app_ota -t upload`. On OTA start: fade out, stop
   Bluetooth/I2S, free memory. After boot: run a self-test (audio task alive,
   Wi-Fi up), then call `esp_ota_mark_app_valid_cancel_rollback()`. An image
   that crashes or fails its self-test rolls back to the previous one, because
   the bootloader has rollback enabled (D1).
5. *(If MQTT is chosen.)* **Mosquitto in the HA stack** — cross-repo work in `~/dev/home-assistant`,
   needs the user's go-ahead (D4). Add a `mosquitto` service to
   `docker-compose.yml` with a password file and the HA MQTT integration, then
   deploy through that repo's `production` flow.
6. *(If MQTT is chosen.)* **`HaDiscovery`** (TDD, native). Builds the discovery config topics and JSON
   for the entities listed in D4. Tests compare against golden JSON fixtures:
   unique_ids are stable, `availability_topic` and device block are present.
   **`HaBridge`** (glue) publishes them retained on every MQTT connect, handles
   commands, publishes state, and sets a last-will of `offline`.

**Done when** HA shows the `bedroom-noise` device with working controls, an OTA
update succeeds and is marked valid, and a deliberately bad image is shown to
roll back (test this once, on the bench).

## Phase 4 — Schedule, controller state machine, buttons

1. **`Schedule`** (TDD). Answers "should it be on at local time T?" for windows
   that cross midnight (21:00–09:00), and "when is the next boundary?". It
   takes an injected clock. Tests cover both DST transitions in the user's
   time zone, plus empty/disabled schedules. Optional later: per-weekday
   windows.
2. **`Controller`** (TDD). The state machine over: scheduled-on, scheduled-off,
   manual-on, manual-off (both held until the next boundary), nap (N minutes,
   then back to schedule), and temporary noise type (until the next scheduled
   start). Inputs are events (button, HA command, schedule boundary, timer).
   Outputs are the target source, volume and on/off. Every transition gets a
   test, including the overlaps: a nap during scheduled-on time, a manual off
   during a nap, the schedule edited while a manual hold is active.
3. **`Buttons`** (TDD). Debounce and short/long/double-press classification
   from a stream of (time, level) samples. Proposed mapping, to finalise after
   Phase 0 shows which keys are usable and the user picks (QUESTIONS.md):
   - Play — toggle noise. While scheduled-off, start a nap (default 2 h).
   - Mode — next noise type (temporary). Long press: back to the default type.
   - Vol− / Vol+ — 5% steps, never above the ceiling.
   - Set (long press) — announce state. For example: speaker
     connected/disconnected shown by a single dim LED blink, daytime only.
4. NVS persistence for volume, default type, schedule, ceiling and speaker
   address. Writes are debounced so flash is not worn out by repeated
   volume-key presses.

5. **Setup idea (user, 2026-11-13): a "pair" button.** On a long press,
   scan for nearby Bluetooth audio devices, pick the strongest one, connect,
   and store its address in NVS as the new speaker. Notes for the design:
   - Scanning is explicit. The `bttest` firmware showed that a stored address
     reconnects directly without any scan, so the scan logging never ran.
   - Filter on the Bluetooth "class of device", so that only audio renderers
     (speakers, headphones) count. Otherwise a phone or laptop with a stronger
     signal would win.
   - Require a minimum signal strength, so a neighbour's speaker is not
     grabbed.
   - Confirm with a short, quiet chime on the new speaker. A second long
     press within 10 s reverts to the previous address.
   - ESP32-A2DP already supports this: `set_ssid_callback` is called once per
     device found, with its name, address and signal strength (RSSI). Return
     true to connect.

**Done when** the device runs a full schedule cycle unattended, buttons and HA
agree with each other (state is always echoed to HA), and a reboot mid-night
resumes correctly.

## Phase 5 — SD-card sound library (optional)

1. Index `/sounds/*.wav|*.mp3` at boot. Offer the files in the HA "Noise type"
   select after the synth presets.
2. `SdPlayer`: streaming WAV reader; MP3 via minimp3 (CC0); gapless loop with a
   crossfade of about 2 s. Read-ahead buffer in PSRAM.
3. Native tests for the WAV header parser, the loop crossfade maths, and
   file-list ordering.

**Done when** a user-supplied 1-hour MP3 loops all night without an audible seam
or underrun.

## Phase 6 — Cutover and Pixel 3 retirement

1. Run the ESP32 for real nights. The Pixel 3 is off or unpaired, but kept as a
   fallback for a week.
2. Retire, in the HA repo and with the user's go-ahead:
   - the Pixel 3's Tasker profiles;
   - the `pixel3-charger` iot-api slug and `iot_map_pixel3_charger` mapping;
   - `switch.tablet_charger` automations.
   The Kasa plug is then free for other use.
3. Update the HA repo memory `project_pixel3_brown_noise`. That project owns its
   memory, so make the change from that repo's session or tell the user.

**Done when** a week of nights has passed with no fallback to the Pixel.

## Testing and quality gates (all phases)

- `./scripts/test.sh` must be green before every commit. It runs the native
  Unity tests and compiles every device env. Add each new env to
  `DEVICE_ENVS` in `scripts/test.sh`.
- Pure logic has red/green tests first. Bug fixes add a regression test.
- On-device checks are recorded in the phase's commit message: what was
  measured, and for how long.
- Every flashed build is tagged `fw-vX.Y.Z` and carries the same string in
  `FIRMWARE_VERSION`, which is published to HA. Then "what is running" is
  always answerable.
