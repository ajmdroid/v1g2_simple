#!/bin/bash
# Nightly validation gate.
# Runs everything in the PR gate plus:
#   - full replay corpus
#   - sanitizer lane (ASan + UBSan)
#   - expanded mutation catalog with tier thresholds
#   - device soak (if hardware available)
#   - timing budget check
#
# This script is NOT intended for PR gating. Use ci-test.sh for PRs.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

START_TIME=$(date +%s)

section() {
  echo ""
  echo -e "${BLUE}== $1 ==${NC}"
}

run_step() {
  local label="$1"
  shift
  echo -e "${YELLOW}[run] ${label}${NC}"
  "$@"
  echo -e "${GREEN}[pass] ${label}${NC}"
}

echo "============================================"
echo "Nightly Validation Gate"
echo "============================================"

if [[ "${WORKSPACE_CLEANED:-0}" != "1" ]]; then
  section "Workspace Cleanup"
  run_step "Safe cleanup" python3 scripts/clean_workspace.py --safe --apply
  export WORKSPACE_CLEANED=1
fi

# ── PR gate (authoritative code gate runs first) ─────────────────────
section "PR Gate (ci-test.sh)"
run_step "Full PR gate" ./scripts/ci-test.sh

# ── Full replay corpus ───────────────────────────────────────────────
section "Replay Corpus (full)"
run_step "Full replay suite" python3 scripts/run_replay_suite.py --lane nightly

# ── Sanitizer lane ───────────────────────────────────────────────────
section "Sanitizer Lane (ASan + UBSan)"
SANITIZER_SUITES=(
  test_packet_parser
  test_drive_replay
  test_lockout_index
  test_camera_alert_module
  test_volume_fade
)
for suite in "${SANITIZER_SUITES[@]}"; do
  run_step "Sanitizer: ${suite}" python3 scripts/run_native_tests_serial.py --env native-sanitized "${suite}"
done

# ── Expanded mutation catalog ────────────────────────────────────────
section "Expanded Mutation Catalog"
run_step "Full mutation catalog" ./scripts/mutation_test.sh --full

# ── Hardware test (optional — requires connected hardware) ────────────
section "Hardware Test (if available)"
if command -v pio >/dev/null 2>&1 && pio device list 2>/dev/null | grep -qE 'usbmodem|ttyACM|ttyUSB'; then
  run_step "Hardware test (nightly)" ./scripts/hardware/test.sh --all --board-id release
else
  echo -e "${YELLOW}[skip] No device detected — skipping hardware test${NC}"
fi

# ── Timing ───────────────────────────────────────────────────────────
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

TIMING_DIR="$ROOT_DIR/.artifacts/test_reports/nightly"
mkdir -p "$TIMING_DIR"
echo "{\"elapsed_seconds\": ${ELAPSED}, \"lane\": \"nightly\"}" > "$TIMING_DIR/timing.json"

section "Budget Check"
run_step "Nightly timing budget" python3 scripts/check_ci_budget.py nightly "$TIMING_DIR/timing.json"

echo ""
echo -e "${GREEN}Nightly validation passed in ${ELAPSED}s${NC}"
