# Runbook — flashing firmware (USB and OTA)

## The rule

Flashing this device, over USB or OTA, restarts audio in someone's bedroom.
So does any on-device audio test. **Each one needs explicit authorization for
that specific action**, and the user has to be aware it is happening. Do not
flash during sleep hours (by default 21:00–09:00) unless the user asks for it
then. Authorization for one flash does not carry over to the next one. (This
extends `feedback_live_hardware_supervision` from `~/dev/home-assistant`.)

Building (`./scripts/test.sh build`) is always fine. It touches nothing.

## Before any flash

1. `./scripts/test.sh` is green.
2. `FIRMWARE_VERSION` in the source was bumped, and the commit exists on
   `main`.
3. For OTA, keep a rollback image: copy
   `firmware/.pio/build/<env>/firmware.bin` from the **currently deployed**
   tag to `backup/rollback-<version>-<date>.bin` before building the new one.
   The bootloader also rolls back by itself when a new image fails its
   self-test (decision D1; plan Phase 3 step 4). The file on disk is a second
   safety net in case that does not work.

## USB flash (first flash, or recovery)

```
./scripts/pio.sh run -e <env> -t upload --upload-port /dev/ttyUSB0
```

The board must be on its **UART** port. If the upload cannot connect: hold
BOOT, tap RST, release BOOT. If that fails too, remove the SD card (its
pull-up on GPIO2 can block the bootloader).

## Two gotchas (learned 2026-11-13)

- **Opening the serial port resets the board.** On Linux, the CP2102 driver
  raises the DTR/RTS lines when the port is opened. The board's auto-reset
  circuit turns that into a reset. This happens even if pyserial sets them
  low first. So any serial monitor attach is a reboot.
- **A new OTA image must run for 60 s before it counts as good.** The app
  marks itself valid after 60 s (`kMarkValidAfterMs`). Any reset before then,
  including the serial-port reset above or a power blip, makes the
  bootloader roll back to the previous image. That is the intended
  protection against a bad update, but it means: after a push, **don't open
  the serial port or power-cycle for at least 70 s.** The first OTA test on
  2026-11-13 rolled back for exactly this reason.

## OTA flash (normal, app firmware)

Current practice with the `app` firmware:

1. Open the 10-minute update window, in either of two ways:
   - **Any time:** hold **KEY4 for 3 s**, or send `o` over serial. The noise
     fades, settings are saved, and the board restarts into the window
     (v0.2.2+). Over USB, sending `o` with
     `scratchpad/tools/ota_window.py`-style code works; remember that opening
     the port resets the board first.
   - **Daytime only:** press RST, or power the board up. A night-time boot
     skips the window, and a software restart (the 15:00 resync) skips it
     too.

   While the window is open, Bluetooth is off (Wi-Fi and Bluetooth never
   run together, because of the memory limit), so there is no noise. KEY1
   closes the window early. After the window, the schedule resumes.
2. `firmware/.env` has `NOISE_IP` (the board's address, printed at boot) and
   `OTA_PASSWORD`. `load_env.py` feeds both to espota.
3. Bump `FIRMWARE_VERSION` in `src/app/main.cpp`, then run
   `./scripts/pio.sh run -e app_ota -t upload`.
4. Wait at least 70 s, then confirm the version (the boot banner, or the
   status line).

## Reading the event log remotely (v0.3.0+)

The board keeps its event lines (boot, Wi-Fi, Bluetooth outcomes, scans,
keys that change state) in a 4 KB log in RTC memory. That memory survives a
software restart, but not power loss or the RST button.

1. Hold KEY4 for 3 s. The board restarts into the update window, and the
   log comes along.
2. Run `nc -q 1 <NOISE_IP> 2323`. The board sends the log, oldest line
   first, and closes the connection.
3. Over USB, sending `l` prints the same log. Opening the port resets the
   board, but that is a chip reset: the log is lost, so prefer the Wi-Fi
   path for anything that happened before.

What the Bluetooth lines mean:

- `no answer from speaker (page timeout)`: the board called the speaker's
  address and nothing replied. The speaker is off, out of range, connected
  to something else, or not accepting connections.
- `speaker refused` / `pairing failed (refused)`: it answered, then
  refused the pairing or the connection.
- `stale pairing key`: one side forgot the other. The board deletes its own
  key and re-pairs on the next attempt.
- `[scan] done: speaker VISIBLE rssi N`: the speaker is discoverable (in
  pairing mode). RSSI is signal strength in dBm; above about -80 is fine.

### Original plan text

```
./scripts/pio.sh run -e app_ota -t upload
```

The `app_ota` env uses `upload_protocol = espota`, with the device's DHCP
reservation as `upload_port` and the OTA password taken from `firmware/.env`.
What happens:

- The device fades out and stops Bluetooth/I2S to free the radio and memory.
- It receives the image and reboots into it.
- If the new image passes its self-test, it marks itself valid. Otherwise
  the bootloader rolls back to the previous image on the next reset.

Afterwards:

1. Check in Home Assistant that the `bedroom-noise` firmware-version sensor
   shows the new version, and that the device is available.
2. Tag it: `git tag fw-vX.Y.Z && git push origin fw-vX.Y.Z`.

## Rolling back by hand

- OTA: flash the previous tag's build (or the `backup/rollback-*.bin` file)
  through the same `app_ota` env.
- USB: `./scripts/pio.sh esptool --port /dev/ttyUSB0 write-flash 0x10000
  backup/rollback-<version>.bin`. 0x10000 is the first app slot in
  `min_spiffs.csv`. If the otadata partition says the device boots from the
  second slot, erase otadata first. When in doubt, write the full factory
  image with `-t upload`.
- Factory firmware: `write-flash 0 backup/stock-firmware-<date>.bin`.
