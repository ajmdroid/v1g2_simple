#!/bin/bash
# Hardware qualification matrix — runs qualification across all lab boards.
#
# Board roles (from test/device/board_inventory.json):
#   release — full hardware test gate (build + flash + device tests + soak)
#   radio   — full hardware test gate (BLE+WiFi coexistence focus)
#   stress  — full hardware test gate (stability focus)
#
# All boards route through ./scripts/hardware/test.sh for unified metrics
# and artifact storage.
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
  section "Board: release — Full Hardware Test"
  run_board release "Release board hardware test" \
    ./scripts/hardware/test.sh --all --board-id release --strict
fi

# ── Radio board ──────────────────────────────────────────────────────
if [ "$BOARD_FILTER" = "all" ] || [ "$BOARD_FILTER" = "radio" ]; then
  section "Board: radio — Full Hardware Test"
  run_board radio "Radio board hardware test" \
    ./scripts/hardware/test.sh --all --board-id radio
fi

# ── Stress board ─────────────────────────────────────────────────────
if [ "$BOARD_FILTER" = "all" ] || [ "$BOARD_FILTER" = "stress" ]; then
  section "Board: stress — Full Hardware Test"
  run_board stress "Stress board hardware test" \
    ./scripts/hardware/test.sh --all --board-id stress
fi

echo ""
if [ "$FAILURES" -gt 0 ]; then
  echo -e "${RED}Hardware matrix: ${FAILURES} board(s) FAILED${NC}"
  exit 1
else
  echo -e "${GREEN}Hardware matrix: all tested boards PASSED${NC}"
fi
