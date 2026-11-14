# CLAUDE.md — noise-generator

Firmware for an **AI-Thinker ESP32-Audio-Kit V2.2** (ESP32-A1S module, ES8388
codec) that replaces the Pixel 3 in the bedroom. The Pixel plays brown noise
over Bluetooth to the bedroom speaker from about 21:00 to 09:00. The ESP32 will:

- generate the noise itself (several types);
- run that schedule on its own;
- take button input (type, nap, volume);
- be controlled from Home Assistant on t480-server;
- update over the air.

Public repo `science/noise-generator`, Apache-2.0.

## Read first

- **`docs/plans/implementation-plan.md`** — the plan. It opens with a summary of the concepts,
  then the design, phases (0 done → 6 cutover), and the open questions. Where
  the work is and what comes next.
- **`docs/decisions.md`** — D1–D8: why PlatformIO/Arduino and not ESPHome,
  why Bluetooth output sits behind a Wi-Fi-coexistence spike, why the noise is
  synthesized, the (deferred) HA-integration question, licensing, schedule ownership, toolchain pinning,
  bedroom safety.
- **`docs/hardware.md`** — board facts, each marked confirmed /
  from-schematic / from-library. Pin maps, the button-vs-codec pin conflicts,
  SD card, USB ports.
- **Questions waiting on the user** are at the end of the plan, each with a
  `> Your answer:` slot. Read the user's answers there before starting a phase.
- `docs/runbooks/bring-up.md`, `docs/runbooks/flash.md` — procedures.

## Status (update as phases land)

- 2026-11-13: repo initialised, research done, toolchain pinned. The `probe`
  firmware builds and its native tests pass.
- 2026-11-13: the board is on `/dev/ttyUSB0` (CP2102; the first cable was
  data-dead). ESP32-D0WD-V3 rev 3.1, 4 MB flash. The stock firmware is backed
  up and verified in `backup/`. Next: flash the probe, with the user's OK.
  The SD card is optional. The HA integration method (D4) is deferred by the
  user; do not push MQTT or a web API until it is discussed.
- 2026-11-13: probe → all six keys work, ES8388 v1, PSRAM 4 MB. `NoiseSynth`
  (white/pink/brown/deep brown) and `Gain` (volume curve, fades, ceiling) are
  written test-first (41 native tests). The `bttest` env streams them over
  A2DP to a desk test speaker, verified on-device. The desk speaker's
  address lives in `firmware/.env`.

## Toolchain — always through the wrappers

- **`./scripts/pio.sh <pio args>`** runs PlatformIO from the repo-local
  `.venv`, pinned in `requirements.txt` (platformio 6.2.0, esptool 5.6.0). It
  refuses to run if the venv has drifted from the pin. `./scripts/pio.sh
  --sync` builds or repairs the venv. `./scripts/pio.sh esptool ...` runs the
  pinned esptool. The wrapper `cd`s into `firmware/` for pio commands.
- **Never use the global `pio`** (`~/.platformio/penv`, 6.1.19). It belongs to
  hot-tub-controller, and it is too old for our pinned platform (pioarduino
  55.03.312-1 = Arduino core 3.3.12 needs Core ≥ 6.2.0). Don't upgrade it from
  here either.
- `PLATFORMIO_CORE_DIR` is `<repo>/.platformio-core` (gitignored), so this
  repo's platforms and toolchains never mix with the global ones.
- `.venv` and `.platformio-core` live on the `~/dev` bind mount. The same
  ones work on bambam and on the dev VM, because the venv is built on
  `/usr/bin/python3` (3.12.3 on both sides). Don't rebuild it against pyenv,
  and don't build from both sides at once.

## Tests — always `./scripts/test.sh`

```
./scripts/test.sh            # native unit tests + compile every device env
./scripts/test.sh native     # host-side Unity tests only
./scripts/test.sh build      # compile only
./scripts/test.sh status     # toolchain + is a board attached?
```

The script never uploads. New device envs get added to `DEVICE_ENVS` in it.

**TDD red/green is mandatory for pure logic**, as in the sibling repos
(`~/dev/hot-tub-controller/CLAUDE.md`):

1. Write the failing test.
2. Run it to prove it fails.
3. Write the minimum code to pass.
4. Refactor while green.

Pure logic lives in `firmware/lib/<Module>/` with no Arduino includes, so it
compiles on the host. Tests go in `firmware/test/test_native/test_<module>/`.
Hardware glue (I2S, codec, A2DP, Wi-Fi, MQTT, GPIO, NVS) lives in
`firmware/src/<env>/`. Keep it thin, and verify it on the device. Bug fixes add a
regression test.

## Firmware layout

- `firmware/platformio.ini` — envs `probe` (Phase 0 bring-up, silent),
  `libcheck` (compile-only link test of the shipping libraries and the flash
  budget; never flash it) and `native` (host tests). Phases add `app` and
  `app_ota`.
- **Flash budget is tight.** A2DP + Wi-Fi + MQTT alone use 85% of a 1.9 MB
  OTA slot (`docs/plans/implementation-plan.md`, "Known constraint"). Check the `Flash:` line after
  adding code or libraries. The platform and
  libraries are pinned to exact versions. Partition table is `min_spiffs.csv`
  (two ~1.9 MB OTA slots; Bluetooth Classic + Wi-Fi builds are large).
- `firmware/src/probe/` — chip/flash/PSRAM report, I2C scan of both codec bus
  candidates, SD mount test, key watch. It drives the speaker amp enable LOW
  and never touches the codec.
- `firmware/lib/BoardId/` — classifies the board variant from I2C scan
  results, and lists the usable keys.
- `firmware/lib/NoiseSynth/` — the noise generator. xorshift64 white noise,
  Kellet pink, a one-pole brown at 140 Hz, plus a 500 Hz second pole for deep
  brown. All types are normalised to 0.18 RMS. Interleaved stereo float.
- `firmware/lib/Gain/` — volume 0–100 on a 50 dB curve, fades, a ceiling
  that is never exceeded, float → int16.
- `firmware/src/bttest/` — A2DP source test: synth → gain → speaker at
  `BT_SINK_ADDR`. Keys: KEY1 play/stop, KEY3 type, KEY5/6 volume, KEY4
  status. **Never call `a2dp.set_volume()`.** It changes the speaker's own
  volume via AVRCP (D2).
- `firmware/load_env.py` + `firmware/.env` (gitignored) — site values as
  build macros. See `.env.example`.

## Hardware facts that bite

- **This board is the ES8388 "v1" pin map** (confirmed by the probe on
  2026-11-13). The vendor's schematic shows v2 and is wrong for this board.
  The codec's I2C is on SDA 33 / SCL 32, and I2S BCLK is on 27. No key
  shares a pin with the codec. KEY2 (GPIO13) is the SD card's DAT3 line, and
  the SD card is optional.
- **Only the original ESP32 can be a Bluetooth audio *source*.** S3/C3/C6 lack
  Bluetooth Classic. Don't "upgrade" the chip.
- **Wi-Fi + A2DP coexistence is unproven for us.** Espressif says it is
  supported. The ESP32-A2DP author says it isn't. Phase 2's overnight soak
  decides it (D2), and analog line-out is the fallback.
- **Two micro-USB ports:** UART (data, `/dev/ttyUSB0`) and POWER (no data).
- GPIO0, 2 and 12 are strapping pins (0 = I2S MCLK, 2 = SD D0, 12 = aux
  detect). If USB flashing fails to connect, hold BOOT and tap RST, or pull
  the SD card.

## Safety — it lives in a bedroom

- **Flashing (USB or OTA) and any audio test are physical actuations in
  someone's bedroom.** Each needs explicit per-action authorization from the
  user, every time. Not during sleep hours (21:00–09:00) unless asked. Building
  is always fine.
- Firmware boots silent, fades in, and respects a volume ceiling. No lights at
  night. Details are in D8 (`docs/decisions.md`).
- Never format the SD card, erase flash, or overwrite the stock-firmware
  backup without asking.
- `sudo` on bambam pops the tpm-pin GUI dialog for a human (`~/CLAUDE.md`). Use
  `sudo <cmd>`, never `sudo -i`. Prefer commands that need no root.
  `steve` is in `dialout`, so serial access needs no sudo.

## Dependencies and licensing

Only Apache/MIT/CC0-licensed libraries go into shipping firmware (D5):
ESP32-A2DP (Apache-2.0), ArduinoJson and 256dpi/arduino-mqtt (MIT), and
minimp3 (CC0) if the SD-card phase happens. **Do not add
`arduino-audio-tools`, `arduino-audio-driver` or `arduino-libhelix`.** They are
GPL-3.0. Use them as references for pin maps and codec setup, never as
dependencies, and don't copy their code. Pin every library to an exact
version in `platformio.ini`.

## Secrets

`firmware/.env` is gitignored. It holds Wi-Fi, MQTT and OTA credentials,
injected at build time (Phase 3, the hot-tub `load_env.py` pattern).
`firmware/.env.example` shows the shape. Flash dumps in `backup/` can contain
NVS secrets, so they are gitignored. Treat them as credentials.

## Git workflow

- Work on `main` and commit per checkpoint, with `./scripts/test.sh` green
  first. There is no auto-deploy. Firmware reaches the device only through a
  human-authorized flash (`docs/runbooks/flash.md`), so pushing to `main` is
  inert.
- Tag every flashed build `fw-vX.Y.Z`, matching `FIRMWARE_VERSION`.
- `~/dev` is bind-mounted between bambam and the dev VM. Never run git (or a
  build) from both sides at once.

## Related projects

- **`~/dev/home-assistant`** — HA on t480-server (address in that repo). Any
  HA-side config or service for this device gets added *there*,
  under its own `production` deploy flow. The ESPHome devices there are a
  useful contrast (`esphome/esphome.sh`, a pinned-toolchain wrapper we copied
  the idea from). HA memory `project_pixel3_brown_noise` describes the Pixel 3
  and the `pixel3-charger` plug that this project retires.
- **`~/dev/hot-tub-controller/esp32`** — the PlatformIO/Arduino precedent:
  `load_env.py` secrets, native Unity tests, ArduinoOTA.
- **`~/dev/network`** — DHCP reservations (`docs/inventory.md`) and the IoT
  SSID. (This repo is public: never write SSIDs, LAN IPs or MACs into it.)
- **`~/dev/sleep-monitor`** — the other bedroom project (bedside Pocuter as a
  BLE proxy). The bedroom's "nothing lights up at night" rule comes from there.
