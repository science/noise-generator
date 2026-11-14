# Hardware reference — AI-Thinker ESP32-Audio-Kit V2.2 (ESP32-A1S)

Status markers used below:

- **[confirmed]** — seen on this physical board with the probe firmware.
- **[from schematic]** — read from the vendor's module schematic (Amazon
  listing image, 2026-11-13), not yet measured.
- **[from library/community]** — from pschatzmann's `arduino-audio-driver` pin
  tables, trombik's `esp-adf-component-ai-thinker-esp32-a1s`, or forum posts.
  These disagree across board revisions, so treat them as hypotheses.

## Confirmed on 2026-11-13 (esptool + stock boot log)

- **Chip: ESP32-D0WD-V3, revision v3.1**, dual core, 240 MHz, 40 MHz crystal.
  esptool lists "Wi-Fi, BT", so the Bluetooth Classic hardware is present
  **[confirmed]**.
- **Flash: 4 MB** (JEDEC manufacturer 0x5E, device 0x4016). The flash voltage
  strap reads 3.3 V **[confirmed]**. So there is no 8 MB escape hatch for
  the flash budget (`docs/plan.md`, "Known constraint").
- **USB bridge: Silicon Labs CP2102** (`10c4:ea60`) → `/dev/ttyUSB0`
  **[confirmed]**. The first micro-USB cable was bad. It gave power but no
  data, which looked exactly like "plugged into the POWER port".
- **Stock firmware:** AI-Thinker "B&T" firmware v1.1.0, built 2021-06-08 on
  ESP-IDF v3.3.2. A single 1 MB factory app partition, no OTA slots.
  It is backed up as `backup/stock-firmware-2026-11-13.bin` (full 4 MB). That
  file was checked against the chip's own checksum (`esptool verify-flash`:
  digest matched), and its SHA-256 is in the `.sha256` file next to it.
- The MAC addresses are in the boot log and the backup. They are deliberately
  not written here, because this repo is public.

## Confirmed on 2026-11-13 (probe firmware)

- **PSRAM: 4096 KB** found and usable **[confirmed]**.
- **Board variant: ES8388 "v1"** **[confirmed]**. The codec answers at I2C
  0x10 on **SDA 33 / SCL 32**, and nothing answers on SDA 18 / SCL 23. This
  **contradicts the vendor's module schematic** (the Amazon image), which
  shows the v2 wiring. That image is evidently for a different module
  revision. Trust the probe, not the listing.
- Bluetooth Classic is compiled into the pinned core and present on the chip
  **[confirmed]**.
- The restore path works **[confirmed]**. Writing the factory backup back
  (`esptool write-flash 0 backup/stock-firmware-2026-11-13.bin`, 8.5 s,
  hash verified) booted the stock firmware again, before the probe replaced
  it.

## What the board is

The vendor listing calls it **ESP32-Audio-Kit_V2.2**, built on the
AI-Thinker **ESP32-A1S** module. The listing and the schematic say:

- The module holds an **original ESP32** (dual-core Xtensa LX6, 240 MHz), plus
  a separate SPI flash chip and a separate SPI **PSRAM** chip on the module
  **[from schematic]**. The A1S is normally sold as 4 MB flash + 8 MB PSRAM.
  The probe and `esptool flash-id` confirm the real sizes.
- **Bluetooth 4.2 BR/EDR + BLE** (the listing). "BR/EDR" is Bluetooth Classic,
  the part of Bluetooth that carries A2DP stereo audio. The original ESP32 is
  the only ESP32 family member with Classic. The ESP32-S3, C3 and C6 have BLE
  only and **cannot** stream audio to a Bluetooth speaker. That is why this
  board, and not a newer chip, fits the job.
- The audio codec is an **Everest ES8388** **[from schematic]**: a stereo
  DAC + ADC, with a headphone amplifier, controlled over I2C and fed audio over
  I2S. (Some older A1S boards used an AC101 codec instead. That chip is end of
  life, and the schematic shows ES8388.)
- There are two small class-D speaker amplifiers for the two JST speaker
  connectors. Their shared enable line is GPIO21 **[from library/community]**.
- Power: 5 V, 4.7–5.3 V, and the listing asks for a supply that can deliver
  more than 1 A. A powered USB hub port is fine. Wi-Fi and Bluetooth transmit
  bursts are the peaks.

The listing claims MP3/AAC/FLAC/WAV playback, "A2DP Source", DLNA and so on.
Those are features of Espressif's ESP-ADF software framework, not of the board.
The board is just an ESP32 plus a codec, and any of them can be built on it.

## The two 3.5 mm jacks

The user remembered them as 2.5 mm. On this board they are 3.5 mm:

- **Headphone out (`PHONEJACK`)** — the ES8388 headphone driver. This is the
  analog line output we would feed into a speaker's AUX input, or into a
  Bluetooth transmitter dongle (see decision D2 in `docs/decisions.md`).
  Plug-in detection is on GPIO39 **[from library/community]**.
- **Line in (`LINEIN` / AUX in)** — the ES8388 ADC input. We have no use for it.
  Plug-in detection is on GPIO12 **[from library/community]**.

GPIO12 is also an ESP32 *strapping pin*. The chip reads its level at reset to
choose the flash voltage. Firmware must only ever read it, never drive it.

## Codec wiring — this board is the "v1" pin map **[confirmed]**

`arduino-audio-driver` names two ES8388 variants:

- **v1**: I2C on SDA 33 / SCL 32. I2S: MCLK 0, BCLK 27, WS (LRCK) 25,
  DOUT (ESP32 → codec) 26, DIN (codec → ESP32) 35.
- **v2**: I2C on SDA 18 / SCL 23, BCLK on GPIO5 (the rest the same).

**This board is v1.** The probe found the ES8388 at 0x10 on SDA 33 / SCL 32
(`firmware/lib/BoardId`). The vendor's module schematic shows v2 wiring, but
it does not match this board. The I2C pins are confirmed. The v1 I2S pins
are still **[from library/community]** until Phase 1 plays audio through
them.

## Buttons (KEY1–KEY6) — all six usable **[confirmed]**

The board has **eight buttons**. Left to right, they are labelled RST, BOOT,
KEY1 … KEY6.

- **RST** resets the chip (it pulls the EN line low). It is not a GPIO, and
  firmware cannot read it. Every press reboots the board. The probe's
  one-blink-on-boot LED made it look like a "button light" on 2026-11-13.
- **BOOT** is GPIO0, a strapping pin. If held during reset, the chip enters
  the USB bootloader instead of running firmware. On this v1 board GPIO0 is
  also the codec's **I2S MCLK** (master clock) output. Pressing BOOT while
  analog audio plays would short that clock to ground, so firmware must not
  use BOOT as a button, and it should not be pressed during analog playback.
  It is harmless during Bluetooth-only output, because I2S is then not
  running.

All six user keys were pressed
in turn on 2026-11-13. Each logged exactly one PRESSED and one released
event, with no bounce visible at the probe's 10 ms polling rate. All six
are active-low: the line reads LOW while the key is held. The silkscreen
order matches the GPIO map below. The function names in parentheses are
ESP-ADF's defaults, not ours.

- KEY1 = GPIO36 (Mode). GPIO36 is input-only and has no internal pull-up; the
  board's external pull-up works.
- KEY2 = GPIO13 (Rec). Also the SD card's DAT3 line. It is only an issue if
  the SD card is used, and the SD card is optional.
- KEY3 = GPIO19 (Play).
- KEY4 = GPIO23 (Set).
- KEY5 = GPIO18 (Vol−).
- KEY6 = GPIO5 (Vol+).

Because the board is v1, none of these pins is shared with the codec. The
v2-only concerns from the first draft of this doc (keys doubling as the
codec's I2C or I2S clock) do not apply.

## SD card slot

**Optional for this project** (user, 2026-11-13). Firmware lives in the
module's own 4 MB flash, and synthesized noise needs no files, so the device
runs with the slot empty. The card only matters if we later want recorded
sounds (plan Phase 5). Code cannot run from the SD card, so it does not help
the flash budget either. Leaving the slot empty also frees KEY2 (GPIO13) and
removes the GPIO2 strapping-pin nuisance during USB flashing.

- Wiring **[from library/community]**: SDMMC on the ESP32's fixed pins.
  CLK = GPIO14, CMD = GPIO15, D0 = GPIO2, D3 = GPIO13. Use 1-bit mode. 4-bit
  mode would also need D1 = GPIO4 and D2 = GPIO12, and GPIO12 is the strapping
  pin and the aux-detect line.
- GPIO2 is a strapping pin as well. The SD card's pull-up on it can interfere
  with flashing on some boards. If a USB flash fails with "Failed to connect",
  remove the SD card and try again.
- The DIP switches (the 5-way block in the middle of the board) connect pull-up
  resistors to some of these lines. trombik's notes say **the `DATA3` and
  `CMD` switches must be ON for the SD card to work**. A community ESPHome
  config for the V2.2 board uses switches **1, 2, 3 ON and 4, 5 OFF**. Record
  the real silkscreen labels and working positions here at bring-up.
- **The 64 GB card from the old phone will be SDXC, formatted exFAT or as an
  Android "adopted" encrypted volume.** The Arduino SD_MMC library mounts FAT32
  reliably. exFAT depends on a FatFs build option we should not rely on. So:
  reformat it as a single FAT32 partition. On Linux:
  `sudo mkfs.vfat -F 32 -n NOISE /dev/sdX1`, after creating one partition with
  `fdisk`. FAT32 caps a single file at 4 GB. That is about 6.7 hours of 16-bit
  44.1 kHz stereo WAV, or days of MP3. Procedure:
  `docs/runbooks/bring-up.md`.

## USB ports — why the board did not enumerate on 2026-11-13

The Audio Kit has **two micro-USB ports**:

- **UART** — wired to a USB-to-serial bridge (a CP2102 or CH340, which shows up
  as `/dev/ttyUSB0`). This is the port for flashing and serial logs.
- **POWER** — 5 V only, no data lines.

On 2026-11-13 the board was plugged into the Plugable hub, but `lsusb` showed
no serial bridge and there was no `/dev/ttyUSB*`. The likeliest cause is that
the cable is in the **POWER** port. Other possibilities: a charge-only cable,
or the board's power switch is off. Fix: move the cable to the port labelled
UART. Then `./scripts/test.sh status` should list `/dev/ttyUSB0`, and `lsusb`
should show `10c4:ea60` (CP2102) or `1a86:7523` (CH340).

The user `steve` is already in the `dialout` group, so no udev rule or sudo is
needed to open the port.

## Other on-board parts

- LEDs: one user LED on GPIO22 **[from library/community]**. There is probably
  also an always-on red power LED. That one may need tape for a dark bedroom,
  since firmware cannot turn it off.
- Two on-board MEMS microphones (not needed). Some boards need capacitor
  changes before they work at all.
- A Li-ion battery connector and charger. The charger is not wired to any GPIO,
  so battery level cannot be read.
- A 14-pin header **[from Amazon listing]** that exposes GND, IO0, RST,
  TX0/RX0, 3V3, IO21, IO22, IO19, IO23, IO18, IO5. Useful for an external
  serial adapter, or for a logic analyser on the codec lines.

## Sources

- Vendor module schematic and header pinout: Amazon "EC Buying ESP32-Audio-Kit"
  listing images (user-provided, 2026-11-13).
- `arduino-audio-driver` board definitions `AudioKitEs8388v1.h`,
  `AudioKitEs8388v2.h`, `AudioKitAC101.h`
  (github.com/pschatzmann/arduino-audio-driver, `src/AudioBoards/`).
- trombik, ESP-ADF board component for the A1S
  (github.com/trombik/esp-adf-component-ai-thinker-esp32-a1s). It tests
  A2DP sink only; A2DP source is listed as untested.
- Home Assistant community thread "ESP32-A1S Audio Kit Media Player"
  (community.home-assistant.io/t/522245) for key pins and DIP settings.
