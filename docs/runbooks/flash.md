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

1. In daytime (outside 21:00–09:00), press RST. The board boots, syncs its
   clock and opens a 10-minute update window before starting Bluetooth.
   Pressing KEY1 closes the window early and starts the noise. At night the
   window is skipped.
2. `firmware/.env` has `NOISE_IP` (the board's address, printed at boot) and
   `OTA_PASSWORD`. `load_env.py` feeds both to espota.
3. Bump `FIRMWARE_VERSION` in `src/app/main.cpp`, then run
   `./scripts/pio.sh run -e app_ota -t upload`.
4. Wait at least 70 s, then confirm the version (the boot banner, or the
   status line).

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
