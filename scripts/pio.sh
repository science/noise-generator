#!/usr/bin/env bash
# pio.sh — run PlatformIO (and esptool) at the versions requirements.txt pins,
# or refuse. Modelled on ~/dev/home-assistant/esphome/esphome.sh.
#
# The venv lives at <repo>/.venv on the ~/dev bind mount, built on
# /usr/bin/python3 (3.12.3 on both bambam and the dev VM), so the same venv
# serves both sides. Do not rebuild it against pyenv.
#
# PlatformIO keeps platforms/toolchains in PLATFORMIO_CORE_DIR. We point that
# at <repo>/.platformio-core so this repo's pioarduino platform never collides
# with the global ~/.platformio used by hot-tub-controller.
#
# Usage (any directory):
#   ./scripts/pio.sh --sync                 # build/repair .venv to match the pin
#   ./scripts/pio.sh run -e probe           # any pio subcommand (runs in firmware/)
#   ./scripts/pio.sh esptool chip-id        # pinned esptool
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VENV="${PIO_VENV:-$ROOT/.venv}"
REQ="$ROOT/requirements.txt"
PYTHON="${PIO_PYTHON:-/usr/bin/python3}"
export PLATFORMIO_CORE_DIR="${PLATFORMIO_CORE_DIR:-$ROOT/.platformio-core}"

pin_of() { grep -E "^$1==[0-9][^[:space:]]*$" "$REQ" | head -1 | cut -d= -f3; }
installed_of() {
  "$VENV/bin/python" -c "import importlib.metadata as m; print(m.version('$1'))" 2>/dev/null
}

PIO_PIN="$(pin_of platformio)"
ESPTOOL_PIN="$(pin_of esptool)"
[ -n "$PIO_PIN" ] && [ -n "$ESPTOOL_PIN" ] || {
  echo "$REQ must pin platformio==X and esptool==Y exactly" >&2; exit 1; }

if [ "${1:-}" = "--sync" ]; then
  [ -x "$VENV/bin/python" ] || "$PYTHON" -m venv "$VENV"
  "$VENV/bin/pip" install --quiet -r "$REQ"
  for p in platformio esptool; do
    want="$(pin_of $p)"; have="$(installed_of $p || true)"
    [ "$want" = "$have" ] || { echo "$p: venv has '${have:-none}', pin is '$want'" >&2; exit 1; }
  done
  echo "platformio $PIO_PIN + esptool $ESPTOOL_PIN ready at $VENV"
  exit 0
fi

[ -x "$VENV/bin/pio" ] || { echo "no venv at $VENV — run: ./scripts/pio.sh --sync" >&2; exit 1; }
for p in platformio esptool; do
  want="$(pin_of $p)"; have="$(installed_of $p || true)"
  if [ "$want" != "$have" ]; then
    echo "ERROR: $p in $VENV is '${have:-none}', requirements.txt pins '$want'." >&2
    echo "Refusing to run — a drifted toolchain silently changes the firmware." >&2
    echo "  Fix: ./scripts/pio.sh --sync" >&2
    exit 1
  fi
done

if [ "${1:-}" = "esptool" ]; then
  shift; exec "$VENV/bin/esptool" "$@"
fi
cd "$ROOT/firmware"
exec "$VENV/bin/pio" "$@"
