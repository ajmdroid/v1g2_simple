#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"

BOARD_ID="${HARDWARE_TEST_BOARD_ID:-release}"
INVENTORY_PATH="${HARDWARE_TEST_INVENTORY:-$ROOT_DIR/test/device/board_inventory.json}"
DEVICE_TESTS_SCRIPT="${HARDWARE_TEST_DEVICE_TESTS_SCRIPT:-$ROOT_DIR/scripts/run_device_tests.sh}"
REAL_SOAK_SCRIPT="${HARDWARE_TEST_REAL_SOAK_SCRIPT:-$ROOT_DIR/scripts/run_real_fw_soak.sh}"
IMPORT_DRIVE_LOG_SCRIPT="${HARDWARE_TEST_IMPORT_DRIVE_LOG_SCRIPT:-$ROOT_DIR/tools/import_drive_log.py}"
ASSEMBLE_RESULT_SCRIPT="${HARDWARE_TEST_ASSEMBLE_RESULT_SCRIPT:-$ROOT_DIR/tools/assemble_hardware_test_result.py}"
ARTIFACT_ROOT="${HARDWARE_TEST_ARTIFACT_ROOT:-$ROOT_DIR/.artifacts/hardware/test}"
SOAK_DURATION_SECONDS="${HARDWARE_TEST_SOAK_DURATION_SECONDS:-300}"
STRICT_WARNINGS="${HARDWARE_TEST_STRICT_WARNINGS:-0}"

SELECTED_EXPLICITLY=0
RUN_DEVICE=0
RUN_CORE=0
RUN_DISPLAY=0
PARSE_DRIVE_LOG=""

usage() {
  cat <<'EOF'
Usage: ./scripts/hardware/test.sh [options]

Options:
  -a, --all                 Run the full fixed test suite (default)
  -d, --device              Run only the device suite step
  -c, --core                Run only the core soak step
  -p, --display             Run only the display-preview soak step
  --parse-drive-log PATH    Parse a saved soak directory or metrics.jsonl instead of
                            running a live soak step. If no soak step is selected,
                            the script infers core/display from the input manifest
                            and otherwise defaults to core.
  --board-id ID             Board id from test/device/board_inventory.json
                            (default: release)
  --duration-seconds N      Soak duration for live soak steps (default: 300)
  --artifact-root PATH      Base output root (default: .artifacts/hardware/test);
                            runs are stored under <artifact-root>/<board-id>/
  --strict                  Treat PASS_WITH_WARNINGS as a failing exit
  -h, --help                Show this help

Examples:
  ./scripts/hardware/test.sh
  ./scripts/hardware/test.sh --core
  ./scripts/hardware/test.sh --display
  ./scripts/hardware/test.sh --all --board-id release --strict
  ./scripts/hardware/test.sh --device --parse-drive-log /path/to/metrics.jsonl
  ./scripts/hardware/test.sh --parse-drive-log /path/to/real_fw_soak_run
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all|-a)
      SELECTED_EXPLICITLY=1
      RUN_DEVICE=1
      RUN_CORE=1
      RUN_DISPLAY=1
      ;;
    --device|-d)
      SELECTED_EXPLICITLY=1
      RUN_DEVICE=1
      ;;
    --core|-c)
      SELECTED_EXPLICITLY=1
      RUN_CORE=1
      ;;
    --display|-p)
      SELECTED_EXPLICITLY=1
      RUN_DISPLAY=1
      ;;
    --parse-drive-log)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --parse-drive-log" >&2
        exit 2
      fi
      PARSE_DRIVE_LOG="$2"
      shift
      ;;
    --board-id)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --board-id" >&2
        exit 2
      fi
      BOARD_ID="$2"
      shift
      ;;
    --duration-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --duration-seconds" >&2
        exit 2
      fi
      SOAK_DURATION_SECONDS="$2"
      shift
      ;;
    --artifact-root)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --artifact-root" >&2
        exit 2
      fi
      ARTIFACT_ROOT="$2"
      shift
      ;;
    --strict)
      STRICT_WARNINGS=1
      ;;
    --help|-h)
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

if ! [[ "$SOAK_DURATION_SECONDS" =~ ^[0-9]+$ ]] || [[ "$SOAK_DURATION_SECONDS" -lt 1 ]]; then
  echo "Invalid --duration-seconds value '$SOAK_DURATION_SECONDS' (expected positive integer)." >&2
  exit 2
fi

infer_parse_step() {
  local input_path="$1"
  python3 - "$input_path" <<'PY'
import json
import sys
from pathlib import Path

source = Path(sys.argv[1]).resolve()
manifest = None
if source.is_dir():
    candidate = source / "manifest.json"
    if candidate.exists():
        manifest = candidate
elif source.is_file():
    candidate = source.parent / "manifest.json"
    if candidate.exists():
        manifest = candidate

if manifest is None:
    print("core_soak")
    raise SystemExit(0)

payload = json.loads(manifest.read_text(encoding="utf-8"))
stress = str(payload.get("stress_class") or "")
if stress == "display_preview":
    print("display_soak")
else:
    print("core_soak")
PY
}

if [[ "$SELECTED_EXPLICITLY" -eq 0 ]]; then
  RUN_DEVICE=1
  RUN_CORE=1
  RUN_DISPLAY=1
fi

PARSE_STEP=""
if [[ -n "$PARSE_DRIVE_LOG" ]]; then
  inferred_parse_step="$(infer_parse_step "$PARSE_DRIVE_LOG")"
  selected_soak_count=$((RUN_CORE + RUN_DISPLAY))
  if [[ "$selected_soak_count" -gt 1 ]]; then
    echo "--parse-drive-log can target only one soak step at a time." >&2
    echo "Use either --core or --display (or omit both to auto-infer)." >&2
    exit 2
  fi
  if [[ "$selected_soak_count" -eq 0 ]]; then
    if [[ "$inferred_parse_step" == "display_soak" ]]; then
      RUN_DISPLAY=1
    else
      RUN_CORE=1
    fi
  fi
  if [[ "$RUN_DISPLAY" -eq 1 ]]; then
    PARSE_STEP="display_soak"
  else
    PARSE_STEP="core_soak"
  fi
fi

if [[ "$RUN_DEVICE" -eq 0 && "$RUN_CORE" -eq 0 && "$RUN_DISPLAY" -eq 0 ]]; then
  echo "No test steps selected." >&2
  exit 2
fi

NEEDS_LIVE_DEVICE="$RUN_DEVICE"
NEEDS_LIVE_CORE=0
NEEDS_LIVE_DISPLAY=0
if [[ "$RUN_CORE" -eq 1 && "$PARSE_STEP" != "core_soak" ]]; then
  NEEDS_LIVE_CORE=1
fi
if [[ "$RUN_DISPLAY" -eq 1 && "$PARSE_STEP" != "display_soak" ]]; then
  NEEDS_LIVE_DISPLAY=1
fi
NEEDS_LIVE_BOARD=0
if [[ "$NEEDS_LIVE_DEVICE" -eq 1 || "$NEEDS_LIVE_CORE" -eq 1 || "$NEEDS_LIVE_DISPLAY" -eq 1 ]]; then
  NEEDS_LIVE_BOARD=1
fi

if [[ "$NEEDS_LIVE_DEVICE" -eq 1 && ! -x "$DEVICE_TESTS_SCRIPT" ]]; then
  echo "Missing executable: $DEVICE_TESTS_SCRIPT" >&2
  exit 2
fi
if [[ ( "$NEEDS_LIVE_CORE" -eq 1 || "$NEEDS_LIVE_DISPLAY" -eq 1 ) && ! -x "$REAL_SOAK_SCRIPT" ]]; then
  echo "Missing executable: $REAL_SOAK_SCRIPT" >&2
  exit 2
fi
if [[ -n "$PARSE_DRIVE_LOG" && ! -f "$IMPORT_DRIVE_LOG_SCRIPT" ]]; then
  echo "Missing import helper: $IMPORT_DRIVE_LOG_SCRIPT" >&2
  exit 2
fi
if [[ ! -f "$ASSEMBLE_RESULT_SCRIPT" ]]; then
  echo "Missing assemble helper: $ASSEMBLE_RESULT_SCRIPT" >&2
  exit 2
fi

DEVICE_PORT=""
METRICS_URL=""
if [[ "$NEEDS_LIVE_BOARD" -eq 1 ]]; then
  if [[ ! -f "$INVENTORY_PATH" ]]; then
    echo "Board inventory not found: $INVENTORY_PATH" >&2
    exit 2
  fi

  BOARD_INFO="$(python3 - "$INVENTORY_PATH" "$BOARD_ID" <<'PY'
import json
import sys
from pathlib import Path

inventory_path = Path(sys.argv[1])
board_id = sys.argv[2]
payload = json.loads(inventory_path.read_text(encoding="utf-8"))
for board in payload.get("boards", []):
    if board.get("board_id") == board_id:
        print(board.get("device_path", ""))
        print(board.get("metrics_url", ""))
        raise SystemExit(0)
raise SystemExit(1)
PY
)" || {
    echo "Board '$BOARD_ID' not found in inventory." >&2
    exit 2
  }

  DEVICE_PORT="$(printf "%s\n" "$BOARD_INFO" | sed -n '1p')"
  METRICS_URL="$(printf "%s\n" "$BOARD_INFO" | sed -n '2p')"
  if [[ -z "$DEVICE_PORT" ]]; then
    echo "Board '$BOARD_ID' is missing a device_path in inventory." >&2
    exit 2
  fi
  if [[ ( "$NEEDS_LIVE_CORE" -eq 1 || "$NEEDS_LIVE_DISPLAY" -eq 1 ) && -z "$METRICS_URL" ]]; then
    echo "Board '$BOARD_ID' is missing a metrics_url in inventory." >&2
    exit 2
  fi
fi

BOARD_ARTIFACT_ROOT="$ARTIFACT_ROOT/$BOARD_ID"
mkdir -p "$BOARD_ARTIFACT_ROOT/runs"

GIT_SHA="$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
GIT_REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
TIMESTAMP="$(date -u +%Y%m%d_%H%M%S)"
RUN_DIR="$BOARD_ARTIFACT_ROOT/runs/${TIMESTAMP}_${GIT_SHA}"
if [[ -e "$RUN_DIR" ]]; then
  suffix=2
  while [[ -e "${RUN_DIR}_${suffix}" ]]; do
    suffix=$((suffix + 1))
  done
  RUN_DIR="${RUN_DIR}_${suffix}"
fi
mkdir -p "$RUN_DIR"

RUN_LOG="$RUN_DIR/run.log"
RESULT_JSON="$RUN_DIR/result.json"
COMPARISON_TXT="$RUN_DIR/comparison.txt"
RUN_HISTORY_TSV="$BOARD_ARTIFACT_ROOT/run_history.tsv"
METRIC_HISTORY_TSV="$BOARD_ARTIFACT_ROOT/metric_history.tsv"

DEVICE_DIR="$RUN_DIR/device_tests"
CORE_DIR="$RUN_DIR/core_soak"
DISPLAY_DIR="$RUN_DIR/display_soak"

PREVIOUS_RUN_DIR="$(find "$BOARD_ARTIFACT_ROOT/runs" -mindepth 1 -maxdepth 1 -type d ! -path "$RUN_DIR" | sort | tail -n1 || true)"
PREVIOUS_DEVICE_MANIFEST=""
PREVIOUS_CORE_MANIFEST=""
PREVIOUS_DISPLAY_MANIFEST=""
if [[ -n "$PREVIOUS_RUN_DIR" ]]; then
  [[ -f "$PREVIOUS_RUN_DIR/device_tests/manifest.json" ]] && PREVIOUS_DEVICE_MANIFEST="$PREVIOUS_RUN_DIR/device_tests/manifest.json"
  [[ -f "$PREVIOUS_RUN_DIR/core_soak/manifest.json" ]] && PREVIOUS_CORE_MANIFEST="$PREVIOUS_RUN_DIR/core_soak/manifest.json"
  [[ -f "$PREVIOUS_RUN_DIR/display_soak/manifest.json" ]] && PREVIOUS_DISPLAY_MANIFEST="$PREVIOUS_RUN_DIR/display_soak/manifest.json"
fi

enabled_steps=()
[[ "$RUN_DEVICE" -eq 1 ]] && enabled_steps+=("device_tests")
[[ "$RUN_CORE" -eq 1 ]] && enabled_steps+=("core_soak")
[[ "$RUN_DISPLAY" -eq 1 ]] && enabled_steps+=("display_soak")
ENABLED_STEPS_CSV="$(IFS=,; echo "${enabled_steps[*]}")"

: > "$RUN_LOG"

run_step() {
  local step_name="$1"
  shift
  local step_log="$RUN_DIR/${step_name}.log"
  local status=0

  echo "==> ${step_name}" | tee -a "$RUN_LOG"
  set +e
  "$@" > >(tee "$step_log" | tee -a "$RUN_LOG") 2>&1
  status=$?
  set -e
  echo "==> ${step_name} exit=${status}" | tee -a "$RUN_LOG"
  echo "" | tee -a "$RUN_LOG"
  return "$status"
}

render_step_views() {
  local step_dir="$1"
  local scoring_json="$step_dir/scoring.json"
  if [[ ! -f "$scoring_json" ]]; then
    return 0
  fi

  python3 - "$scoring_json" "$step_dir/comparison.txt" "$step_dir/comparison.tsv" <<'PY'
import json
import sys
from pathlib import Path

from tools.hardware_report_utils import write_comparison_text, write_comparison_tsv

scoring_path = Path(sys.argv[1])
text_path = Path(sys.argv[2])
tsv_path = Path(sys.argv[3])
payload = json.loads(scoring_path.read_text(encoding="utf-8"))
write_comparison_text(payload, text_path)
write_comparison_tsv(payload, tsv_path)
PY
}

device_exit=0
core_exit=0
display_exit=0

if [[ "$RUN_DEVICE" -eq 1 ]]; then
  device_args=(
    env
    DEVICE_PORT="$DEVICE_PORT"
    DEVICE_BOARD_ID="$BOARD_ID"
    "$DEVICE_TESTS_SCRIPT"
    --full
    --out-dir "$DEVICE_DIR"
  )
  if [[ -n "$PREVIOUS_DEVICE_MANIFEST" ]]; then
    device_args+=(--compare-to "$PREVIOUS_DEVICE_MANIFEST")
  fi
  run_step "device_tests" "${device_args[@]}" || device_exit=$?
  render_step_views "$DEVICE_DIR"
fi

if [[ "$RUN_CORE" -eq 1 ]]; then
  if [[ "$PARSE_STEP" == "core_soak" ]]; then
    core_args=(
      python3
      "$IMPORT_DRIVE_LOG_SCRIPT"
      --input "$PARSE_DRIVE_LOG"
      --out-dir "$CORE_DIR"
      --board-id "$BOARD_ID"
      --git-sha "$GIT_SHA"
      --git-ref "$GIT_REF"
      --stress-class core
      --lane hardware-test-parse
    )
    if [[ -n "$PREVIOUS_CORE_MANIFEST" ]]; then
      core_args+=(--compare-to "$PREVIOUS_CORE_MANIFEST")
    fi
    run_step "core_soak" "${core_args[@]}" || core_exit=$?
  else
    core_args=(
      env
      DEVICE_PORT="$DEVICE_PORT"
      DEVICE_BOARD_ID="$BOARD_ID"
      "$REAL_SOAK_SCRIPT"
      --duration-seconds "$SOAK_DURATION_SECONDS"
      --with-fs
      --metrics-url "$METRICS_URL"
      --require-metrics
      --profile drive_wifi_ap
      --out-dir "$CORE_DIR"
    )
    if [[ -n "$PREVIOUS_CORE_MANIFEST" ]]; then
      core_args+=(--compare-to "$PREVIOUS_CORE_MANIFEST")
    fi
    run_step "core_soak" "${core_args[@]}" || core_exit=$?
    render_step_views "$CORE_DIR"
  fi
fi

if [[ "$RUN_DISPLAY" -eq 1 ]]; then
  if [[ "$PARSE_STEP" == "display_soak" ]]; then
    display_args=(
      python3
      "$IMPORT_DRIVE_LOG_SCRIPT"
      --input "$PARSE_DRIVE_LOG"
      --out-dir "$DISPLAY_DIR"
      --board-id "$BOARD_ID"
      --git-sha "$GIT_SHA"
      --git-ref "$GIT_REF"
      --stress-class display_preview
      --lane hardware-test-parse
    )
    if [[ -n "$PREVIOUS_DISPLAY_MANIFEST" ]]; then
      display_args+=(--compare-to "$PREVIOUS_DISPLAY_MANIFEST")
    fi
    run_step "display_soak" "${display_args[@]}" || display_exit=$?
  else
    display_args=(
      env
      DEVICE_PORT="$DEVICE_PORT"
      DEVICE_BOARD_ID="$BOARD_ID"
      "$REAL_SOAK_SCRIPT"
      --skip-flash
      --duration-seconds "$SOAK_DURATION_SECONDS"
      --metrics-url "$METRICS_URL"
      --require-metrics
      --profile drive_wifi_ap
      --drive-display-preview
      --out-dir "$DISPLAY_DIR"
    )
    if [[ -n "$PREVIOUS_DISPLAY_MANIFEST" ]]; then
      display_args+=(--compare-to "$PREVIOUS_DISPLAY_MANIFEST")
    fi
    run_step "display_soak" "${display_args[@]}" || display_exit=$?
    render_step_views "$DISPLAY_DIR"
  fi
fi

python3 "$ASSEMBLE_RESULT_SCRIPT" \
  --run-dir "$RUN_DIR" \
  --result-json "$RESULT_JSON" \
  --comparison-txt "$COMPARISON_TXT" \
  --run-history-tsv "$RUN_HISTORY_TSV" \
  --metric-history-tsv "$METRIC_HISTORY_TSV" \
  --warning-policy "$( [[ "$STRICT_WARNINGS" == "1" ]] && echo blocking || echo non_blocking )" \
  --board-id "$BOARD_ID" \
  --device-port "$DEVICE_PORT" \
  --metrics-url "$METRICS_URL" \
  --git-sha "$GIT_SHA" \
  --git-ref "$GIT_REF" \
  --previous-run-dir "$PREVIOUS_RUN_DIR" \
  --enabled-steps "$ENABLED_STEPS_CSV" \
  --device-exit "$device_exit" \
  --core-exit "$core_exit" \
  --display-exit "$display_exit"

rm -rf "$BOARD_ARTIFACT_ROOT/latest"
ln -s "runs/$(basename "$RUN_DIR")" "$BOARD_ARTIFACT_ROOT/latest"

suite_result="$(python3 -c 'import json, sys; from pathlib import Path; print(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))["result"])' "$RESULT_JSON")"
echo "Hardware test result: $suite_result"
echo "Warning policy: $( [[ "$STRICT_WARNINGS" == "1" ]] && echo blocking || echo non-blocking )"
echo "Latest artifacts: $BOARD_ARTIFACT_ROOT/latest"
echo "Readable summary: $COMPARISON_TXT"

if [[ "$suite_result" == "FAIL" ]]; then
  exit 1
fi
if [[ "$suite_result" == "PASS_WITH_WARNINGS" && "$STRICT_WARNINGS" == "1" ]]; then
  exit 1
fi
exit 0
