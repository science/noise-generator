#!/usr/bin/env bash
# Unified test entry point. Claude Code sessions must use this rather than
# ad-hoc pio commands, so every run checks the same things the same way.
#
#   ./scripts/test.sh            # native unit tests + build every firmware env
#   ./scripts/test.sh native     # host-side Unity tests only (fast, no device)
#   ./scripts/test.sh build      # compile every device env (no upload, no device)
#   ./scripts/test.sh status     # show toolchain + whether a board is attached
#
# Nothing here uploads firmware or talks to a device. Flashing is a separate,
# human-authorized step — see docs/runbooks/flash.md.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FW="$ROOT/firmware"
PIO="${PIO:-$ROOT/scripts/pio.sh}"
# Device envs to compile. Add new ones here as phases land (app, app_ota...).
DEVICE_ENVS=(probe app bttest libcheck)

run_native() {
  echo "== native unit tests"
  "$PIO" test -e native
}

run_build() {
  for env in "${DEVICE_ENVS[@]}"; do
    echo "== build $env"
    "$PIO" run -e "$env"
  done
}

status() {
  echo "pio: $("$PIO" --version)"
  echo "platform pin: $(grep -E '^platform *= *https' "$FW/platformio.ini" | head -1 | sed 's/.*download\///')"
  if ls /dev/ttyUSB* /dev/ttyACM* >/dev/null 2>&1; then
    echo "serial ports:"; ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
  else
    echo "serial ports: none — board not attached, or on its power-only USB port"
  fi
}

case "${1:-all}" in
  native) run_native ;;
  build)  run_build ;;
  status) status ;;
  all)    run_native; run_build ;;
  *) echo "usage: $0 [all|native|build|status]" >&2; exit 2 ;;
esac
