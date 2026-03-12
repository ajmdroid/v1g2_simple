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
HTTP_TIMEOUT_SECONDS="${HARDWARE_TEST_HTTP_TIMEOUT_SECONDS:-5}"
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

usage() {
  cat <<'EOF'
Usage: ./scripts/hardware/test.sh [options]

Options:
  -a, --all                 Run the full fixed test suite (default)
  -d, --device              Run only the device suite step
  -c, --core                Run only the core soak step
  -p, --display             Run only the display-preview soak step
  --parse-drive-log PATH    Parse a saved soak directory, metrics.jsonl, panic.jsonl,
                            or serial.log instead of running a live soak step. If
                            no soak step is selected, the script infers core/display
                            from the input manifest and otherwise defaults to core.
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
  ./scripts/hardware/test.sh --core --parse-drive-log /path/to/serial.log
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
UPTIME_LOG="$RUN_DIR/uptime_continuity.log"

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

# ── Uptime continuity tracking ───────────────────────────────────────
suite_last_uptime_ms=""
suite_reboot_count=0
UPTIME_LOG=""

poll_uptime_ms() {
  local endpoint="$METRICS_URL"
  if [[ "$endpoint" != *"soak="* ]]; then
    if [[ "$endpoint" == *"?"* ]]; then
      endpoint="${endpoint}&soak=1"
    else
      endpoint="${endpoint}?soak=1"
    fi
  fi
  local payload
  payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$endpoint" 2>/dev/null || true)"
  if [[ -z "$payload" ]]; then
    echo ""
    return
  fi
  printf "%s" "$payload" | tr -d '\r\n' | sed -n 's/.*"uptimeMs":[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -n1
}

check_uptime_continuity() {
  local context="$1"
  if [[ "$NEEDS_LIVE_BOARD" -eq 0 || -z "$METRICS_URL" ]]; then
    return
  fi
  local current_uptime
  current_uptime="$(poll_uptime_ms)"
  if [[ ! "$current_uptime" =~ ^[0-9]+$ ]]; then
    return
  fi
  if [[ -n "$suite_last_uptime_ms" && "$current_uptime" -lt "$suite_last_uptime_ms" ]]; then
    suite_reboot_count=$((suite_reboot_count + 1))
    local msg="[reboot] uptimeMs dropped from ${suite_last_uptime_ms} to ${current_uptime} (${context})"
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
  # Verify endpoint reachable before committing to the scenario
  if ! curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$METRICS_URL" >/dev/null 2>&1; then
    echo "==> rad_scenario [skip] metrics endpoint unreachable" | tee -a "$RUN_LOG"
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

# ── RAD scenario preflight ──────────────────────────────────────
check_uptime_continuity "before_rad_scenario"
run_rad_scenario || rad_exit=$?
check_uptime_continuity "after_rad_scenario"

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
  check_uptime_continuity "after_device_tests"
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
    check_uptime_continuity "after_core_soak"
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
    check_uptime_continuity "after_display_soak"
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
if [[ "$rad_exit" -ne 0 ]]; then
  echo "RAD scenario: FAIL (exit=$rad_exit)"
fi
if [[ "$suite_reboot_count" -gt 0 ]]; then
  echo "Uptime continuity: ${suite_reboot_count} reboot(s) detected (see $UPTIME_LOG)"
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
