# Design decisions

Each entry gives the decision, why it was made, and what would make us revisit
it. Dates are when the decision was recorded. "Open" means a spike or the user
still has to settle it.

## D1 — Firmware framework: PlatformIO + Arduino core 3.x (pioarduino), not ESPHome

Recorded 2026-11-13.

**Decision.** Write the firmware in C++ with the Arduino core for ESP32,
version 3.3.12 (ESP-IDF 5.5 underneath). Build it with PlatformIO, using the
community `pioarduino` platform fork, pinned to an exact release. This is the
same shape as `~/dev/hot-tub-controller/esp32`, including native (host-side)
Unity tests for pure logic.

**Why not ESPHome**, which the two devices in `~/dev/home-assistant/esphome/`
use:

- The two hard parts of this project are custom C++ either way. ESPHome has
  no Bluetooth A2DP *source* component. The only ESPHome A2DP component
  (`PxPert/a2dp_sink`) is a sink: it receives audio from a phone. Its author
  says a source is "a totally different implementation". ESPHome also has no
  noise generator. So we would be writing two external components and fighting
  ESPHome's build to enable Bluetooth Classic, while paying its memory
  overhead on a chip that already has to fit Wi-Fi and Bluetooth Classic.
- What ESPHome would give us for free is Wi-Fi, OTA, buttons and the
  Home Assistant API. The Arduino core gives us Wi-Fi, OTA and buttons almost
  as cheaply. MQTT discovery (D4) gives Home Assistant entities with no
  hand-written HA config.
- Pure logic, such as the noise DSP, the schedule and the button state machine,
  can be unit-tested on the host with Unity, exactly like the hot-tub
  firmware. ESPHome lambdas cannot be tested that way. The HA repo works around
  this by prototyping in Python and transcribing by hand.

**Why not raw ESP-IDF.** We would only need it if we have to change Bluetooth,
Wi-Fi or memory settings that the prebuilt Arduino libraries fix in place. The
prebuilt core already has what we need (checked 2026-11-13 in its `sdkconfig`):

- `CONFIG_BT_CLASSIC_ENABLED=y` and `CONFIG_BT_A2DP_ENABLE=y`.
- `CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y` (Wi-Fi/Bluetooth time-sharing).
- `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` (a bad OTA image rolls back).
- `CONFIG_SPIRAM=y` (PSRAM support).

**Revisit if** the Phase 2 coexistence soak fails in ways that need sdkconfig
tuning (for example Wi-Fi buffer counts or Bluetooth task placement). The
escape hatch is pioarduino's hybrid `framework = arduino, espidf` mode. It
rebuilds the core from source with our own `sdkconfig`, so the code stays the
same.

## D2 — Audio output path: Bluetooth A2DP source first, behind a go/no-go spike — OPEN

Recorded 2026-11-13.

**Goal.** Replace the Pixel 3 without touching the bedroom speaker. Today the
Pixel streams a brown-noise MP3 over Bluetooth to that speaker from about 21:00
to 09:00 (`~/dev/home-assistant` memory `project_pixel3_brown_noise`).

**Capability.** The original ESP32 can be a Bluetooth A2DP *source*, meaning it
sends audio to a speaker. The Apache-2.0 library `pschatzmann/ESP32-A2DP`
(`BluetoothA2DPSource`, v1.8.12, 2026-10-31) does this. We hand it a callback
that fills 44.1 kHz stereo frames. Bluedroid, Espressif's Bluetooth stack,
SBC-encodes them and sends them to the speaker.

**The risk: Wi-Fi and Bluetooth share one 2.4 GHz radio.** Sources disagree:

- Espressif's coexistence table (ESP-IDF docs, "RF Coexistence") marks Wi-Fi
  station mode, connected, alongside a connected Bluetooth Classic link as "Y":
  supported and stable.
- The ESP32-A2DP author's wiki says flatly that the ESP32 "only supports either
  Bluetooth or WIFI, but not both at the same time", and asks people not to
  file issues about it.
- The ESPHome a2dp_sink author reports Wi-Fi staying connected for Home
  Assistant control during A2DP, but not handling heavy traffic at the same
  time.

Our case is the easy one. The audio is generated on the device, so Wi-Fi
carries only a few small MQTT messages a minute, plus a rare OTA (and audio
stops during OTA). Even so, we prove it before building on it.

**Spike (Phase 2, go/no-go).** Stream synthesized noise to the real speaker
while connected to Wi-Fi and publishing MQTT heartbeats. Run it overnight for
8 hours or more. Pass means:

- no audible dropouts (the firmware counts buffer underruns);
- no A2DP disconnects that do not recover by themselves within 30 seconds;
- Wi-Fi/MQTT stays reachable (Home Assistant sees the heartbeat);
- free heap stays flat.

**Fallbacks if it fails,** in order of preference:

1. **Analog line out into the speaker's AUX input**, if it has one. No radio
   contention at all, and no pairing. This is the simplest and most reliable
   option.
2. **Analog line out into an off-the-shelf Bluetooth transmitter dongle**
   (3.5 mm in, A2DP out, USB-powered, around $20). This keeps the existing
   speaker wireless and takes Bluetooth off the ESP32 entirely.
3. Bluetooth only, no Wi-Fi: schedule and buttons on the device, no Home
   Assistant control. This fails a stated goal, so it is a last resort.

The firmware keeps the output behind one interface (`AudioSink`: either the
`I2sCodecSink` or the `A2dpSink`), so switching is a config change and not a
rewrite. The analog path gets built in Phase 1 regardless, because it is the
simplest way to hear the generator.

**The speaker** (user, 2026-11-13): an **Edifier R2000DB** powered bookshelf
pair. It has Bluetooth and analog inputs, so fallback 1 is available. Its
analog inputs may be RCA rather than 3.5 mm; check before buying a cable. The
user prefers Bluetooth. **It stays on as long as a Bluetooth link from the
source is active**, and the Pixel 3 link has given no connectivity trouble.
Consequences:

- When noise is "off", the ESP32 should keep the A2DP link connected rather
  than drop it. Dropping it risks the speaker going into standby, and it may
  not come back for a Bluetooth reconnect without a button press.
- Phase 2 must establish whether a connected but *idle* link (the A2DP stream
  suspended) keeps the speaker awake, or whether it needs a continuous stream
  of silence. Streaming silence costs radio time all day, so prefer the idle
  link if it works.
- Pairing: the Pixel 3 must be unpaired or have Bluetooth off first. Otherwise
  the speaker may reconnect to it instead.

## D3 — Sound source: synthesize on the device; SD-card files are an optional later phase

Recorded 2026-11-13.

**Decision.** Generate noise in software. Brown (also called red), pink and
white noise are a pseudo-random number generator followed by a small filter:

- **White**: a fast PRNG (xorshift32 or PCG32), scaled to the sample range.
- **Brown**: white noise run through a *leaky integrator*:
  `b = (b + k·w) / (1 + k)`, with k ≈ 0.02, plus gain makeup. The leak keeps
  the output from drifting off to DC. Spectrum falls 6 dB per octave.
- **Pink**: Paul Kellet's refined filter (a sum of seven one-pole filters),
  or the Voss-McCartney algorithm. Spectrum falls 3 dB per octave.
- **Variants** are parameter changes on those: "deep brown" (an extra low-pass
  stage), brightness, and stereo width (independent generators per channel).

**Why synthesize.**

- **Power and CPU are negligible.** About 20 floating-point operations per
  sample, at 44,100 samples per second times two channels, is a fraction of
  1% of one 240 MHz core. The ESP32 has a hardware single-precision FPU.
  Decoding MP3 costs roughly 10–20% of a core plus SD-card reads. Both numbers
  are small next to the Bluetooth radio, which dominates the board's power
  draw. The whole board runs at around 0.5–1 W either way, and it is
  mains-powered. Power efficiency is not a deciding factor. Simplicity is.
- **No loop seam.** A file has to loop. A generator never repeats.
- **No files to manage**, and it works with no SD card.
- **Tunable from Home Assistant** (colour, depth, width) without re-rendering
  files.

**SD-card files (Phase 5, optional)** cover sounds that are not simple noise
colours, such as rain, a fan or surf, and let the user's own long MP3s play.
Preferred formats:

- **WAV/PCM**: zero decode cost, and the 64 GB card has room for it.
- **MP3**, decoded with **minimp3** (public domain, CC0). We use minimp3 rather
  than libhelix because pschatzmann's libhelix wrapper is GPL-3.0 (D5).

Files loop with a short crossfade so the seam is inaudible.

**Calibration idea.** If the user shares the Pixel's current brown-noise MP3,
we can measure its spectrum and tune the synth's "brown" preset to match. The
first night on the new device would then sound like the old one.

## D4 — Home Assistant integration — DEFERRED, to discuss after Phase 0

**Status (2026-11-13): postponed by the user.** Their view is that their
requirements call for neither a message broker nor a web API. They think the
MQTT proposal below came from my reading of the requirements, not from the
requirements themselves. Revisit together once there is working
communication with the board, starting from what "control from Home
Assistant" has to cover. Concretely: which of the schedule, on/off, nap,
noise type and volume must HA be able to see or change, and what can live on
the device alone? The text below is the original proposal, kept for that
discussion. It is not a decision.

### Original proposal (2026-11-13): MQTT with HA discovery

Recorded 2026-11-13.

**Decision.** The device connects to an MQTT broker and publishes Home
Assistant *discovery* messages. Those are retained JSON configs, and HA turns
them into entities on its own: no YAML in the HA repo for the entities
themselves. A last-will message marks the device unavailable when it drops
off.

**The catch:** `~/dev/home-assistant` runs no MQTT broker today.
`docs/plan.md` there already anticipates adding Mosquitto as one more Docker
image. Adding it means a new service in that repo's `docker-compose.yml`, a
password file, and the HA MQTT integration. That work happens **in the HA repo,
under its own deploy discipline** (`production` branch, t480 pull), not from
here. It needs the user's go-ahead (QUESTIONS.md).

**Alternatives considered.**

- **ESPHome native API**: only available to ESPHome firmware (see D1).
- **HTTP REST on the device**, with HA `rest` sensors (polling) and
  `rest_command` (control). This needs no broker, but HA has to poll, and every
  entity is hand-written YAML. Workable as a fallback.
- **HA webhooks pushed from the device**: one-way only, still needs
  hand-written entities.

**Planned entities** (device name `bedroom-noise`):

- switch "Noise" (manual on/off; overrides the schedule until its next
  boundary);
- select "Noise type";
- number "Volume" (0–100, capped by a firmware ceiling);
- button "Nap" and number "Nap minutes";
- switch "Schedule enabled";
- text "Schedule start" / "Schedule end" (HH:MM);
- sensors: playback state, output (Bluetooth or analog), Bluetooth connected,
  Wi-Fi RSSI, uptime, free heap, firmware version, SD card status.

## D5 — Licensing: Apache-2.0, permissive dependencies only

Recorded 2026-11-13.

The repo is public under Apache-2.0. Shipping firmware links only permissively
licensed code:

- ESP32-A2DP — Apache-2.0.
- ESP-IDF — Apache-2.0.
- Arduino core for ESP32 (`espressif/arduino-esp32`) — LGPL-2.1. Its terms
  only bite when a binary is *distributed*. They are then met by the source
  being available, and this repo is public. Every Arduino-ESP32 project lives
  with this. It does not affect our Apache-2.0 licence.
- ArduinoJson — MIT.
- 256dpi/arduino-mqtt — MIT.
- minimp3 — CC0 (if Phase 5 happens).
- The ES8388 register sequence — written here from the ES8388 datasheet. We
  may consult Espressif's ESP-ADF driver, but GitHub reports ESP-ADF's licence
  as non-standard ("ESPRESSIF MIT", restricted to Espressif chips). So check
  the licence on the specific file before copying anything verbatim.

Deliberately excluded: `arduino-audio-tools`, `arduino-audio-driver`,
`arduino-libhelix` (all GPL-3.0, by pschatzmann). Our source code could still
be Apache-2.0 alongside them, but any firmware *binary* built with them would be
covered by GPL-3.0. Keeping them out avoids that question entirely. They remain
excellent references for pin maps and codec setup, as long as we don't copy
their code.

## D6 — Schedule ownership: the device owns the schedule; Home Assistant edits and overrides it

Recorded 2026-11-13.

**Decision.** The on/off schedule (default 21:00–09:00) lives in the device's
NVS (non-volatile key-value storage on flash). It runs from the device's own
clock, synced by NTP, with a POSIX TZ string for daylight saving. Home
Assistant can change the schedule, turn noise on or off manually, and start a
nap. But HA is a remote control, not the clock.

**Why.** Night-time noise must not depend on Wi-Fi, MQTT, Home Assistant, or
t480-server. t480 reboots itself every night at 03:00 by design
(`~/dev/home-assistant/CLAUDE.md`), so an HA-driven schedule would have a
known gap in the middle of every night. The precedent in the HA repo is the
Sensibo "off-HA safeties": schedules that deliberately run on the device. If NTP
has never synced since boot, the device keeps its last state and retries.
It does not guess the time.

**Manual-override semantics** (to confirm with the user, see QUESTIONS.md):

- A manual on/off holds until the next schedule boundary, then the schedule
  resumes. This is how a thermostat "hold until next change" works.
- A nap plays for N minutes, then returns to whatever the schedule says.
- A temporary noise-type change (from a button) lasts until the next scheduled
  start. HA's select sets the default.

## D7 — Toolchain pinning: repo-local venv + exact platform/library versions

Recorded 2026-11-13.

- `requirements.txt` pins `platformio==6.2.0` and `esptool==5.6.0`.
  `scripts/pio.sh` builds `.venv` from it and refuses to run if the venv
  disagrees. This is the same mechanism and the same reason as the HA repo's
  `esphome/esphome.sh`: a drifted toolchain silently changes what gets
  flashed.
- The pinned pioarduino release needs PlatformIO Core ≥ 6.2.0. The global
  `~/.platformio/penv` is 6.1.19 and serves hot-tub-controller, so we do not
  touch it.
- `PLATFORMIO_CORE_DIR` points at `<repo>/.platformio-core`. This repo's
  platforms and toolchains are therefore kept separate from the global ones.
- `platformio.ini` pins the platform by exact release zip URL, and libraries
  by exact version.

## D8 — Safety: it lives in a bedroom

Recorded 2026-11-13.

The device makes noise next to sleeping people (and pets, per the
HA repo). So:

- **Boot silent.** The amp enable stays LOW, and Bluetooth output starts at
  volume 0. Playback always fades in (default 20 s) and fades out (default 5 s).
- **Volume ceiling.** A compile-time maximum, plus an NVS-stored maximum that HA
  can lower but not raise above the compile-time one.
- **Watchdog-safe audio.** If the audio task stalls, output goes silent. It
  does not loop a stale buffer, which would produce a loud buzz.
- **No light at night.** The user LED stays off except during bench bring-up.
  This follows the bedside Pocuter rule in the HA repo: nothing lights up on its
  own at night.
- **Flashing, OTA and audio tests are physical actuation in someone's
  bedroom.** Each one needs explicit authorization for that specific action,
  with the user aware of it. Do not do them during sleep hours unless asked.
  This extends the HA repo's `feedback_live_hardware_supervision` rule.
