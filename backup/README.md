# backup/

Flash images live here and are gitignored (see `.gitignore`).

- `stock-firmware-<date>.bin` — a full 4 MB read of the board's flash taken
  before our first flash, so the factory demo firmware can be restored with
  `esptool write-flash 0x0 <file>`. Procedure: `docs/runbooks/bring-up.md`.
- `rollback-<version>-<date>.bin` — the previous known-good app image, taken
  before any risky OTA. Procedure: `docs/runbooks/flash.md`.

A full flash dump can contain Wi-Fi credentials stored in NVS once our
firmware has run, so treat these files as secrets.
