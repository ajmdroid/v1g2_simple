#!/bin/bash
# Hardware qualification matrix — runs qualification across all lab boards.
#
# Board roles (from test/device/board_inventory.json):
#   release — full qualify_hardware.sh gate
#   radio   — full device test suite (BLE+WiFi coexistence)
#   stress  — extended soak (20 cycles, 6s cooldown)
#
# Usage:
#   ./scripts/qualify_hardware_matrix.sh           # run all boards
#   ./scripts/qualify_hardware_matrix.sh release    # run release board only
#   ./scripts/qualify_hardware_matrix.sh radio      # run radio board only
#   ./scripts/qualify_hardware_matrix.sh stress     # run stress board only

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

BOARD_FILTER="${1:-all}"
INVENTORY="test/device/board_inventory.json"
FAILURES=0

section() {
  echo ""
  echo -e "${BLUE}== $1 ==${NC}"
}

run_board() {
  local board_id="$1"
  local label="$2"
  shift 2
  echo -e "${YELLOW}[run] ${label}${NC}"
  if "$@"; then
    echo -e "${GREEN}[pass] ${label}${NC}"
  else
    echo -e "${RED}[fail] ${label}${NC}"
    FAILURES=$((FAILURES + 1))
  fi
}

if [ ! -f "$INVENTORY" ]; then
  echo "Board inventory not found: $INVENTORY" >&2
  echo "Create test/device/board_inventory.json with board definitions." >&2
  exit 2
fi

echo "============================================"
echo "Hardware Qualification Matrix"
echo "============================================"

# ── Release board ────────────────────────────────────────────────────
if [ "$BOARD_FILTER" = "all" ] || [ "$BOARD_FILTER" = "release" ]; then
  section "Board: release — Full Qualification"
  run_board release "Release board qualification" \
    ./scripts/qualify_hardware.sh --board-id release
fi

# ── Radio board ──────────────────────────────────────────────────────
if [ "$BOARD_FILTER" = "all" ] || [ "$BOARD_FILTER" = "radio" ]; then
  section "Board: radio — Device Tests (full)"
  RADIO_PORT=$(python3 -c "
import json
with open('$INVENTORY') as f:
    inv = json.load(f)
for b in inv['boards']:
    if b['board_id'] == 'radio':
        print(b['device_path'])
        break
" 2>/dev/null || echo "")
  if [ -n "$RADIO_PORT" ] && [ -e "$RADIO_PORT" ]; then
    run_board radio "Radio board device tests" \
      env DEVICE_PORT="$RADIO_PORT" DEVICE_BOARD_ID="radio" ./scripts/run_device_tests.sh --full
  else
    echo -e "${YELLOW}[skip] Radio board not connected at expected port${NC}"
  fi
fi

# ── Stress board ─────────────────────────────────────────────────────
if [ "$BOARD_FILTER" = "all" ] || [ "$BOARD_FILTER" = "stress" ]; then
  section "Board: stress — Soak Test"
  STRESS_PORT=$(python3 -c "
import json
with open('$INVENTORY') as f:
    inv = json.load(f)
for b in inv['boards']:
    if b['board_id'] == 'stress':
        print(b['device_path'])
        break
" 2>/dev/null || echo "")
  if [ -n "$STRESS_PORT" ] && [ -e "$STRESS_PORT" ]; then
    run_board stress "Stress board soak (20 cycles)" \
      env DEVICE_PORT="$STRESS_PORT" DEVICE_BOARD_ID="stress" ./scripts/run_device_soak.sh --cycles 20 --cooldown-seconds 6
  else
    echo -e "${YELLOW}[skip] Stress board not connected at expected port${NC}"
  fi
fi

echo ""
if [ "$FAILURES" -gt 0 ]; then
  echo -e "${RED}Hardware matrix: ${FAILURES} board(s) FAILED${NC}"
  exit 1
else
  echo -e "${GREEN}Hardware matrix: all tested boards PASSED${NC}"
fi
