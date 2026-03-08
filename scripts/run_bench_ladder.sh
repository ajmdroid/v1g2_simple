#!/usr/bin/env bash
#
# run_bench_ladder.sh - Authoritative bench stress ladder runner with
# fail classification (A/B/C) and stop-policy enforcement.
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

LEVEL_NAMES=(L0 L1 L2 L3 L4 L5 L6 L7 L8)
LEVEL_RAD=(200 250 300 350 400 500 650 800 1000)
LEVEL_INTERVAL=(15 12 10 8 7 6 5 4 3)

DURATION_SECONDS=240
RUNS_PER_LEVEL=2
START_LEVEL="L0"
END_LEVEL="L8"
METRICS_URL=""
FLASH_MODE="--skip-flash"
OUT_DIR=""
DRY_RUN=0
DEVICE_ARGS=()

usage() {
  cat <<'EOF'
Usage: ./scripts/run_bench_ladder.sh [options] [-- <device-test extra args>]

Options:
  --duration-seconds N     Soak duration for each ladder run (default: 240)
  --runs-per-level N       Runs per level (default: 2; authoritative policy)
  --start-level LX         First level to run (default: L0)
  --end-level LX           Last level to run (default: L8)
  --metrics-url URL        Override metrics URL forwarded to device-test.sh
  --skip-flash             Skip flash for each level run (default)
  --with-flash             Flash in each level run
  --out-dir PATH           Campaign output directory
  --dry-run                Print resolved commands and exit
  -h, --help               Show this help

Examples:
  ./scripts/run_bench_ladder.sh
  ./scripts/run_bench_ladder.sh --start-level L6 --end-level L8
  ./scripts/run_bench_ladder.sh -- --no-auto-kill-monitor
EOF
}

is_uint() {
  local value="${1:-}"
  [[ "$value" =~ ^[0-9]+$ ]]
}

level_index() {
  local target="$1"
  local i
  for i in "${!LEVEL_NAMES[@]}"; do
    if [[ "${LEVEL_NAMES[$i]}" == "$target" ]]; then
      echo "$i"
      return 0
    fi
  done
  return 1
}

sanitize_field() {
  printf "%s" "$1" | tr '\t\r\n' '   '
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --duration-seconds" >&2; exit 2; }
      DURATION_SECONDS="$2"
      shift
      ;;
    --runs-per-level)
      [[ $# -lt 2 ]] && { echo "Missing value for --runs-per-level" >&2; exit 2; }
      RUNS_PER_LEVEL="$2"
      shift
      ;;
    --start-level)
      [[ $# -lt 2 ]] && { echo "Missing value for --start-level" >&2; exit 2; }
      START_LEVEL="$2"
      shift
      ;;
    --end-level)
      [[ $# -lt 2 ]] && { echo "Missing value for --end-level" >&2; exit 2; }
      END_LEVEL="$2"
      shift
      ;;
    --metrics-url)
      [[ $# -lt 2 ]] && { echo "Missing value for --metrics-url" >&2; exit 2; }
      METRICS_URL="$2"
      shift
      ;;
    --skip-flash)
      FLASH_MODE="--skip-flash"
      ;;
    --with-flash)
      FLASH_MODE="--with-flash"
      ;;
    --out-dir)
      [[ $# -lt 2 ]] && { echo "Missing value for --out-dir" >&2; exit 2; }
      OUT_DIR="$2"
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      ;;
    --)
      shift
      while [[ $# -gt 0 ]]; do
        DEVICE_ARGS+=("$1")
        shift
      done
      break
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

if ! is_uint "$DURATION_SECONDS" || [[ "$DURATION_SECONDS" -lt 1 ]]; then
  echo "Invalid --duration-seconds '$DURATION_SECONDS' (expected >= 1)." >&2
  exit 2
fi
if ! is_uint "$RUNS_PER_LEVEL" || [[ "$RUNS_PER_LEVEL" -lt 1 ]]; then
  echo "Invalid --runs-per-level '$RUNS_PER_LEVEL' (expected >= 1)." >&2
  exit 2
fi

START_INDEX="$(level_index "$START_LEVEL" || true)"
END_INDEX="$(level_index "$END_LEVEL" || true)"
if [[ -z "$START_INDEX" || -z "$END_INDEX" ]]; then
  echo "Invalid level range. Valid levels: ${LEVEL_NAMES[*]}" >&2
  exit 2
fi
if (( START_INDEX > END_INDEX )); then
  echo "Invalid level range: $START_LEVEL is after $END_LEVEL." >&2
  exit 2
fi

if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="$ROOT_DIR/.artifacts/test_reports/bench_ladder_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$OUT_DIR"

RUNS_TSV="$OUT_DIR/ladder_runs.tsv"
SUMMARY_MD="$OUT_DIR/ladder_summary.md"
printf "level\trun\tstatus\tclass\treason\tsummary\tcommand\n" > "$RUNS_TSV"

echo "Bench Ladder Config:"
echo "  out dir: $OUT_DIR"
echo "  levels: $START_LEVEL .. $END_LEVEL"
echo "  runs per level: $RUNS_PER_LEVEL"
echo "  duration seconds: $DURATION_SECONDS"
echo "  flash mode: $FLASH_MODE"
echo "  metrics url: ${METRICS_URL:-default}"
echo "  dry run: $DRY_RUN"
if [[ "${#DEVICE_ARGS[@]}" -gt 0 ]]; then
  echo "  extra device args: ${DEVICE_ARGS[*]}"
fi
if [[ "$RUNS_PER_LEVEL" -ne 2 ]]; then
  echo "  WARNING: authoritative ladder policy expects --runs-per-level 2"
fi

highest_cleared=""
highest_cleared_index=-1
first_fail_level=""
first_fail_class=""
first_fail_reason=""
first_sustained_b_level=""
first_sustained_b_reason=""
class_a_level=""
class_a_reason=""
stop_reason=""
stopped=0

for (( level_i=START_INDEX; level_i<=END_INDEX; level_i++ )); do
  level="${LEVEL_NAMES[$level_i]}"
  rad="${LEVEL_RAD[$level_i]}"
  interval="${LEVEL_INTERVAL[$level_i]}"
  level_passes=0
  level_fails=0
  level_first_fail_class=""
  level_first_fail_reason=""

  echo ""
  echo "==> $level (rad=$rad interval=${interval}s)"

  for (( run_i=1; run_i<=RUNS_PER_LEVEL; run_i++ )); do
    run_dir="$OUT_DIR/${level}_run${run_i}"
    mkdir -p "$run_dir"
    run_log="$run_dir/runner.log"
    summary_path="$run_dir/summary.md"
    cmd=(
      "./scripts/device-test.sh"
      "$FLASH_MODE"
      "--duration-seconds" "$DURATION_SECONDS"
      "--rad-duration-scale-pct" "$rad"
      "--transition-drive-interval-seconds" "$interval"
      "--out-dir" "$run_dir"
    )
    if [[ -n "$METRICS_URL" ]]; then
      cmd+=(--metrics-url "$METRICS_URL")
    fi
    if [[ "${#DEVICE_ARGS[@]}" -gt 0 ]]; then
      cmd+=("${DEVICE_ARGS[@]}")
    fi
    cmd_text="$(printf '%q ' "${cmd[@]}")"

    status="PASS"
    klass="NONE"
    reason="none"
    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "  [dry-run] run $run_i: $cmd_text"
    else
      echo "  run $run_i/$RUNS_PER_LEVEL"
      set +e
      "${cmd[@]}" >"$run_log" 2>&1
      rc=$?
      set -e
      if [[ "$rc" -ne 0 ]]; then
        status="FAIL"
        classify_out="$(python3 "$ROOT_DIR/tools/classify_device_test_failure.py" "$run_dir" || true)"
        klass="$(printf "%s\n" "$classify_out" | awk -F= '/^class=/{print $2; exit}')"
        reason="$(printf "%s\n" "$classify_out" | sed -n 's/^reason=//p' | head -n1)"
        if [[ -z "$klass" ]]; then
          klass="B"
        fi
        if [[ -z "$reason" ]]; then
          reason="classification unavailable"
        fi
      fi
    fi

    if [[ "$status" == "PASS" ]]; then
      level_passes=$((level_passes + 1))
      echo "    PASS"
    else
      level_fails=$((level_fails + 1))
      echo "    FAIL class=$klass reason=$reason"
      if [[ -z "$first_fail_level" ]]; then
        first_fail_level="$level"
        first_fail_class="$klass"
        first_fail_reason="$reason"
      fi

      if [[ "$level_fails" -eq 1 ]]; then
        level_first_fail_class="$klass"
        level_first_fail_reason="$reason"
        if [[ "$klass" == "A" ]]; then
          class_a_level="$level"
          class_a_reason="$reason"
          stop_reason="Class A at ${level} run ${run_i}: ${reason}"
          stopped=1
        fi
      else
        sustained_class="$klass"
        sustained_reason="$reason"
        if [[ "$level_first_fail_class" == "C" ]]; then
          sustained_class="B"
          sustained_reason="reclassified C->B after repeated failure: ${reason}"
        fi
        if [[ "$sustained_class" == "A" ]]; then
          class_a_level="$level"
          class_a_reason="$sustained_reason"
          stop_reason="Class A sustained at ${level}: ${sustained_reason}"
        else
          if [[ -z "$first_sustained_b_level" ]]; then
            first_sustained_b_level="$level"
            first_sustained_b_reason="$sustained_reason"
          fi
          stop_reason="Sustained Class ${sustained_class} at ${level}: ${sustained_reason}"
        fi
        stopped=1
      fi
    fi

    printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
      "$level" \
      "$run_i" \
      "$status" \
      "$klass" \
      "$(sanitize_field "$reason")" \
      "$summary_path" \
      "$(sanitize_field "$cmd_text")" >> "$RUNS_TSV"

    if [[ "$stopped" -eq 1 ]]; then
      break
    fi
  done

  if [[ "$level_passes" -eq "$RUNS_PER_LEVEL" ]]; then
    highest_cleared="$level"
    highest_cleared_index="$level_i"
  fi

  if [[ "$stopped" -eq 1 ]]; then
    break
  fi
done

l6_index="$(level_index "L6")"
class_a_index=-1
if [[ -n "$class_a_level" ]]; then
  class_a_index="$(level_index "$class_a_level")"
fi
sustained_b_index=-1
if [[ -n "$first_sustained_b_level" ]]; then
  sustained_b_index="$(level_index "$first_sustained_b_level")"
fi

acceptance="NOT_ACCEPTABLE"
acceptance_reason=""
if [[ "$class_a_index" -ge 0 ]]; then
  acceptance="NOT_ACCEPTABLE"
  acceptance_reason="Class A reliability failure encountered at ${class_a_level}."
elif [[ "$highest_cleared_index" -lt "$l6_index" ]]; then
  acceptance="NOT_ACCEPTABLE"
  acceptance_reason="L6 not cleared 2/2."
elif [[ "$sustained_b_index" -ge 0 ]]; then
  if [[ "$sustained_b_index" -ge 7 ]]; then
    acceptance="ACCEPTABLE"
    acceptance_reason="First sustained Class B at ${first_sustained_b_level} (>=L7)."
  else
    acceptance="NOT_ACCEPTABLE"
    acceptance_reason="First sustained Class B at ${first_sustained_b_level} (<=L6)."
  fi
else
  acceptance="ACCEPTABLE"
  acceptance_reason="L6 cleared and no sustained Class B/A failures."
fi

{
  echo "# Bench Stress Ladder Summary"
  echo ""
  echo "- Result: **$acceptance**"
  echo "- Reason: $acceptance_reason"
  echo "- Timestamp (UTC): $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  echo "- Levels executed: $START_LEVEL .. $END_LEVEL"
  echo "- Runs per level: $RUNS_PER_LEVEL"
  echo "- Duration per run (s): $DURATION_SECONDS"
  echo "- Flash mode: $FLASH_MODE"
  echo "- Highest cleared level: ${highest_cleared:-none}"
  echo "- First failing level: ${first_fail_level:-none}"
  echo "- First failing class: ${first_fail_class:-none}"
  echo "- First sustained Class B level: ${first_sustained_b_level:-none}"
  echo "- Class A level: ${class_a_level:-none}"
  if [[ -n "$stop_reason" ]]; then
    echo "- Ladder stop reason: $stop_reason"
  fi
  echo ""
  echo "## Run Results"
  echo ""
  echo "| Level | Run | Status | Class | Reason | Summary |"
  echo "|---|---:|---|---|---|---|"
  awk -F '\t' 'NR>1 {printf "| %s | %s | %s | %s | %s | `%s` |\n", $1, $2, $3, $4, $5, $6}' "$RUNS_TSV"
  echo ""
  echo "## Artifacts"
  echo ""
  echo "- Ladder summary: \`$SUMMARY_MD\`"
  echo "- Run TSV: \`$RUNS_TSV\`"
} > "$SUMMARY_MD"

echo ""
echo "Ladder summary: $SUMMARY_MD"
echo "Run TSV: $RUNS_TSV"
echo "Acceptance: $acceptance"
echo "Reason: $acceptance_reason"

if [[ "$acceptance" != "ACCEPTABLE" ]]; then
  exit 1
fi
exit 0
