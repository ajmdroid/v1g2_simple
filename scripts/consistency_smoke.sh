#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

run_step() {
  echo "==> $*"
  "$@"
}

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO (pio) is required but not found in PATH." >&2
  exit 1
fi

run_step pio run -e waveshare-349
run_step pio run -e waveshare-349-windows
run_step pio test -e native \
  -f test_ble_client \
  -f test_packet_parser \
  -f test_connection_state \
  -f test_wifi_manager

if command -v npm >/dev/null 2>&1 && [ -f "$ROOT_DIR/interface/package.json" ]; then
  run_step npm --prefix "$ROOT_DIR/interface" run lint
fi
