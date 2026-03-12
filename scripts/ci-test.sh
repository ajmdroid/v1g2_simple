#!/bin/bash
# Authoritative repo gate used locally and by GitHub workflows.

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
echo "Authoritative Local CI Gate"
echo "============================================"

section "Semantic Gates"
run_step "Bug pattern scanner" python3 scripts/check_bug_patterns.py
run_step "Bug pattern scanner regression tests" python3 scripts/test_bug_pattern_scanner.py
run_step "BLE deletion semantic guard" python3 scripts/check_ble_deletion_contract.py
run_step "Frontend HTTP resilience semantic guard" python3 scripts/check_frontend_http_resilience_contract.py
run_step "BLE hot-path semantic guard" python3 scripts/check_ble_hot_path_semantic_guard.py
run_step "Display flush semantic guard" python3 scripts/check_display_flush_semantic_guard.py
run_step "SD lock semantic guard" python3 scripts/check_sd_lock_semantic_guard.py
run_step "Main loop semantic guard" python3 scripts/check_main_loop_semantic_guard.py
run_step "Native unit tests" python3 scripts/run_native_tests_serial.py
run_step "Functional scenarios" ./scripts/run_functional_tests.sh

section "Critical Mutation Gate"
run_step "Tracked critical mutation catalog" ./scripts/mutation_test.sh --critical

section "Perf Scoring Gate"
run_step "Deterministic perf scorer regression tests" python3 scripts/test_perf_scoring.py
run_step "Hardware manifest scoring regression tests" python3 scripts/test_hardware_run_scoring.py
run_step "Single hardware test script regression tests" python3 scripts/test_hardware_test_script.py

section "Compatibility Guards"
run_step "WiFi API contracts" python3 scripts/check_wifi_api_contract.py
run_step "BLE hot-path snapshot contract" python3 scripts/check_ble_hot_path_contract.py
run_step "Perf CSV column contract" python3 scripts/check_perf_csv_column_contract.py
run_step "Display flush discipline contract" python3 scripts/check_display_flush_discipline_contract.py
run_step "SD lock discipline contract" python3 scripts/check_sd_lock_discipline_contract.py
run_step "Main loop call-order contract" python3 scripts/check_main_loop_call_order_contract.py
run_step "OBD boot-safety contract" python3 scripts/check_obd_boot_safety_contract.py
run_step "Extern/global usage contract" python3 scripts/check_extern_usage_contract.py

section "Docs Hygiene"
run_step "Perf SLO doc/json contract" python3 scripts/check_perf_slo_contract.py

section "Frontend"
cd interface
run_step "Frontend dependencies" bash -lc "npm ci --silent 2>/dev/null || npm install --silent"
run_step "Frontend lint/type checks" npm run lint
run_step "Frontend unit tests with coverage" npm run test:coverage
run_step "Frontend build" npm run build
run_step "Frontend deploy" npm run deploy
cd "$ROOT_DIR"

section "Frontend Packaging"
run_step "Web asset guardrails" python3 scripts/check_web_asset_budget.py
run_step "Audio manifest guardrail" python3 scripts/check_audio_asset_manifest.py

section "Firmware Build"
run_step "Firmware static analysis" ./scripts/pio-check.sh
run_step "Firmware build" pio run -e waveshare-349 -j 1
run_step "LittleFS image build" pio run -e waveshare-349 -t buildfs -j 1
run_step "Flash package truth report" python3 scripts/report_flash_package_size.py \
  --max-firmware-bytes 5570560 \
  --expect-littlefs-bytes 2424832

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))
echo ""
echo -e "${GREEN}All gates passed in ${ELAPSED}s${NC}"

# Emit timing artifact for budget checker
TIMING_DIR="$ROOT_DIR/.artifacts/test_reports/ci-test"
mkdir -p "$TIMING_DIR"
echo "{\"elapsed_seconds\": ${ELAPSED}, \"lane\": \"ci-test\"}" > "$TIMING_DIR/timing.json"

section "Size Report"
echo "waveshare-349:"
pio run -e waveshare-349 -t size -j 1 2>/dev/null | grep -E "(RAM|Flash|used|bytes)"

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "============================================"
echo -e "${GREEN}All CI checks passed${NC}"
echo "Elapsed: ${ELAPSED}s"
echo "============================================"
