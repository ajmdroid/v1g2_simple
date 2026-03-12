#!/usr/bin/env bash
#
# iron-gate.sh - Single-command local ship gate for trusted repo checks only.
#
# True PASS point:
#   1) firmware_build = PASS
#   2) sd_lock_contract = PASS
#   3) parser_native_smoke = PASS
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

OUT_DIR=""
DRY_RUN=0

usage() {
  cat <<'USAGE'
Usage: ./scripts/iron-gate.sh [options]

Options:
  --out-dir PATH               Write artifacts to PATH
  --dry-run                    Print resolved commands and exit
  -h, --help                   Show help
Examples:
  ./scripts/iron-gate.sh
  ./scripts/iron-gate.sh --out-dir .artifacts/iron_gate/manual
USAGE
}

is_uint() {
  local value="${1:-}"
  [[ "$value" =~ ^[0-9]+$ ]]
}

while [[ $# -gt 0 ]]; do
  case "$1" in
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
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
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

printf "Iron Gate Config:\n"
printf "  out dir: %s\n" "$OUT_DIR"

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo ""
fi

if ! run_step "firmware_build" "cd '$ROOT_DIR' && pio run" "$BUILD_LOG" "$BUILD_LOG"; then
  true
elif ! run_step "sd_lock_contract" "cd '$ROOT_DIR' && python3 scripts/check_sd_lock_discipline_contract.py" "$CONTRACT_LOG" "$CONTRACT_LOG"; then
  true
elif ! run_step "parser_native_smoke" "cd '$ROOT_DIR' && pio test -e native -f test_ble_display_pipeline -f test_packet_parser_stream" "$NATIVE_LOG" "$NATIVE_LOG"; then
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
  echo ""
  echo "Counts: PASS=$pass_count FAIL=$fail_count"
  echo ""
  echo "## Artifacts"
  echo "- Results TSV: $RESULTS_TSV"
  echo "- Firmware log: $BUILD_LOG"
  echo "- Contract log: $CONTRACT_LOG"
  echo "- Native log: $NATIVE_LOG"
} > "$SUMMARY_MD"

if [[ "$overall" == "PASS" ]]; then
  echo "Iron gate PASS. Summary: $SUMMARY_MD"
  exit 0
fi

echo "Iron gate FAIL. Summary: $SUMMARY_MD" >&2
exit 1
