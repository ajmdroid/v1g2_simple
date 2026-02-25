#!/usr/bin/env bash
#
# iron-gate.sh - Single-command, objective ship gate for modularization hardening.
#
# True PASS point:
#   1) firmware_build = PASS
#   2) sd_lock_contract = PASS
#   3) parser_native_smoke = PASS
#   4) device_suite = PASS (all items inside scripts/device-test.sh)
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR=""
DEVICE_FLASH_MODE="--with-flash"
DRY_RUN=0
DEVICE_ARGS=()

usage() {
  cat <<'USAGE'
Usage: ./scripts/iron-gate.sh [options] [device-test options...]

Options:
  --with-flash                 Force flash before device gate (default)
  --skip-flash                 Skip flash in device gate
  --out-dir PATH               Write artifacts to PATH
  --dry-run                    Print resolved commands and exit
  -h, --help                   Show help

Any unknown option is forwarded to scripts/device-test.sh.
Examples:
  ./scripts/iron-gate.sh
  ./scripts/iron-gate.sh --skip-flash --duration-seconds 90
  ./scripts/iron-gate.sh --with-flash --metrics-url http://192.168.160.212/api/debug/metrics
USAGE
}

is_uint() {
  local value="${1:-}"
  [[ "$value" =~ ^[0-9]+$ ]]
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-flash)
      DEVICE_FLASH_MODE="--with-flash"
      ;;
    --skip-flash)
      DEVICE_FLASH_MODE="--skip-flash"
      ;;
    --out-dir)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --out-dir" >&2
        exit 2
      fi
      OUT_DIR="$2"
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      DEVICE_ARGS+=("$1")
      ;;
  esac
  shift
done

if [[ -z "$OUT_DIR" ]]; then
  timestamp="$(date +%Y%m%d_%H%M%S)"
  OUT_DIR="$ROOT_DIR/.artifacts/iron_gate/iron_${timestamp}"
fi
mkdir -p "$OUT_DIR"

RESULTS_TSV="$OUT_DIR/results.tsv"
SUMMARY_MD="$OUT_DIR/summary.md"

printf "step\tstatus\tduration_s\tartifact\tnotes\n" > "$RESULTS_TSV"

append_result() {
  local step="$1"
  local status="$2"
  local duration_s="$3"
  local artifact="$4"
  local notes="$5"
  printf "%s\t%s\t%s\t%s\t%s\n" "$step" "$status" "$duration_s" "$artifact" "$notes" >> "$RESULTS_TSV"
}

run_step() {
  local step="$1"
  local cmd="$2"
  local log_path="$3"
  local artifact="$4"

  local start_s end_s duration_s
  start_s="$(date +%s)"

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "[dry-run] $step: $cmd"
    append_result "$step" "DRY_RUN" "0" "$artifact" "$cmd"
    return 0
  fi

  echo "==> [$step]"
  set +e
  bash -lc "$cmd" >"$log_path" 2>&1
  local rc=$?
  set -e

  end_s="$(date +%s)"
  duration_s=$((end_s - start_s))

  if [[ $rc -eq 0 ]]; then
    append_result "$step" "PASS" "$duration_s" "$artifact" "ok"
    echo "    PASS (${duration_s}s)"
    return 0
  fi

  append_result "$step" "FAIL" "$duration_s" "$artifact" "rc=$rc"
  echo "    FAIL (${duration_s}s) - see $log_path" >&2
  return $rc
}

BUILD_LOG="$OUT_DIR/firmware_build.log"
CONTRACT_LOG="$OUT_DIR/sd_lock_contract.log"
NATIVE_LOG="$OUT_DIR/parser_native_smoke.log"
DEVICE_LOG="$OUT_DIR/device_suite.log"
DEVICE_OUT_DIR="$OUT_DIR/device_suite"
mkdir -p "$DEVICE_OUT_DIR"

DEVICE_CMD=("./scripts/device-test.sh" "$DEVICE_FLASH_MODE" "--out-dir" "$DEVICE_OUT_DIR")
if [[ ${#DEVICE_ARGS[@]} -gt 0 ]]; then
  DEVICE_CMD+=("${DEVICE_ARGS[@]}")
fi

printf "Iron Gate Config:\n"
printf "  out dir: %s\n" "$OUT_DIR"
printf "  flash mode: %s\n" "$DEVICE_FLASH_MODE"
printf "  device args:"
if [[ ${#DEVICE_ARGS[@]} -eq 0 ]]; then
  printf " (none)\n"
else
  printf " %s\n" "${DEVICE_ARGS[*]}"
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo ""
fi

if ! run_step "firmware_build" "cd '$ROOT_DIR' && pio run" "$BUILD_LOG" "$BUILD_LOG"; then
  true
elif ! run_step "sd_lock_contract" "cd '$ROOT_DIR' && python3 scripts/check_sd_lock_discipline_contract.py" "$CONTRACT_LOG" "$CONTRACT_LOG"; then
  true
elif ! run_step "parser_native_smoke" "cd '$ROOT_DIR' && pio test -e native -f test_ble_display_pipeline -f test_packet_parser_stream" "$NATIVE_LOG" "$NATIVE_LOG"; then
  true
elif ! run_step "device_suite" "cd '$ROOT_DIR' && $(printf '%q ' "${DEVICE_CMD[@]}")" "$DEVICE_LOG" "$DEVICE_OUT_DIR/summary.md"; then
  true
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  cat > "$SUMMARY_MD" <<EOF_SUM
# Iron Gate Summary

Status: DRY_RUN

No checks executed.

Outputs:
- Results TSV: $RESULTS_TSV
- Firmware log: $BUILD_LOG
- Contract log: $CONTRACT_LOG
- Native log: $NATIVE_LOG
- Device log: $DEVICE_LOG
EOF_SUM
  echo "Dry run complete. Summary: $SUMMARY_MD"
  exit 0
fi

overall="PASS"
if awk -F '\t' 'NR > 1 && $2 == "FAIL" { found=1 } END { exit found ? 0 : 1 }' "$RESULTS_TSV"; then
  overall="FAIL"
fi

pass_count="$(awk -F '\t' 'NR > 1 && $2 == "PASS" { c++ } END { print c + 0 }' "$RESULTS_TSV")"
fail_count="$(awk -F '\t' 'NR > 1 && $2 == "FAIL" { c++ } END { print c + 0 }' "$RESULTS_TSV")"

{
  echo "# Iron Gate Summary"
  echo ""
  echo "Status: $overall"
  echo ""
  echo "True PASS point requires all steps PASS:"
  echo "1. firmware_build"
  echo "2. sd_lock_contract"
  echo "3. parser_native_smoke"
  echo "4. device_suite"
  echo ""
  echo "Counts: PASS=$pass_count FAIL=$fail_count"
  echo ""
  echo "## Artifacts"
  echo "- Results TSV: $RESULTS_TSV"
  echo "- Firmware log: $BUILD_LOG"
  echo "- Contract log: $CONTRACT_LOG"
  echo "- Native log: $NATIVE_LOG"
  echo "- Device log: $DEVICE_LOG"
  echo "- Device summary: $DEVICE_OUT_DIR/summary.md"
} > "$SUMMARY_MD"

if [[ "$overall" == "PASS" ]]; then
  echo "Iron gate PASS. Summary: $SUMMARY_MD"
  exit 0
fi

echo "Iron gate FAIL. Summary: $SUMMARY_MD" >&2
exit 1
