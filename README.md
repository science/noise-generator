# noise-generator

A bedside noise generator on an **ESP32-Audio-Kit (ESP32-A1S, ES8388 codec)**.
It synthesizes grey-to-brown noise and streams it to a Bluetooth speaker. It
runs its own nightly schedule, and has physical buttons for on/off and nap,
tone (darker/brighter) and volume. It updates over the air.

It replaces an old phone that did exactly one job: play a brown-noise MP3 to a
Bluetooth speaker every night.

**Status:** the board is brought up. Noise synthesis and Bluetooth streaming
work on the bench. Building the real firmware is next. See [docs/plans/implementation-plan.md](docs/plans/implementation-plan.md).

## Why this board

The original ESP32 is the only ESP32 with **Bluetooth Classic**, and Bluetooth
Classic is what an A2DP audio *source* needs: something that sends audio to a
speaker. Newer chips (S3, C3, C6) have BLE only. The Audio Kit adds an ES8388
codec with a headphone output, a microSD slot, PSRAM and six buttons.

## Docs

- [docs/plans/implementation-plan.md](docs/plans/implementation-plan.md) — phased implementation plan
- [docs/decisions.md](docs/decisions.md) — design decisions and their reasons
- [docs/hardware.md](docs/hardware.md) — board reference: pins, conflicts, quirks
- [docs/runbooks/bring-up.md](docs/runbooks/bring-up.md) — first USB bring-up
- [docs/runbooks/flash.md](docs/runbooks/flash.md) — USB and OTA flashing

## Build

Requires Linux with `/usr/bin/python3` (3.10+).

```
./scripts/pio.sh --sync      # create .venv with the pinned PlatformIO + esptool
./scripts/test.sh            # host unit tests + compile all firmware envs
./scripts/pio.sh run -e probe -t upload --upload-port /dev/ttyUSB0
./scripts/pio.sh device monitor -e probe --port /dev/ttyUSB0
```

Use the board's **UART** micro-USB port. The other port only supplies power.

## Licence

Apache-2.0. See [LICENSE](LICENSE). The firmware depends only on
permissively licensed libraries. The reasons are in
[docs/decisions.md](docs/decisions.md) (D5).
