#!/usr/bin/env bash
#
# run_device_soak.sh - Run repeated hardware device test cycles and collect
# stability/resource metrics from real board logs.
#
# Usage examples:
#   ./scripts/run_device_soak.sh --cycles 20
#   ./scripts/run_device_soak.sh --cycles 50 --cooldown-seconds 6
#   ./scripts/run_device_soak.sh --cycles 10 --quick --stop-on-fail
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

CYCLES=20
STOP_ON_FAIL=0
RUN_ARGS=()
OUT_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --cycles)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --cycles" >&2
        exit 2
      fi
      CYCLES="$2"
      shift
      ;;
    --quick|--full|--stress)
      RUN_ARGS+=("$1")
      ;;
    --cooldown-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --cooldown-seconds" >&2
        exit 2
      fi
      RUN_ARGS+=("$1" "$2")
      shift
      ;;
    --stop-on-fail)
      STOP_ON_FAIL=1
      ;;
    --out-dir)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --out-dir" >&2
        exit 2
      fi
      OUT_DIR="$2"
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [options]"
      echo ""
      echo "Options:"
      echo "  --cycles N             Number of soak cycles to run (default: 20)"
      echo "  --quick                Pass-through to run_device_tests.sh"
      echo "  --full                 Pass-through to run_device_tests.sh"
      echo "  --stress               Pass-through to run_device_tests.sh"
      echo "  --cooldown-seconds N   Pass-through to run_device_tests.sh"
      echo "  --stop-on-fail         Stop immediately on first failed cycle"
      echo "  --out-dir PATH         Write soak artifacts to PATH"
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      echo "Usage: $0 [--cycles N] [--quick|--full] [--stress] [--cooldown-seconds N] [--stop-on-fail] [--out-dir PATH]" >&2
      exit 2
      ;;
  esac
  shift
done

if ! [[ "$CYCLES" =~ ^[0-9]+$ ]] || [[ "$CYCLES" -lt 1 ]]; then
  echo "Invalid --cycles value '$CYCLES' (expected positive integer)." >&2
  exit 2
fi

if [[ ! -x "$ROOT_DIR/scripts/run_device_tests.sh" ]]; then
  echo "Missing executable: $ROOT_DIR/scripts/run_device_tests.sh" >&2
  exit 1
fi

if [[ -z "$OUT_DIR" ]]; then
  timestamp="$(date +%Y%m%d_%H%M%S)"
  OUT_DIR="$ROOT_DIR/.artifacts/test_reports/device_soak_$timestamp"
fi
mkdir -p "$OUT_DIR"

SOAK_LOG="$OUT_DIR/soak.log"
CSV_PATH="$OUT_DIR/cycles.csv"
SUMMARY_MD="$OUT_DIR/summary.md"
RESET_REASON_RAW="$OUT_DIR/reset_reasons_raw.txt"

: > "$SOAK_LOG"
: > "$RESET_REASON_RAW"
printf "cycle,start_utc,end_utc,duration_s,status,exit_code,failed_suite,report_dir,min_free_heap,wdt_or_panic_count,reset_line_count,event_bus_published,event_bus_consumed,event_bus_dropped,event_bus_dual_published,event_bus_dual_dropped\n" > "$CSV_PATH"

extract_min_free_heap() {
  local log_path="$1"
  grep -oE 'free_heap=[0-9]+' "$log_path" \
    | awk -F= 'NR == 1 || $2 < min { min = $2 } END { if (NR > 0) print min }'
}

extract_wdt_or_panic_count() {
  local log_path="$1"
  grep -Eic 'task watchdog|task_wdt|Guru Meditation|panic|abort\(' "$log_path" || true
}

extract_reset_line_count() {
  local log_path="$1"
  grep -c 'rst:0x' "$log_path" || true
}

extract_event_bus_main_totals() {
  local log_path="$1"
  awk '
    /\[bus\] published=[0-9]+ consumed=[0-9]+ dropped=[0-9]+/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^published=/) { split($i, a, "="); pub += a[2] + 0; }
        if ($i ~ /^consumed=/)  { split($i, a, "="); con += a[2] + 0; }
        if ($i ~ /^dropped=/)   { split($i, a, "="); drp += a[2] + 0; }
      }
    }
    END { printf "%d %d %d\n", pub + 0, con + 0, drp + 0; }
  ' "$log_path"
}

extract_event_bus_dual_totals() {
  local log_path="$1"
  awk '
    /\[bus\] dual-producer: published=[0-9]+ dropped=[0-9]+/ {
      for (i = 1; i <= NF; i++) {
        if ($i ~ /^published=/) { split($i, a, "="); pub += a[2] + 0; }
        if ($i ~ /^dropped=/)   { split($i, a, "="); drp += a[2] + 0; }
      }
    }
    END { printf "%d %d\n", pub + 0, drp + 0; }
  ' "$log_path"
}

SOAK_START_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
SOAK_START_EPOCH="$(date +%s)"

pass_cycles=0
fail_cycles=0
completed_cycles=0
total_duration_s=0
total_wdt_or_panic=0
total_reset_lines=0
total_event_pub=0
total_event_con=0
total_event_drop=0
total_event_dual_pub=0
total_event_dual_drop=0
global_min_heap=""
global_min_heap_cycle=""

echo "==> Starting device soak run"
echo "    cycles: $CYCLES"
echo "    run args: ${RUN_ARGS[*]:-(none)}"
echo "    out dir: $OUT_DIR"
echo ""

for cycle in $(seq 1 "$CYCLES"); do
  cycle_start_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  cycle_start_epoch="$(date +%s)"
  cycle_log="$OUT_DIR/cycle_${cycle}.log"

  echo "==> [cycle $cycle/$CYCLES] starting at $cycle_start_utc"

  set +e
  ./scripts/run_device_tests.sh "${RUN_ARGS[@]}" 2>&1 | tee "$cycle_log"
  cmd_status=${PIPESTATUS[0]}
  set -e

  cat "$cycle_log" >> "$SOAK_LOG"

  cycle_end_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  cycle_end_epoch="$(date +%s)"
  cycle_duration_s=$((cycle_end_epoch - cycle_start_epoch))
  total_duration_s=$((total_duration_s + cycle_duration_s))
  completed_cycles=$((completed_cycles + 1))

  report_dir="$(awk -F'Reports written to: ' '/Reports written to:/ { path=$2 } END { print path }' "$cycle_log" | xargs)"
  failed_suite="$(awk -F'Device test run stopped at: ' '/Device test run stopped at:/ { suite=$2 } END { print suite }' "$cycle_log" | xargs)"

  metrics_log="$cycle_log"
  if [[ -n "$report_dir" && -f "$report_dir/device.log" ]]; then
    metrics_log="$report_dir/device.log"
  fi

  min_free_heap="$(extract_min_free_heap "$metrics_log")"
  wdt_or_panic_count="$(extract_wdt_or_panic_count "$metrics_log")"
  reset_line_count="$(extract_reset_line_count "$metrics_log")"
  read -r event_pub event_con event_drop <<<"$(extract_event_bus_main_totals "$metrics_log")"
  read -r event_dual_pub event_dual_drop <<<"$(extract_event_bus_dual_totals "$metrics_log")"

  if [[ -n "$min_free_heap" ]]; then
    if [[ -z "$global_min_heap" || "$min_free_heap" -lt "$global_min_heap" ]]; then
      global_min_heap="$min_free_heap"
      global_min_heap_cycle="$cycle"
    fi
  fi

  total_wdt_or_panic=$((total_wdt_or_panic + wdt_or_panic_count))
  total_reset_lines=$((total_reset_lines + reset_line_count))
  total_event_pub=$((total_event_pub + event_pub))
  total_event_con=$((total_event_con + event_con))
  total_event_drop=$((total_event_drop + event_drop))
  total_event_dual_pub=$((total_event_dual_pub + event_dual_pub))
  total_event_dual_drop=$((total_event_dual_drop + event_dual_drop))

  grep -oE 'rst:0x[0-9a-fA-F]+' "$metrics_log" >> "$RESET_REASON_RAW" || true

  status_word="PASS"
  if [[ "$cmd_status" -eq 0 ]]; then
    pass_cycles=$((pass_cycles + 1))
  else
    status_word="FAIL"
    fail_cycles=$((fail_cycles + 1))
  fi

  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
    "$cycle" \
    "$cycle_start_utc" \
    "$cycle_end_utc" \
    "$cycle_duration_s" \
    "$status_word" \
    "$cmd_status" \
    "$failed_suite" \
    "$report_dir" \
    "$min_free_heap" \
    "$wdt_or_panic_count" \
    "$reset_line_count" \
    "$event_pub" \
    "$event_con" \
    "$event_drop" \
    "$event_dual_pub" \
    "$event_dual_drop" >> "$CSV_PATH"

  echo "==> [cycle $cycle/$CYCLES] $status_word exit=$cmd_status duration=${cycle_duration_s}s min_heap=${min_free_heap:-n/a} wdt_or_panic=$wdt_or_panic_count"
  echo ""

  if [[ "$cmd_status" -ne 0 && "$STOP_ON_FAIL" -eq 1 ]]; then
    echo "Stopping soak early due to --stop-on-fail." >&2
    break
  fi
done

SOAK_END_UTC="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
SOAK_END_EPOCH="$(date +%s)"
SOAK_ELAPSED_S=$((SOAK_END_EPOCH - SOAK_START_EPOCH))

failure_rate_pct="$(awk -v total="$completed_cycles" -v failed="$fail_cycles" 'BEGIN { if (total == 0) printf "0.00"; else printf "%.2f", (failed * 100.0) / total }')"
pass_rate_pct="$(awk -v total="$completed_cycles" -v passed="$pass_cycles" 'BEGIN { if (total == 0) printf "0.00"; else printf "%.2f", (passed * 100.0) / total }')"

{
  echo "# Device Soak Summary"
  echo ""
  echo "- Start (UTC): $SOAK_START_UTC"
  echo "- End (UTC): $SOAK_END_UTC"
  echo "- Requested cycles: $CYCLES"
  echo "- Completed cycles: $completed_cycles"
  echo "- Passed cycles: $pass_cycles ($pass_rate_pct%)"
  echo "- Failed cycles: $fail_cycles ($failure_rate_pct%)"
  echo "- Total runtime (wall): ${SOAK_ELAPSED_S}s"
  echo "- Aggregate cycle runtime: ${total_duration_s}s"
  echo "- Watchdog/panic signature count: $total_wdt_or_panic"
  echo "- Reset-line count: $total_reset_lines"
  if [[ -n "$global_min_heap" ]]; then
    echo "- Lowest observed free heap: $global_min_heap (cycle $global_min_heap_cycle)"
  else
    echo "- Lowest observed free heap: n/a"
  fi
  echo "- Event bus totals: published=$total_event_pub consumed=$total_event_con dropped=$total_event_drop"
  echo "- Event bus dual-producer totals: published=$total_event_dual_pub dropped=$total_event_dual_drop"
  echo ""
  echo "## Failed Cycles"
  echo ""
  if [[ "$fail_cycles" -eq 0 ]]; then
    echo "None."
  else
    echo "| Cycle | Exit | Failed Suite | Report Dir |"
    echo "|------:|-----:|--------------|------------|"
    awk -F, 'NR > 1 && $5 == "FAIL" { printf "| %s | %s | %s | %s |\n", $1, $6, ($7 == "" ? "-" : $7), ($8 == "" ? "-" : $8) }' "$CSV_PATH"
  fi
  echo ""
  echo "## Reset Reasons"
  echo ""
  if [[ ! -s "$RESET_REASON_RAW" ]]; then
    echo "No reset lines captured."
  else
    echo "| Reason | Count |"
    echo "|--------|------:|"
    sort "$RESET_REASON_RAW" | uniq -c | sort -nr | awk '{ printf "| `%s` | %s |\n", $2, $1 }'
  fi
  echo ""
  echo "## Artifacts"
  echo ""
  echo "- Cycle CSV: \`$CSV_PATH\`"
  echo "- Soak log: \`$SOAK_LOG\`"
} > "$SUMMARY_MD"

echo "==> Soak complete"
echo "    completed: $completed_cycles / $CYCLES"
echo "    pass: $pass_cycles"
echo "    fail: $fail_cycles"
echo "    failure rate: ${failure_rate_pct}%"
echo "    summary: $SUMMARY_MD"
echo "    csv: $CSV_PATH"

if [[ "$fail_cycles" -gt 0 ]]; then
  exit 1
fi
