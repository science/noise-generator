# Runbook — first bring-up of the Audio Kit (Phase 0)

Nothing in this runbook makes sound. The probe firmware holds the speaker amp
off and never configures the codec. Step 3 writes to the board (it replaces the
factory demo firmware), which is why step 2 backs that firmware up first.

All commands run from the repo root. They use `./scripts/pio.sh`, which runs
the pinned PlatformIO and esptool. Run `./scripts/pio.sh --sync` once per
machine first.

## 1. Get the board onto USB

1. Use the micro-USB port labelled **UART**, not the one labelled POWER. The
   POWER port has no data lines. That is the likeliest reason nothing showed up
   on 2026-11-13.
2. Use a data-capable cable. Many phone cables are charge-only.
3. If the board has a power slide switch, it must be on (a power LED lights).
4. Check that the port is there:

   ```
   ./scripts/test.sh status        # should list /dev/ttyUSB0
   lsusb | grep -iE '10c4:ea60|1a86:7523'   # CP2102 or CH340 bridge
   ```

   `steve` is in the `dialout` group, so no sudo is needed to open the port.

## 2. Identify the chip and back up the stock firmware

```
./scripts/pio.sh esptool --port /dev/ttyUSB0 chip-id
./scripts/pio.sh esptool --port /dev/ttyUSB0 flash-id
```

esptool resets the board into its ROM bootloader automatically. It does this
with the serial adapter's DTR/RTS lines. If it says "Failed to connect":

- Hold **BOOT**, tap **RST**, then release BOOT, and run the command again.
- If that still fails, remove the SD card. The card's pull-up on GPIO2 (a
  strapping pin) can block entry to the bootloader.

Write down the chip revision, the MAC address and the detected flash size
("Detected flash size: 4MB"). Then read the whole flash. Use the size esptool
reported: 0x400000 is 4 MB, 0x800000 is 8 MB.

```
./scripts/pio.sh esptool --port /dev/ttyUSB0 read-flash 0 0x400000 \
    backup/stock-firmware-$(date +%F).bin
```

`backup/` is gitignored. To restore the factory firmware later:
`./scripts/pio.sh esptool --port /dev/ttyUSB0 write-flash 0 backup/stock-firmware-<date>.bin`.

## 3. Flash and read the probe

```
./scripts/pio.sh run -e probe -t upload --upload-port /dev/ttyUSB0
./scripts/pio.sh device monitor -e probe --port /dev/ttyUSB0
```

(Claude Code has no terminal for an interactive monitor. It can capture the
output with a bounded read instead, for example
`timeout 40 ./scripts/pio.sh device monitor -e probe --port /dev/ttyUSB0 --quiet`,
after resetting the board.)

The probe prints a report at boot, then every 30 s. Type `r` to print it on
demand. Record the following in `docs/hardware.md` and change the matching
items to **[confirmed]**:

- `chip`, `features` — expect `BT-Classic=yes`.
- `flash`, `PSRAM` — expect 4 MB / about 8 MB. If PSRAM shows 0, check the
  `-DBOARD_HAS_PSRAM` build flag and the module marking.
- `I2C v1` / `I2C v2` and `board variant` — 2026-11-13 result: **ES8388
  v1** (0x10 on SDA33/SCL32). The vendor schematic predicted v2, wrongly.
- `SD card` — see step 4.
- Press **each key in turn**, KEY1 to KEY6. Note which ones log `PRESSED`. On
  v2 the probe does not watch KEY4–6, because those pins belong to the codec.
- Plug headphones in and out to see the headphone-detect pin change.
- Write down the DIP switch positions as found, with their silkscreen labels.

## 4. The 64 GB SD card

If the probe reports `SD card: mounted`, the card is usable as it is. If it
reports `mount FAILED`, work through these in order:

1. **DIP switches.** The `DATA3` and `CMD` pull-up switches must be ON. Fix
   them, reset the board, and check again.
2. **Filesystem.** A card from a phone is usually exFAT, or an encrypted
   Android "adoptable storage" volume. Neither will mount. Reformat it as FAT32.
   **This wipes the card. Confirm with the user first** (they said on
   2026-11-13 that it probably needs reinitialising).

   Put the card in a USB reader on bambam. Identify it carefully: compare
   `lsblk` with the card removed and inserted, and check that the size is
   about 64 GB. Then:

   ```
   lsblk -o NAME,SIZE,MODEL,TRAN          # find the card, e.g. /dev/sdX
   sudo wipefs -a /dev/sdX                # remove old partition signatures
   echo 'type=c' | sudo sfdisk /dev/sdX   # one FAT32 (LBA) partition, whole card
   sudo mkfs.vfat -F 32 -n NOISE /dev/sdX1
   ```

   `sudo` here triggers the tpm-pin GUI PIN dialog on bambam (see
   `~/CLAUDE.md`). A human has to be at the desk to enter the PIN. Use
   `sudo <cmd>`, never `sudo -i`.

   Linux's `mkfs.vfat` formats cards larger than 32 GB as FAT32 without
   complaint. (Windows' own formatter refuses to.) Use 32 KB clusters (the
   default at this size). Large clusters suit long audio files.

3. Re-insert the card into the board, reset, and confirm `mounted`.

## 5. Finish

- Update `docs/hardware.md`: board variant, keys, DIP switches, flash/PSRAM,
  MAC, SD result.
- If the variant differs from v1, the codec pins in the Phase 1 plan change.
  Update `docs/plan.md` too.
- Commit: `docs: record Phase 0 bring-up findings`.
