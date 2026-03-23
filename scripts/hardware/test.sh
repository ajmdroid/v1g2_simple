#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT_DIR"
SELF_NAME="${HARDWARE_TEST_SELF_NAME:-./scripts/hardware/test.sh}"

BOARD_ID="${HARDWARE_TEST_BOARD_ID:-release}"
INVENTORY_PATH="${HARDWARE_TEST_INVENTORY:-$ROOT_DIR/test/device/board_inventory.json}"
DEVICE_TESTS_SCRIPT="${HARDWARE_TEST_DEVICE_TESTS_SCRIPT:-$ROOT_DIR/scripts/run_device_tests.sh}"
REAL_SOAK_SCRIPT="${HARDWARE_TEST_REAL_SOAK_SCRIPT:-$ROOT_DIR/scripts/run_real_fw_soak.sh}"
IMPORT_DRIVE_LOG_SCRIPT="${HARDWARE_TEST_IMPORT_DRIVE_LOG_SCRIPT:-$ROOT_DIR/tools/import_drive_log.py}"
IMPORT_PERF_CSV_SCRIPT="${HARDWARE_TEST_IMPORT_PERF_CSV_SCRIPT:-$ROOT_DIR/tools/import_perf_csv.py}"
ASSEMBLE_RESULT_SCRIPT="${HARDWARE_TEST_ASSEMBLE_RESULT_SCRIPT:-$ROOT_DIR/tools/assemble_hardware_test_result.py}"
ARTIFACT_ROOT="${HARDWARE_TEST_ARTIFACT_ROOT:-$ROOT_DIR/.artifacts/hardware/test}"
SOAK_DURATION_SECONDS="${HARDWARE_TEST_SOAK_DURATION_SECONDS:-300}"
STRICT_WARNINGS="${HARDWARE_TEST_STRICT_WARNINGS:-0}"
STRICT_SOAKS="${HARDWARE_TEST_STRICT_SOAKS:-0}"
BASELINE_OVERRIDE_PATH="${HARDWARE_TEST_BASELINE_OVERRIDE_PATH:-}"
HTTP_TIMEOUT_SECONDS="${HARDWARE_TEST_HTTP_TIMEOUT_SECONDS:-5}"
METRICS_ENDPOINT_ATTEMPTS="${HARDWARE_TEST_METRICS_ENDPOINT_ATTEMPTS:-6}"
METRICS_ENDPOINT_RETRY_DELAY_SECONDS="${HARDWARE_TEST_METRICS_ENDPOINT_RETRY_DELAY_SECONDS:-2}"
RAD_SCENARIO_ID="${HARDWARE_TEST_RAD_SCENARIO_ID:-RAD-03}"
RAD_DURATION_SCALE_PCT="${HARDWARE_TEST_RAD_DURATION_SCALE_PCT:-100}"
RAD_MIN_RX_DELTA="${HARDWARE_TEST_RAD_MIN_RX_DELTA:-20}"
RAD_MIN_PARSE_SUCCESS_DELTA="${HARDWARE_TEST_RAD_MIN_PARSE_SUCCESS_DELTA:-20}"
RAD_MIN_DISPLAY_UPDATES_DELTA="${HARDWARE_TEST_RAD_MIN_DISPLAY_UPDATES_DELTA:-10}"
RAD_TIMEOUT_SECONDS="${HARDWARE_TEST_RAD_TIMEOUT_SECONDS:-25}"

SELECTED_EXPLICITLY=0
RUN_DEVICE=0
RUN_CORE=0
RUN_DISPLAY=0
PARSE_DRIVE_LOG=""
SEGMENT_SELECTOR="auto"
LIST_SEGMENTS=0

usage() {
  cat <<EOF
Usage: ${SELF_NAME} [options]

Options:
  -a, --all                 Run the full fixed test suite (default)
  -d, --device              Run only the device suite step
  -c, --core                Run only the core soak step
  -p, --display             Run only the display-preview soak step
  --parse-drive-log PATH    Parse a saved soak directory, metrics.jsonl, panic.jsonl,
                            serial.log, or perf CSV (.csv) instead of running a
                            live soak step. If no soak step is selected, the script
                            infers core/display from the input manifest and
                            otherwise defaults to core.
  --segment VALUE           Drive segment selector for perf CSV imports:
                            auto (default), last, longest-connected, or 1-based index
  --list-segments           List discovered perf CSV segments and exit
  --board-id ID             Board id from test/device/board_inventory.json
                            (default: release)
  --duration-seconds N      Soak duration for live soak steps (default: 300)
  --artifact-root PATH      Base output root (default: .artifacts/hardware/test);
                            runs are stored under <artifact-root>/<board-id>/
  --strict                  Treat PASS_WITH_WARNINGS as a failing exit
  --strict-soaks            Make soak steps authoritative for the suite result
  -h, --help                Show this help

Examples:
  ${SELF_NAME}
  ${SELF_NAME} --core
  ${SELF_NAME} --display
  ${SELF_NAME} --all --board-id release --strict
  ${SELF_NAME} --all --board-id release --strict-soaks --strict
  ${SELF_NAME} --device --parse-drive-log /path/to/metrics.jsonl
  ${SELF_NAME} --core --parse-drive-log /path/to/serial.log
  ${SELF_NAME} --parse-drive-log /path/to/real_fw_soak_run
  ${SELF_NAME} --list-segments --parse-drive-log /path/to/perf_boot_1.csv
  ${SELF_NAME} --core --parse-drive-log /path/to/perf_boot_1.csv
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
    --segment)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --segment" >&2
        exit 2
      fi
      SEGMENT_SELECTOR="$2"
      shift
      ;;
    --list-segments)
      LIST_SEGMENTS=1
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
    --strict-soaks)
      STRICT_SOAKS=1
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

if [[ "$LIST_SEGMENTS" -eq 1 ]]; then
  if [[ -z "$PARSE_DRIVE_LOG" ]]; then
    echo "--list-segments requires --parse-drive-log PATH." >&2
    exit 2
  fi
  if [[ "$PARSE_DRIVE_LOG" != *.csv ]]; then
    echo "--list-segments only supports perf CSV inputs." >&2
    exit 2
  fi
fi

if [[ "$SEGMENT_SELECTOR" != "auto" && -z "$PARSE_DRIVE_LOG" ]]; then
  echo "--segment requires --parse-drive-log PATH." >&2
  exit 2
fi

if [[ "$SEGMENT_SELECTOR" != "auto" && "$PARSE_DRIVE_LOG" != *.csv ]]; then
  echo "--segment only supports perf CSV inputs." >&2
  exit 2
fi

if [[ "$LIST_SEGMENTS" -eq 1 ]]; then
  python3 "$IMPORT_PERF_CSV_SCRIPT" --input "$PARSE_DRIVE_LOG" --list-segments --segment "$SEGMENT_SELECTOR"
  exit $?
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
if [[ -n "$PARSE_DRIVE_LOG" ]]; then
  if [[ "$PARSE_DRIVE_LOG" == *.csv ]]; then
    if [[ ! -f "$IMPORT_PERF_CSV_SCRIPT" ]]; then
      echo "Missing import helper: $IMPORT_PERF_CSV_SCRIPT" >&2
      exit 2
    fi
  else
    if [[ ! -f "$IMPORT_DRIVE_LOG_SCRIPT" ]]; then
      echo "Missing import helper: $IMPORT_DRIVE_LOG_SCRIPT" >&2
      exit 2
    fi
  fi
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
if [[ -z "$BASELINE_OVERRIDE_PATH" ]]; then
  BASELINE_OVERRIDE_PATH="$BOARD_ARTIFACT_ROOT/baseline_manifest_overrides.json"
fi

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
UPTIME_LOG="$RUN_DIR/uptime_continuity.log"

DEVICE_DIR="$RUN_DIR/device_tests"
CORE_DIR="$RUN_DIR/core_soak"
DISPLAY_DIR="$RUN_DIR/display_soak"

enabled_steps=()
[[ "$RUN_DEVICE" -eq 1 ]] && enabled_steps+=("device_tests")
[[ "$RUN_CORE" -eq 1 ]] && enabled_steps+=("core_soak")
[[ "$RUN_DISPLAY" -eq 1 ]] && enabled_steps+=("display_soak")
ENABLED_STEPS_CSV="$(IFS=,; echo "${enabled_steps[*]}")"

is_trustworthy_previous_run() {
  local candidate_dir="$1"
  python3 - "$candidate_dir" "$ENABLED_STEPS_CSV" <<'PY'
import json
import sys
from pathlib import Path

candidate_dir = Path(sys.argv[1])
enabled_steps = [item for item in sys.argv[2].split(",") if item]
result_path = candidate_dir / "result.json"
if not result_path.is_file():
    raise SystemExit(1)

payload = json.loads(result_path.read_text(encoding="utf-8"))
steps = {str(item.get("name")): item for item in payload.get("steps") or []}
trusted_results = {"PASS", "PASS_WITH_WARNINGS", "NO_BASELINE"}

for step_name in enabled_steps:
    step = steps.get(step_name)
    if not isinstance(step, dict):
        raise SystemExit(1)
    if str(step.get("result", "")) not in trusted_results:
        raise SystemExit(1)
    if str(step.get("comparison_kind", "")) == "no_scoring":
        raise SystemExit(1)
    manifest_path = Path(str(step.get("manifest_path", "")))
    if not manifest_path.is_file():
        raise SystemExit(1)

raise SystemExit(0)
PY
}

read_baseline_override_previous_run_dir() {
  local override_path="$1"
  python3 - "$override_path" <<'PY'
import json
import sys
from pathlib import Path

override_path = Path(sys.argv[1])
if not override_path.is_file():
    raise SystemExit(1)

try:
    payload = json.loads(override_path.read_text(encoding="utf-8"))
except json.JSONDecodeError:
    raise SystemExit(1)

candidate = Path(str(payload.get("previous_run_dir") or "")).resolve()
if not candidate.is_dir():
    raise SystemExit(1)

print(candidate)
PY
}

collect_baseline_override_manifests() {
  local override_path="$1"
  local step_name="$2"
  python3 - "$override_path" "$step_name" <<'PY'
import json
import sys
from pathlib import Path

override_path = Path(sys.argv[1])
step_name = sys.argv[2]
if not override_path.is_file():
    raise SystemExit(1)

try:
    payload = json.loads(override_path.read_text(encoding="utf-8"))
except json.JSONDecodeError:
    raise SystemExit(1)

steps = payload.get("steps")
if not isinstance(steps, dict):
    raise SystemExit(1)

manifests = steps.get(step_name)
if not isinstance(manifests, list):
    raise SystemExit(1)

printed = False
for raw_path in manifests:
    manifest_path = Path(str(raw_path)).resolve()
    if not manifest_path.is_file():
        continue
    print(manifest_path)
    printed = True

if not printed:
    raise SystemExit(1)
PY
}

find_previous_run_dir() {
  local override_previous_run_dir=""
  override_previous_run_dir="$(read_baseline_override_previous_run_dir "$BASELINE_OVERRIDE_PATH" || true)"
  if [[ -n "$override_previous_run_dir" ]]; then
    printf '%s\n' "$override_previous_run_dir"
    return 0
  fi

  local candidate_dir=""
  while IFS= read -r candidate_dir; do
    [[ -z "$candidate_dir" ]] && continue
    if is_trustworthy_previous_run "$candidate_dir"; then
      printf '%s\n' "$candidate_dir"
      return 0
    fi
  done < <(find "$BOARD_ARTIFACT_ROOT/runs" -mindepth 1 -maxdepth 1 -type d ! -path "$RUN_DIR" | sort -r)
  return 1
}

collect_previous_step_manifests() {
  local step_name="$1"
  if collect_baseline_override_manifests "$BASELINE_OVERRIDE_PATH" "$step_name"; then
    return 0
  fi

  python3 - "$BOARD_ARTIFACT_ROOT/runs" "$RUN_DIR" "$step_name" <<'PY'
import json
import sys
from pathlib import Path

runs_root = Path(sys.argv[1])
current_run_dir = Path(sys.argv[2]).resolve()
step_name = sys.argv[3]
pass_like = {"PASS", "PASS_WITH_WARNINGS"}
fallback_like = {"NO_BASELINE"}

passing: list[Path] = []
fallback: list[Path] = []

for candidate_dir in sorted(runs_root.iterdir(), reverse=True):
    if not candidate_dir.is_dir() or candidate_dir.resolve() == current_run_dir:
        continue
    result_path = candidate_dir / "result.json"
    if not result_path.is_file():
        continue
    try:
        payload = json.loads(result_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        continue
    steps = {str(item.get("name")): item for item in payload.get("steps") or [] if isinstance(item, dict)}
    step = steps.get(step_name)
    if not isinstance(step, dict):
        continue
    if str(step.get("comparison_kind", "")) == "no_scoring":
        continue
    manifest_path = Path(str(step.get("manifest_path", "")))
    if not manifest_path.is_absolute():
        manifest_path = candidate_dir / manifest_path
    manifest_path = manifest_path.resolve()
    if not manifest_path.is_file():
        continue
    result = str(step.get("result", ""))
    if result in pass_like:
        passing.append(manifest_path)
    elif result in fallback_like:
        fallback.append(manifest_path)

selected = passing[:3] if passing else fallback[:3]
for manifest_path in selected:
    print(manifest_path)
PY
}

PREVIOUS_RUN_DIR="$(find_previous_run_dir || true)"
PREVIOUS_DEVICE_MANIFESTS=()
PREVIOUS_CORE_MANIFESTS=()
PREVIOUS_DISPLAY_MANIFESTS=()
while IFS= read -r manifest_path; do
  [[ -n "$manifest_path" ]] && PREVIOUS_DEVICE_MANIFESTS+=("$manifest_path")
done < <(collect_previous_step_manifests "device_tests")
while IFS= read -r manifest_path; do
  [[ -n "$manifest_path" ]] && PREVIOUS_CORE_MANIFESTS+=("$manifest_path")
done < <(collect_previous_step_manifests "core_soak")
while IFS= read -r manifest_path; do
  [[ -n "$manifest_path" ]] && PREVIOUS_DISPLAY_MANIFESTS+=("$manifest_path")
done < <(collect_previous_step_manifests "display_soak")
PREVIOUS_DEVICE_MANIFEST="${PREVIOUS_DEVICE_MANIFESTS[0]:-}"
PREVIOUS_CORE_MANIFEST="${PREVIOUS_CORE_MANIFESTS[0]:-}"
PREVIOUS_DISPLAY_MANIFEST="${PREVIOUS_DISPLAY_MANIFESTS[0]:-}"

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

  if ! python3 - "$scoring_json" "$step_dir/comparison.txt" "$step_dir/comparison.tsv" <<'PY'
import json
import sys
from pathlib import Path

from tools.hardware_report_utils import write_comparison_text, write_comparison_tsv

scoring_path = Path(sys.argv[1])
text_path = Path(sys.argv[2])
tsv_path = Path(sys.argv[3])
try:
    payload = json.loads(scoring_path.read_text(encoding="utf-8"))
except json.JSONDecodeError as exc:
    raise SystemExit(f"invalid scoring json: {exc}")
write_comparison_text(payload, text_path)
write_comparison_tsv(payload, tsv_path)
PY
  then
    echo "[WARN] Skipping comparison view render for $step_dir because scoring.json is invalid." | tee -a "$RUN_LOG"
  fi
}

# ── Uptime continuity tracking ───────────────────────────────────────
suite_last_uptime_ms=""
suite_uptime_drop_count=0
suite_suspicious_uptime_drop_count=0
suite_flash_boundary_advisory_count=0
UPTIME_LOG=""

metrics_soak_url() {
  local endpoint="$1"
  if [[ -z "$endpoint" ]]; then
    echo ""
    return
  fi
  if [[ "$endpoint" != *"soak="* ]]; then
    if [[ "$endpoint" == *"?"* ]]; then
      endpoint="${endpoint}&soak=1"
    else
      endpoint="${endpoint}?soak=1"
    fi
  fi
  echo "$endpoint"
}

poll_uptime_ms() {
  local endpoint
  endpoint="$(metrics_soak_url "$METRICS_URL")"
  local payload
  payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$endpoint" 2>/dev/null || true)"
  if [[ -z "$payload" ]]; then
    echo ""
    return
  fi
  printf "%s" "$payload" | tr -d '\r\n' | sed -n 's/.*"uptimeMs":[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -n1
}

validate_json_payload() {
  python3 -c 'import json, sys; json.load(sys.stdin)' >/dev/null 2>&1
}

metrics_endpoint_max_wait_seconds() {
  local max_wait_seconds=$((METRICS_ENDPOINT_ATTEMPTS * HTTP_TIMEOUT_SECONDS))
  if [[ "$METRICS_ENDPOINT_ATTEMPTS" -gt 1 ]]; then
    max_wait_seconds=$((max_wait_seconds + (METRICS_ENDPOINT_ATTEMPTS - 1) * METRICS_ENDPOINT_RETRY_DELAY_SECONDS))
  fi
  echo "$max_wait_seconds"
}

wait_for_metrics_endpoint_recovery() {
  local endpoint_name="$1"
  local endpoint_url="$2"
  local attempt=1
  local payload=""
  local max_wait_seconds
  max_wait_seconds="$(metrics_endpoint_max_wait_seconds)"

  if [[ -z "$endpoint_url" ]]; then
    return 0
  fi

  echo "==> waiting for ${endpoint_name} (${METRICS_ENDPOINT_ATTEMPTS} attempt(s), retry ${METRICS_ENDPOINT_RETRY_DELAY_SECONDS}s, bounded wait <= ${max_wait_seconds}s)" | tee -a "$RUN_LOG"
  while [[ "$attempt" -le "$METRICS_ENDPOINT_ATTEMPTS" ]]; do
    payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$endpoint_url" 2>/dev/null || true)"
    if [[ -n "$payload" ]] && printf "%s" "$payload" | validate_json_payload; then
      if [[ "$attempt" -eq 1 ]]; then
        echo "    ${endpoint_name}: OK" | tee -a "$RUN_LOG"
      else
        echo "    ${endpoint_name}: OK after retry (${attempt}/${METRICS_ENDPOINT_ATTEMPTS})" | tee -a "$RUN_LOG"
      fi
      return 0
    fi
    if [[ "$attempt" -lt "$METRICS_ENDPOINT_ATTEMPTS" && "$METRICS_ENDPOINT_RETRY_DELAY_SECONDS" -gt 0 ]]; then
      sleep "$METRICS_ENDPOINT_RETRY_DELAY_SECONDS"
    fi
    attempt=$((attempt + 1))
  done
  echo "    ${endpoint_name}: timeout waiting for recovery" | tee -a "$RUN_LOG"
  return 1
}

check_uptime_continuity() {
  local context="$1"
  local allow_flash_reset_advisory="${2:-0}"
  if [[ "$NEEDS_LIVE_BOARD" -eq 0 || -z "$METRICS_URL" ]]; then
    return
  fi
  local current_uptime
  current_uptime="$(poll_uptime_ms)"
  if [[ ! "$current_uptime" =~ ^[0-9]+$ ]]; then
    return
  fi
  if [[ -n "$suite_last_uptime_ms" && "$current_uptime" -lt "$suite_last_uptime_ms" ]]; then
    suite_uptime_drop_count=$((suite_uptime_drop_count + 1))
    local msg=""
    if [[ "$allow_flash_reset_advisory" -eq 1 && "$suite_uptime_drop_count" -eq 1 ]]; then
      suite_flash_boundary_advisory_count=$((suite_flash_boundary_advisory_count + 1))
      msg="[advisory] uptimeMs dropped from ${suite_last_uptime_ms} to ${current_uptime} (${context}; expected flash/reset boundary)"
    else
      suite_suspicious_uptime_drop_count=$((suite_suspicious_uptime_drop_count + 1))
      msg="[reboot] uptimeMs dropped from ${suite_last_uptime_ms} to ${current_uptime} (${context})"
    fi
    echo "$msg" | tee -a "$RUN_LOG"
    if [[ -n "$UPTIME_LOG" ]]; then
      echo "$msg" >> "$UPTIME_LOG"
    fi
  fi
  suite_last_uptime_ms="$current_uptime"
}

# ── RAD scenario preflight ───────────────────────────────────────────
run_rad_scenario() {
  if [[ "$NEEDS_LIVE_BOARD" -eq 0 || -z "$METRICS_URL" ]]; then
    return 0
  fi
  local metrics_poll_url=""
  metrics_poll_url="$(metrics_soak_url "$METRICS_URL")"
  if ! wait_for_metrics_endpoint_recovery "RAD metrics endpoint" "$metrics_poll_url"; then
    echo "==> rad_scenario [skip] metrics endpoint failed to recover" | tee -a "$RUN_LOG"
    return 0
  fi
  local debug_base="${METRICS_URL%%\?*}"
  debug_base="${debug_base%/api/debug/metrics}"
  local rad_log="$RUN_DIR/rad_scenario.log"
  local rc=0

  echo "==> rad_scenario" | tee -a "$RUN_LOG"
  set +e
  python3 - "$debug_base" "$RAD_SCENARIO_ID" "$RAD_MIN_RX_DELTA" \
    "$RAD_MIN_PARSE_SUCCESS_DELTA" "$RAD_MIN_DISPLAY_UPDATES_DELTA" \
    "$HTTP_TIMEOUT_SECONDS" "$RAD_DURATION_SCALE_PCT" "$RAD_TIMEOUT_SECONDS" \
    >"$rad_log" 2>&1 <<'RADPY'
import json
import sys
import time
import urllib.request

base = sys.argv[1].rstrip("/")
scenario_id = sys.argv[2]
min_rx = int(sys.argv[3])
min_parse = int(sys.argv[4])
min_display = int(sys.argv[5])
timeout = int(sys.argv[6])
scale_pct = int(sys.argv[7])
scenario_timeout_s = int(sys.argv[8])

def get_json(path):
    req = urllib.request.Request(base + path, method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)

def post_json(path, payload=None):
    body = b"" if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        base + path,
        data=body,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.load(resp)

def subset(metrics):
    keys = ["rxPackets", "parseSuccesses", "parseFailures", "displayUpdates"]
    return {k: int(metrics.get(k, 0) or 0) for k in keys}

ok = True
reasons = []
keys = ["rxPackets", "parseSuccesses", "parseFailures", "displayUpdates"]
reset_resp = {"success": False}
start_resp = {"success": False}
status = {}
pre = {k: 0 for k in keys}
post = {k: 0 for k in keys}
delta = {k: 0 for k in keys}
cleanup_stop_success = False

try:
    reset_resp = post_json("/api/debug/metrics/reset")
    pre = subset(get_json("/api/debug/metrics?soak=1"))
    start_resp = post_json("/api/debug/v1-scenario/start", {
        "id": scenario_id,
        "loop": False,
        "streamRepeatMs": 700,
        "durationScalePct": scale_pct,
    })

    t0 = time.time()
    while True:
        if time.time() - t0 > scenario_timeout_s:
            reasons.append(f"scenario timeout (> {scenario_timeout_s}s)")
            ok = False
            break
        try:
            status = get_json("/api/debug/v1-scenario/status")
        except Exception:
            time.sleep(0.25)
            continue
        if not status.get("running", False):
            break
        time.sleep(0.25)

    post = subset(get_json("/api/debug/metrics?soak=1"))
    delta = {k: post[k] - pre[k] for k in pre}

    if not reset_resp.get("success"):
        ok = False
        reasons.append("metrics reset failed")
    if not start_resp.get("success"):
        ok = False
        reasons.append("scenario start failed")
    if delta["rxPackets"] < min_rx:
        ok = False
        reasons.append(f"rxPackets delta {delta['rxPackets']} < {min_rx}")
    if delta["parseSuccesses"] < min_parse:
        ok = False
        reasons.append(f"parseSuccesses delta {delta['parseSuccesses']} < {min_parse}")
    if delta["parseFailures"] != 0:
        ok = False
        reasons.append(f"parseFailures delta {delta['parseFailures']} != 0")
    if delta["displayUpdates"] < min_display:
        ok = False
        reasons.append(f"displayUpdates delta {delta['displayUpdates']} < {min_display}")
except Exception as exc:
    ok = False
    reasons.append(f"exception: {exc}")
finally:
    try:
        stop_resp = post_json("/api/debug/v1-scenario/stop")
        cleanup_stop_success = bool(stop_resp.get("success"))
    except Exception as stop_exc:
        cleanup_stop_success = False
        reasons.append(f"scenario stop failed: {stop_exc}")

if not cleanup_stop_success and "scenario stop failed" not in " ".join(reasons):
    ok = False
    reasons.append("scenario cleanup stop failed")

print(f"scenario={scenario_id} durationScalePct={scale_pct} timeoutSec={scenario_timeout_s}")
print(f"delta_rxPackets={delta['rxPackets']} delta_parseSuccesses={delta['parseSuccesses']} delta_parseFailures={delta['parseFailures']} delta_displayUpdates={delta['displayUpdates']}")
if reasons:
    print("reasons=" + "; ".join(reasons))
print(f"result={'PASS' if ok else 'FAIL'}")

sys.exit(0 if ok else 1)
RADPY
  rc=$?
  set -e
  cat "$rad_log" | tee -a "$RUN_LOG"
  echo "==> rad_scenario exit=${rc}" | tee -a "$RUN_LOG"
  echo "" | tee -a "$RUN_LOG"
  return "$rc"
}

device_exit=0
core_exit=0
display_exit=0
rad_exit=0

if [[ "$RUN_DEVICE" -eq 1 ]]; then
  # ── RAD scenario preflight ────────────────────────────────────
  check_uptime_continuity "before_rad_scenario"
  run_rad_scenario || rad_exit=$?
  check_uptime_continuity "after_rad_scenario"
fi

if [[ "$RUN_DEVICE" -eq 1 ]]; then
  device_args=(
    env
    DEVICE_PORT="$DEVICE_PORT"
    DEVICE_BOARD_ID="$BOARD_ID"
    "$DEVICE_TESTS_SCRIPT"
    --full
    --out-dir "$DEVICE_DIR"
  )
  if [[ "${#PREVIOUS_DEVICE_MANIFESTS[@]}" -gt 0 ]]; then
    for previous_manifest in "${PREVIOUS_DEVICE_MANIFESTS[@]}"; do
      device_args+=(--compare-to "$previous_manifest")
    done
  fi
  run_step "device_tests" "${device_args[@]}" || device_exit=$?
  render_step_views "$DEVICE_DIR"
  check_uptime_continuity "after_device_tests" 1
fi

if [[ "$RUN_CORE" -eq 1 ]]; then
  if [[ "$PARSE_STEP" == "core_soak" ]]; then
    if [[ "$PARSE_DRIVE_LOG" == *.csv ]]; then
      core_args=(
        python3
        "$IMPORT_PERF_CSV_SCRIPT"
        --input "$PARSE_DRIVE_LOG"
        --out-dir "$CORE_DIR"
        --board-id "$BOARD_ID"
        --git-sha "$GIT_SHA"
        --git-ref "$GIT_REF"
        --profile drive_wifi_ap
        --segment "$SEGMENT_SELECTOR"
        --stress-class core
        --lane hardware-test-parse
      )
    else
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
    fi
    if [[ "${#PREVIOUS_CORE_MANIFESTS[@]}" -gt 0 ]]; then
      for previous_manifest in "${PREVIOUS_CORE_MANIFESTS[@]}"; do
        core_args+=(--compare-to "$previous_manifest")
      done
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
      --max-ap-transition-churn-delta 1
      --max-proxy-adv-transition-churn-delta 1
      --out-dir "$CORE_DIR"
    )
    if [[ "${#PREVIOUS_CORE_MANIFESTS[@]}" -gt 0 ]]; then
      for previous_manifest in "${PREVIOUS_CORE_MANIFESTS[@]}"; do
        core_args+=(--compare-to "$previous_manifest")
      done
    fi
    run_step "core_soak" "${core_args[@]}" || core_exit=$?
    render_step_views "$CORE_DIR"
    check_uptime_continuity "after_core_soak" 1
  fi
fi

if [[ "$RUN_DISPLAY" -eq 1 ]]; then
  if [[ "$PARSE_STEP" == "display_soak" ]]; then
    if [[ "$PARSE_DRIVE_LOG" == *.csv ]]; then
      display_args=(
        python3
        "$IMPORT_PERF_CSV_SCRIPT"
        --input "$PARSE_DRIVE_LOG"
        --out-dir "$DISPLAY_DIR"
        --board-id "$BOARD_ID"
        --git-sha "$GIT_SHA"
        --git-ref "$GIT_REF"
        --profile drive_wifi_ap
        --segment "$SEGMENT_SELECTOR"
        --stress-class display_preview
        --lane hardware-test-parse
      )
    else
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
    fi
    if [[ "${#PREVIOUS_DISPLAY_MANIFESTS[@]}" -gt 0 ]]; then
      for previous_manifest in "${PREVIOUS_DISPLAY_MANIFESTS[@]}"; do
        display_args+=(--compare-to "$previous_manifest")
      done
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
      --max-ap-transition-churn-delta 1
      --max-proxy-adv-transition-churn-delta 1
      --out-dir "$DISPLAY_DIR"
    )
    if [[ "${#PREVIOUS_DISPLAY_MANIFESTS[@]}" -gt 0 ]]; then
      for previous_manifest in "${PREVIOUS_DISPLAY_MANIFESTS[@]}"; do
        display_args+=(--compare-to "$previous_manifest")
      done
    fi
    run_step "display_soak" "${display_args[@]}" || display_exit=$?
    render_step_views "$DISPLAY_DIR"
    check_uptime_continuity "after_display_soak"
  fi
fi

assemble_args=(
  python3 "$ASSEMBLE_RESULT_SCRIPT"
  --run-dir "$RUN_DIR"
  --result-json "$RESULT_JSON"
  --comparison-txt "$COMPARISON_TXT"
  --run-history-tsv "$RUN_HISTORY_TSV"
  --metric-history-tsv "$METRIC_HISTORY_TSV"
  --warning-policy "$( [[ "$STRICT_WARNINGS" == "1" ]] && echo blocking || echo non_blocking )"
  --board-id "$BOARD_ID"
  --device-port "$DEVICE_PORT"
  --metrics-url "$METRICS_URL"
  --git-sha "$GIT_SHA"
  --git-ref "$GIT_REF"
  --previous-run-dir "$PREVIOUS_RUN_DIR"
  --enabled-steps "$ENABLED_STEPS_CSV"
  --device-exit "$device_exit"
  --core-exit "$core_exit"
  --display-exit "$display_exit"
)
if [[ "$STRICT_SOAKS" == "1" ]]; then
  assemble_args+=(--strict-soaks)
fi
"${assemble_args[@]}"

rm -rf "$BOARD_ARTIFACT_ROOT/latest"
ln -s "runs/$(basename "$RUN_DIR")" "$BOARD_ARTIFACT_ROOT/latest"

suite_result="$(python3 -c 'import json, sys; from pathlib import Path; print(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))["result"])' "$RESULT_JSON")"
rollup_summary="$(python3 -c 'import json, sys; from pathlib import Path; print(json.loads(Path(sys.argv[1]).read_text(encoding="utf-8")).get("rollup_summary", ""))' "$RESULT_JSON")"
echo "Hardware test result: $suite_result"
if [[ -n "$rollup_summary" ]]; then
  echo "$rollup_summary"
fi
echo "Warning policy: $( [[ "$STRICT_WARNINGS" == "1" ]] && echo blocking || echo non-blocking )"
if [[ "$rad_exit" -ne 0 ]]; then
  echo "RAD scenario: FAIL (exit=$rad_exit)"
fi
if [[ "$suite_uptime_drop_count" -gt 0 ]]; then
  if [[ "$suite_suspicious_uptime_drop_count" -gt 0 ]]; then
    echo "Uptime continuity: ${suite_uptime_drop_count} drop(s) detected, ${suite_suspicious_uptime_drop_count} suspicious (see $UPTIME_LOG)"
  else
    echo "Uptime continuity: ${suite_uptime_drop_count} advisory drop(s) detected at flashing step boundary (see $UPTIME_LOG)"
  fi
fi
echo "Latest artifacts: $BOARD_ARTIFACT_ROOT/latest"
echo "Readable summary: $COMPARISON_TXT"

if [[ "$rad_exit" -ne 0 ]]; then
  exit 1
fi

if [[ "$suite_result" == "FAIL" ]]; then
  exit 1
fi
if [[ "$suite_result" == "PASS_WITH_WARNINGS" && "$STRICT_WARNINGS" == "1" ]]; then
  exit 1
fi
exit 0
