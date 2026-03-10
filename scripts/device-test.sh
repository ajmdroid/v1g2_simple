#!/usr/bin/env bash
#
# device-test.sh - Hardware test suite runner with per-item PASS/FAIL output
# and metrics evidence for each decision.
#
# Intended workflow:
#   1) Sanity check debug metrics endpoint
#   2) Run camera API/UI/display smoke on hardware
#   3) Run a short radar scenario (default: RAD-03) and verify parser/display deltas
#   4) Run real firmware soak (display stress)
#   5) Run real firmware soak (core only)
#
# Usage examples:
#   ./scripts/device-test.sh
#   ./scripts/device-test.sh --metrics-url http://192.168.160.212/api/debug/metrics
#   ./scripts/device-test.sh --duration-seconds 90 --rad-scenario RAD-02
#   ./scripts/device-test.sh --with-flash --port /dev/cu.usbmodem21201
#
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

METRICS_URL="${REAL_FW_METRICS_URL:-http://192.168.160.212/api/debug/metrics}"
HTTP_TIMEOUT_SECONDS="${REAL_FW_HTTP_TIMEOUT_SECONDS:-5}"
METRICS_ENDPOINT_ATTEMPTS="${REAL_FW_METRICS_ENDPOINT_ATTEMPTS:-3}"
METRICS_ENDPOINT_RETRY_DELAY_SECONDS="${REAL_FW_METRICS_ENDPOINT_RETRY_DELAY_SECONDS:-1}"
DURATION_SECONDS=60
SKIP_FLASH=1
TEST_PORT="${DEVICE_PORT:-}"
AUTO_KILL_MONITOR=1
DRY_RUN=0

SUITE_PROFILE_VERSION="device_v2"
SOAK_PROFILE="drive_wifi_ap"
SOAK_MIN_METRICS_OK_SAMPLES=3
SOAK_LATENCY_GATE_MODE="hybrid"
SOAK_LATENCY_ROBUST_MIN_SAMPLES=8
SOAK_LATENCY_ROBUST_MAX_EXCEED_PCT=5
SOAK_WIFI_ROBUST_SKIP_FIRST_SAMPLES=2
SOAK_MINIMA_TAIL_EXCLUDE_SAMPLES="${REAL_FW_SOAK_MINIMA_TAIL_EXCLUDE_SAMPLES:-3}"
SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS=3
SOAK_MIN_DISPLAY_UPDATES_DELTA=1
SOAK_ENABLE_TRANSITION_QUAL=1
SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS=15
SOAK_TRANSITION_FLAP_CYCLES=3
SOAK_TRANSITION_MIN_PROXY_ADV_OFF_TRANSITIONS=3
SOAK_TRANSITION_MAX_PROXY_RECOVERY_MS=30000
SOAK_TRANSITION_MAX_SAMPLES_TO_STABLE=6
IGNORE_GPS_ERRORS="${REAL_FW_IGNORE_GPS_ERRORS:-0}"

RAD_SCENARIO_ID="RAD-03"
RAD_DURATION_SCALE_PCT="${REAL_FW_RAD_DURATION_SCALE_PCT:-100}"
RAD_TIMEOUT_SECONDS="${REAL_FW_RAD_TIMEOUT_SECONDS:-}"
RAD_MIN_RX_DELTA=20
RAD_MIN_PARSE_SUCCESS_DELTA=20
RAD_MIN_DISPLAY_UPDATES_DELTA=10
CAMERA_RADAR_STRESS=1

OUT_DIR=""

if [[ "$IGNORE_GPS_ERRORS" != "1" ]]; then
  IGNORE_GPS_ERRORS=0
fi

# ANSI colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

TEST_NAMES=()
TEST_STATUS=()
TEST_METRICS=()
TEST_ARTIFACTS=()
TEST_COMMANDS=()

HARNESS_PRECHECK_CLASS=""
HARNESS_PRECHECK_CODE=""
HARNESS_PRECHECK_REASON=""
SUITE_EXIT_REASON=""

# Cross-test uptime continuity tracking
suite_last_uptime_ms=""
suite_reboot_count=0
suite_reboot_log=""

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
  local current_uptime
  current_uptime="$(poll_uptime_ms)"
  if [[ ! "$current_uptime" =~ ^[0-9]+$ ]]; then
    return
  fi
  # Flash-enabled soak items intentionally reboot the device inside the test
  # harness, so comparing pre/post uptime across those item boundaries is not a
  # valid stability signal.
  if [[ "$SKIP_FLASH" -eq 0 && "$context" == after\ soak_* ]]; then
    suite_last_uptime_ms="$current_uptime"
    return
  fi
  if [[ -n "$suite_last_uptime_ms" && "$current_uptime" -lt "$suite_last_uptime_ms" ]]; then
    suite_reboot_count=$((suite_reboot_count + 1))
    local msg="[FAIL] Device reboot detected ${context}: uptimeMs dropped from ${suite_last_uptime_ms} to ${current_uptime}"
    echo -e "${RED}${msg}${NC}"
    suite_reboot_log="${suite_reboot_log}${msg}\n"
  fi
  suite_last_uptime_ms="$current_uptime"
}

usage() {
  cat <<'EOF'
Usage: ./scripts/device-test.sh [options]

Options:
  --metrics-url URL              Debug metrics URL (default: env or 192.168.160.212)
  --duration-seconds N           Soak duration per run (default: 60)
  --http-timeout-seconds N       HTTP timeout for API calls (default: 5)
  --port PATH                    Fixed serial port (default: DEVICE_PORT or auto via soak script)
  --skip-flash                   Skip firmware flash before soak runs (default)
  --with-flash                   Flash before soak runs
  --rad-scenario ID              Short radar scenario ID (default: RAD-03)
  --rad-duration-scale-pct N     RAD scenario duration scale percent (default: 100)
  --rad-timeout-seconds N        RAD scenario completion timeout (default: auto from scale)
  --camera-radar-stress          Run camera+radar overlap stress item (default)
  --no-camera-radar-stress       Skip camera+radar overlap stress item
  --no-transition-soak           Skip Cycle 3 transition qualification soak
  --transition-flap-cycles N     Transition flap cycles for transition soak (default: 3)
  --transition-drive-interval-seconds N
                                 Transition flap action interval (default: 15)
  --transition-max-recovery-ms N Max proxy-off recovery time gate (default: 30000)
  --transition-max-samples N     Max samples-to-stable gate (default: 6)
  --soak-minima-tail-exclude-samples N
                                 Ignore last N soak samples for DMA minima floors (default: 3)
  --ignore-gps-errors            Suppress GPS advisory warnings in soak scoring
  --no-auto-kill-monitor         Do not auto-stop pio device monitor when port is busy
  --out-dir PATH                 Write reports to PATH
  --dry-run                      Print resolved config and exit
  -h, --help                     Show this help
EOF
}

is_uint() {
  local value="${1:-}"
  [[ "$value" =~ ^[0-9]+$ ]]
}

resolve_rad_timeout_seconds() {
  if [[ -n "$RAD_TIMEOUT_SECONDS" ]]; then
    echo "$RAD_TIMEOUT_SECONDS"
    return
  fi

  # Keep legacy behavior at 100% (25s), then scale linearly for larger/smaller
  # scenarios so high-scale runs don't fail on harness timeout.
  local auto_timeout=$(( (25 * RAD_DURATION_SCALE_PCT + 99) / 100 ))
  if [[ "$auto_timeout" -lt 25 ]]; then
    auto_timeout=25
  fi
  echo "$auto_timeout"
}

add_result() {
  local name="$1"
  local status="$2"
  local metrics="$3"
  local artifact="$4"
  local command="${5:-}"

  TEST_NAMES+=("$name")
  TEST_STATUS+=("$status")
  TEST_METRICS+=("${metrics//$'\n'/; }")
  TEST_ARTIFACTS+=("$artifact")
  TEST_COMMANDS+=("$command")
}

status_color() {
  local status="$1"
  if [[ "$status" == "PASS" ]]; then
    printf "%bPASS%b" "$GREEN" "$NC"
  else
    printf "%bFAIL%b" "$RED" "$NC"
  fi
}

shell_join() {
  local out=""
  local arg
  for arg in "$@"; do
    if [[ -z "$out" ]]; then
      out="$(printf '%q' "$arg")"
    else
      out="$out $(printf '%q' "$arg")"
    fi
  done
  printf "%s" "$out"
}

set_harness_precheck_failure() {
  HARNESS_PRECHECK_CLASS="$1"
  HARNESS_PRECHECK_CODE="$2"
  HARNESS_PRECHECK_REASON="$3"
}

extract_soak_metric() {
  local summary="$1"
  local key="$2"
  case "$key" in
    result) awk '/- Result:/{gsub(/\*| /,"",$3); print $3; exit}' "$summary" ;;
    rx) awk '/- rxPackets delta:/{print $4; exit}' "$summary" ;;
    parse_ok) awk '/- parseSuccesses delta:/{print $4; exit}' "$summary" ;;
    parse_fail) awk '/- parseFailures delta:/{print $4; exit}' "$summary" ;;
    wifi_gate) awk '/- Peak wifiMaxUs used for gate:/{print $7; exit}' "$summary" ;;
    disp_pipe) awk '/- Peak dispPipeMaxUs:/{print $4; exit}' "$summary" ;;
    dma_min) awk '/- Min heapDmaMin \(SLO\):/{print $5; exit}' "$summary" ;;
    dma_largest) awk '/- Min heapDmaLargestMin \(SLO\):/{print $5; exit}' "$summary" ;;
    dma_below_floor)
      awk -F': ' '/- DMA largest current below-floor samples\/total:/{split($2,a," "); print a[1]; exit}' "$summary"
      ;;
    dma_largest_to_free_p50)
      awk -F': ' '/- DMA largest\/free pct min\/p05\/p50:/{n=split($2,a," / "); if (n >= 3) print a[3]; exit}' "$summary"
      ;;
    dma_fragmentation_p95)
      awk -F': ' '/- DMA fragmentation pct p50\/p95\/max:/{n=split($2,a," / "); if (n >= 2) print a[2]; exit}' "$summary"
      ;;
    proxy_off_delta) awk '/- Proxy advertising transition deltas on\/off:/{print $9; exit}' "$summary" ;;
    transition_samples) awk '/- Transition primary samples\/time-to-stable:/{print $5; exit}' "$summary" ;;
    transition_ms)
      awk '/- Transition primary samples\/time-to-stable:/{gsub("ms","",$7); print $7; exit}' "$summary"
      ;;
    proxy_off_samples) awk '/- Proxy off samples\/time-to-stable:/{print $5; exit}' "$summary" ;;
    proxy_off_recovery_ms)
      awk '/- Proxy off samples\/time-to-stable:/{gsub("ms","",$7); print $7; exit}' "$summary"
      ;;
    fail_reasons)
      awk '
        /- Failing checks:/ {in_block=1; next}
        /- Peak sample context/ {in_block=0}
        in_block && /^  - / {
          sub(/^  - /, "")
          printf "%s; ", $0
        }
      ' "$summary"
      ;;
    *) ;;
  esac
}

parse_metrics_payload() {
  local payload="$1"
  python3 - "$payload" <<'PY'
import json
import sys

required = ["rxPackets", "parseSuccesses", "parseFailures", "dispPipeMaxUs", "wifiMaxUs", "displayUpdates"]
try:
    data = json.loads(sys.argv[1])
except Exception as exc:
    print(f"invalid_json={exc}")
    sys.exit(2)

missing = [k for k in required if k not in data]
if missing:
    print("missing=" + ",".join(missing))
    sys.exit(3)

print(
    "rxPackets={rxPackets} parseSuccesses={parseSuccesses} parseFailures={parseFailures} "
    "dispPipeMaxUs={dispPipeMaxUs} wifiMaxUs={wifiMaxUs} displayUpdates={displayUpdates}".format(**data)
)
PY
}

run_soak_test() {
  local test_name="$1"
  shift

  local run_log="$OUT_DIR/${test_name}.log"
  local item_out_dir="$OUT_DIR/${test_name}_artifacts"
  mkdir -p "$item_out_dir"
  local cmd=("./scripts/run_real_fw_soak.sh"
    --duration-seconds "$DURATION_SECONDS"
    --metrics-url "$METRICS_URL"
    --http-timeout-seconds "$HTTP_TIMEOUT_SECONDS"
    --profile "$SOAK_PROFILE"
    --require-metrics
    --min-metrics-ok-samples "$SOAK_MIN_METRICS_OK_SAMPLES"
    --latency-gate-mode "$SOAK_LATENCY_GATE_MODE"
    --latency-robust-min-samples "$SOAK_LATENCY_ROBUST_MIN_SAMPLES"
    --latency-robust-max-exceed-pct "$SOAK_LATENCY_ROBUST_MAX_EXCEED_PCT"
    --wifi-robust-skip-first-samples "$SOAK_WIFI_ROBUST_SKIP_FIRST_SAMPLES"
    --exclude-tail-samples-for-minima "$SOAK_MINIMA_TAIL_EXCLUDE_SAMPLES"
    --out-dir "$item_out_dir")
  if [[ "$IGNORE_GPS_ERRORS" -eq 1 ]]; then
    cmd+=(--ignore-gps-errors)
  fi
  if [[ "$SKIP_FLASH" -eq 1 ]]; then
    cmd+=(--skip-flash)
  fi
  if [[ -n "$TEST_PORT" ]]; then
    cmd+=(--port "$TEST_PORT")
  fi
  while [[ $# -gt 0 ]]; do
    cmd+=("$1")
    shift
  done
  local cmd_text
  cmd_text="$(shell_join "${cmd[@]}")"

  echo -e "${YELLOW}==> Running $test_name...${NC}"
  local rc
  if "${cmd[@]}" >"$run_log" 2>&1; then
    rc=0
  else
    rc=$?
  fi

  local summary
  summary="$item_out_dir/summary.md"
  if [[ ! -f "$summary" ]]; then
    summary="$(awk '/summary:/{s=$2} END{print s}' "$run_log")"
  fi
  local result_word
  result_word="$(awk '/result:/{r=$2} END{print r}' "$run_log")"

  local status="FAIL"
  if [[ "$rc" -eq 0 && "$result_word" == "PASS" ]]; then
    status="PASS"
  fi

  local metrics="rc=$rc result=${result_word:-unknown}"
  if [[ -n "$summary" && -f "$summary" ]]; then
    local rx parse_ok parse_fail wifi_gate disp_pipe dma_min dma_largest dma_below_floor
    local dma_largest_to_free_p50 dma_fragmentation_p95 proxy_off_delta
    local transition_samples transition_ms proxy_off_samples proxy_off_recovery_ms fail_reasons
    rx="$(extract_soak_metric "$summary" rx)"
    parse_ok="$(extract_soak_metric "$summary" parse_ok)"
    parse_fail="$(extract_soak_metric "$summary" parse_fail)"
    wifi_gate="$(extract_soak_metric "$summary" wifi_gate)"
    disp_pipe="$(extract_soak_metric "$summary" disp_pipe)"
    dma_min="$(extract_soak_metric "$summary" dma_min)"
    dma_largest="$(extract_soak_metric "$summary" dma_largest)"
    dma_below_floor="$(extract_soak_metric "$summary" dma_below_floor)"
    dma_largest_to_free_p50="$(extract_soak_metric "$summary" dma_largest_to_free_p50)"
    dma_fragmentation_p95="$(extract_soak_metric "$summary" dma_fragmentation_p95)"
    proxy_off_delta="$(extract_soak_metric "$summary" proxy_off_delta)"
    transition_samples="$(extract_soak_metric "$summary" transition_samples)"
    transition_ms="$(extract_soak_metric "$summary" transition_ms)"
    proxy_off_samples="$(extract_soak_metric "$summary" proxy_off_samples)"
    proxy_off_recovery_ms="$(extract_soak_metric "$summary" proxy_off_recovery_ms)"
    fail_reasons="$(extract_soak_metric "$summary" fail_reasons)"
    metrics+=" rx=${rx:-n/a} parseOK=${parse_ok:-n/a} parseFail=${parse_fail:-n/a} wifiGate=${wifi_gate:-n/a} dispPipe=${disp_pipe:-n/a} dmaMin=${dma_min:-n/a} dmaLargest=${dma_largest:-n/a}"
    metrics+=" dmaBelowFloor=${dma_below_floor:-n/a} dmaLargestToFreeP50=${dma_largest_to_free_p50:-n/a} dmaFragP95=${dma_fragmentation_p95:-n/a}"
    metrics+=" proxyOffDelta=${proxy_off_delta:-n/a} tStableSamples=${transition_samples:-n/a} tStableMs=${transition_ms:-n/a} proxyOffStableSamples=${proxy_off_samples:-n/a} proxyOffStableMs=${proxy_off_recovery_ms:-n/a}"
    if [[ -n "$fail_reasons" ]]; then
      metrics+=" reasons=\"${fail_reasons}\""
    fi
  fi

  local artifact="$run_log"
  if [[ -n "$summary" ]]; then
    artifact="$summary"
  fi
  add_result "$test_name" "$status" "$metrics" "$artifact" "$cmd_text"
}

run_metrics_endpoint_test() {
  local test_name="metrics_endpoint"
  local endpoint="$METRICS_URL"
  if [[ "$endpoint" != *"soak="* ]]; then
    if [[ "$endpoint" == *"?"* ]]; then
      endpoint="${endpoint}&soak=1"
    else
      endpoint="${endpoint}?soak=1"
    fi
  fi

  local payload=""
  local parse_out=""
  local parse_rc=4
  local had_payload=0
  local attempt=1
  local max_wait_seconds=$((METRICS_ENDPOINT_ATTEMPTS * HTTP_TIMEOUT_SECONDS))
  if [[ "$METRICS_ENDPOINT_ATTEMPTS" -gt 1 ]]; then
    max_wait_seconds=$((max_wait_seconds + (METRICS_ENDPOINT_ATTEMPTS - 1) * METRICS_ENDPOINT_RETRY_DELAY_SECONDS))
  fi
  while [[ "$attempt" -le "$METRICS_ENDPOINT_ATTEMPTS" ]]; do
    payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$endpoint" 2>/dev/null || true)"
    if [[ -n "$payload" ]]; then
      had_payload=1
      parse_out="$(parse_metrics_payload "$payload" 2>/dev/null)"
      parse_rc=$?
      if [[ "$parse_rc" -eq 0 ]]; then
        break
      fi
    else
      parse_out="no_response"
      parse_rc=4
    fi
    if [[ "$attempt" -lt "$METRICS_ENDPOINT_ATTEMPTS" && "$METRICS_ENDPOINT_RETRY_DELAY_SECONDS" -gt 0 ]]; then
      sleep "$METRICS_ENDPOINT_RETRY_DELAY_SECONDS"
    fi
    attempt=$((attempt + 1))
  done

  local cmd_text="curl -fsS --max-time $HTTP_TIMEOUT_SECONDS $endpoint (attempts=$METRICS_ENDPOINT_ATTEMPTS retryDelay=${METRICS_ENDPOINT_RETRY_DELAY_SECONDS}s)"
  if [[ "$had_payload" -eq 0 ]]; then
    local reason="Metrics endpoint unreachable after ${METRICS_ENDPOINT_ATTEMPTS} attempt(s) (timeout=${HTTP_TIMEOUT_SECONDS}s, retryDelay=${METRICS_ENDPOINT_RETRY_DELAY_SECONDS}s, boundedWait<=${max_wait_seconds}s)."
    set_harness_precheck_failure \
      "Class C (harness transient)" \
      "HARNESS_PRECHECK_AP_UNREACHABLE" \
      "$reason"
    add_result "$test_name" "FAIL" \
      "class=${HARNESS_PRECHECK_CLASS} code=${HARNESS_PRECHECK_CODE} reason=\"${reason}\"" \
      "$endpoint" \
      "$cmd_text"
    return
  fi

  local metric_text="attempt=${attempt} ${parse_out}"
  if [[ "$parse_rc" -eq 0 ]]; then
    HARNESS_PRECHECK_CLASS=""
    HARNESS_PRECHECK_CODE=""
    HARNESS_PRECHECK_REASON=""
    add_result "$test_name" "PASS" "$metric_text" "$endpoint" "$cmd_text"
  else
    local reason="Metrics endpoint payload invalid for soak parser (parse_rc=${parse_rc}, detail=${parse_out})."
    set_harness_precheck_failure \
      "Class B (harness contract mismatch)" \
      "HARNESS_PRECHECK_INVALID_PAYLOAD" \
      "$reason"
    add_result "$test_name" "FAIL" \
      "class=${HARNESS_PRECHECK_CLASS} code=${HARNESS_PRECHECK_CODE} attempt=${attempt} ${parse_out}" \
      "$endpoint" \
      "$cmd_text"
  fi
}

run_camera_smoke_test() {
  local test_name="camera_smoke"
  local run_log="$OUT_DIR/${test_name}.log"
  local item_out_dir="$OUT_DIR/${test_name}_artifacts"
  mkdir -p "$item_out_dir"
  local cmd=(python3 "./scripts/camera_device_smoke.py"
    --metrics-url "$METRICS_URL"
    --http-timeout-seconds "$HTTP_TIMEOUT_SECONDS"
    --out-dir "$item_out_dir")
  local cmd_text
  cmd_text="$(shell_join "${cmd[@]}")"

  echo -e "${YELLOW}==> Running $test_name...${NC}"
  local rc
  if "${cmd[@]}" >"$run_log" 2>&1; then
    rc=0
  else
    rc=$?
  fi

  local summary="$item_out_dir/summary.md"
  local result_word
  result_word="$(awk '/^result:/{r=$2} END{print r}' "$run_log")"
  local display_delta
  display_delta="$(awk -F= '/^display_updates_delta=/{v=$2} END{print v}' "$run_log")"
  local status="FAIL"
  if [[ "$rc" -eq 0 && "$result_word" == "PASS" ]]; then
    status="PASS"
  fi

  local metrics="rc=$rc result=${result_word:-unknown} displayDelta=${display_delta:-n/a}"
  local failure
  failure="$(awk -F= '/^failure=/{sub(/^failure=/,""); print; exit}' "$run_log")"
  if [[ -n "$failure" ]]; then
    metrics+=" failure=\"${failure}\""
  fi

  local artifact="$run_log"
  if [[ -f "$summary" ]]; then
    artifact="$summary"
  fi
  add_result "$test_name" "$status" "$metrics" "$artifact" "$cmd_text"
}

run_rad_short_test() {
  local test_name="rad_short_${RAD_SCENARIO_ID}"
  local debug_base="${METRICS_URL%%\?*}"
  debug_base="${debug_base%/api/debug/metrics}"
  local rad_wait_timeout="$RESOLVED_RAD_TIMEOUT_SECONDS"

  local rad_log="$OUT_DIR/${test_name}.log"
  local cmd_text
  cmd_text="$(shell_join python3 - "$debug_base" "$RAD_SCENARIO_ID" "$RAD_MIN_RX_DELTA" "$RAD_MIN_PARSE_SUCCESS_DELTA" "$RAD_MIN_DISPLAY_UPDATES_DELTA" "$HTTP_TIMEOUT_SECONDS" "$RAD_DURATION_SCALE_PCT" "$rad_wait_timeout")"
  local rc
  if python3 - "$debug_base" "$RAD_SCENARIO_ID" "$RAD_MIN_RX_DELTA" "$RAD_MIN_PARSE_SUCCESS_DELTA" "$RAD_MIN_DISPLAY_UPDATES_DELTA" "$HTTP_TIMEOUT_SECONDS" "$RAD_DURATION_SCALE_PCT" "$rad_wait_timeout" >"$rad_log" <<'PY'
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
    keys = ["rxPackets", "parseSuccesses", "parseFailures", "displayUpdates", "dispPipeMaxUs", "wifiMaxUs"]
    return {k: int(metrics.get(k, 0) or 0) for k in keys}

ok = True
reasons = []
keys = ["rxPackets", "parseSuccesses", "parseFailures", "displayUpdates", "dispPipeMaxUs", "wifiMaxUs"]
reset_resp = {"success": False}
start_resp = {"success": False}
status = {}
pre = {k: 0 for k in keys}
post = {k: 0 for k in keys}
delta = {k: 0 for k in keys}
status_poll_errors = 0
status_poll_last_error = ""
cleanup_stop_success = False
cleanup_stop_error = ""

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
            reasons.append(f"scenario timeout waiting for completion (> {scenario_timeout_s}s)")
            ok = False
            break
        try:
            status = get_json("/api/debug/v1-scenario/status")
        except Exception as status_exc:
            status_poll_errors += 1
            status_poll_last_error = str(status_exc)
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
    if status.get("running", False):
        ok = False
        reasons.append("scenario still running at end")
    if int(status.get("eventsTotal", 0)) <= 0:
        ok = False
        reasons.append("eventsTotal is 0")
    if int(status.get("eventsEmitted", 0)) < int(status.get("eventsTotal", 0)):
        ok = False
        reasons.append("eventsEmitted < eventsTotal")
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
    # Prevent scenario bleed-through into subsequent soak items.
    try:
        stop_resp = post_json("/api/debug/v1-scenario/stop")
        cleanup_stop_success = bool(stop_resp.get("success"))
    except Exception as stop_exc:
        cleanup_stop_success = False
        cleanup_stop_error = str(stop_exc)

if not cleanup_stop_success:
    ok = False
    if cleanup_stop_error:
        reasons.append(f"scenario cleanup stop exception: {cleanup_stop_error}")
    else:
        reasons.append("scenario cleanup stop failed")

print(f"scenario={scenario_id}")
print(f"durationScalePct={scale_pct}")
print(f"timeoutSec={scenario_timeout_s}")
print(f"start_success={int(bool(start_resp.get('success')))} reset_success={int(bool(reset_resp.get('success')))} completedRuns={status.get('completedRuns', 'n/a')} events={status.get('eventsEmitted', 'n/a')}/{status.get('eventsTotal', 'n/a')} durationMs={status.get('durationMs', 'n/a')}")
print(f"delta_rxPackets={delta['rxPackets']} delta_parseSuccesses={delta['parseSuccesses']} delta_parseFailures={delta['parseFailures']} delta_displayUpdates={delta['displayUpdates']}")
print(f"peak_dispPipeMaxUs={post['dispPipeMaxUs']} peak_wifiMaxUs={post['wifiMaxUs']}")
print(f"status_poll_errors={status_poll_errors} cleanup_stop_success={int(cleanup_stop_success)}")
if status_poll_last_error:
    print(f"status_poll_last_error={status_poll_last_error}")
if reasons:
    print("reasons=" + "; ".join(reasons))

sys.exit(0 if ok else 1)
PY
  then
    rc=0
  else
    rc=$?
  fi

  local metrics
  metrics="$(tr '\n' '; ' < "$rad_log" | sed 's/; $//')"
  if [[ "$rc" -eq 0 ]]; then
    add_result "$test_name" "PASS" "$metrics" "$rad_log" "$cmd_text"
  else
    add_result "$test_name" "FAIL" "$metrics" "$rad_log" "$cmd_text"
  fi
}

run_camera_radar_overlap_test() {
  local test_name="camera_radar_overlap"
  local run_log="$OUT_DIR/${test_name}.log"
  local item_out_dir="$OUT_DIR/${test_name}_artifacts"
  mkdir -p "$item_out_dir"
  local cmd=(python3 "./scripts/camera_radar_device_stress.py"
    --metrics-url "$METRICS_URL"
    --http-timeout-seconds "$HTTP_TIMEOUT_SECONDS"
    --out-dir "$item_out_dir"
    --scenario-id "$RAD_SCENARIO_ID"
    --duration-scale-pct "$RAD_DURATION_SCALE_PCT"
    --mode both
    --flap-cycles "$SOAK_TRANSITION_FLAP_CYCLES"
    --flap-interval-seconds "$SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS")
  local cmd_text
  cmd_text="$(shell_join "${cmd[@]}")"

  echo -e "${YELLOW}==> Running $test_name...${NC}"
  local rc
  if "${cmd[@]}" >"$run_log" 2>&1; then
    rc=0
  else
    rc=$?
  fi

  local summary="$item_out_dir/summary.md"
  local details="$item_out_dir/details.json"
  local result_word
  result_word="$(awk '/^result:/{r=$2} END{print r}' "$run_log")"
  local status="FAIL"
  if [[ "$rc" -eq 0 && "$result_word" == "PASS" ]]; then
    status="PASS"
  fi

  local metrics="rc=$rc result=${result_word:-unknown}"
  if [[ -f "$details" ]]; then
    local detail_metrics
    detail_metrics="$(python3 - "$details" <<'PY'
import json
import sys

with open(sys.argv[1], "r", encoding="utf-8") as f:
    data = json.load(f)

phases = {phase.get("phase"): phase for phase in data.get("phases", [])}
parts = []
for name in ("overlap", "flap"):
    phase = phases.get(name)
    if not phase:
        continue
    metrics = phase.get("metrics", {})
    parts.append(f"{name}={phase.get('result', 'unknown')}")
    parts.append(f"{name}_camPeak={metrics.get('camera_correlated_disp_pipe_peak_us', 'n/a')}")
    parts.append(f"{name}_camSamples={metrics.get('camera_correlated_disp_pipe_sample_count', 'n/a')}")
    parts.append(f"{name}_dispPeak={metrics.get('dispPipeMaxUs_peak', 'n/a')}")
    parts.append(f"{name}_parseFail={metrics.get('parseFailures_delta', 'n/a')}")
    parts.append(f"{name}_qDrop={metrics.get('queueDrops_delta', 'n/a')}")
print(" ".join(parts))
PY
)"
    if [[ -n "$detail_metrics" ]]; then
      metrics+=" $detail_metrics"
    fi
  fi

  local failure
  failure="$(awk -F= '/_failure=/{sub(/^[^=]*=/,""); print; exit}' "$run_log")"
  if [[ -n "$failure" ]]; then
    metrics+=" failure=\"${failure}\""
  fi

  local artifact="$run_log"
  if [[ -f "$summary" ]]; then
    artifact="$summary"
  fi
  add_result "$test_name" "$status" "$metrics" "$artifact" "$cmd_text"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --metrics-url)
      [[ $# -lt 2 ]] && { echo "Missing value for --metrics-url" >&2; exit 2; }
      METRICS_URL="$2"
      shift
      ;;
    --duration-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --duration-seconds" >&2; exit 2; }
      DURATION_SECONDS="$2"
      shift
      ;;
    --http-timeout-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --http-timeout-seconds" >&2; exit 2; }
      HTTP_TIMEOUT_SECONDS="$2"
      shift
      ;;
    --port)
      [[ $# -lt 2 ]] && { echo "Missing value for --port" >&2; exit 2; }
      TEST_PORT="$2"
      shift
      ;;
    --skip-flash)
      SKIP_FLASH=1
      ;;
    --with-flash)
      SKIP_FLASH=0
      ;;
    --rad-scenario)
      [[ $# -lt 2 ]] && { echo "Missing value for --rad-scenario" >&2; exit 2; }
      RAD_SCENARIO_ID="$2"
      shift
      ;;
    --rad-duration-scale-pct)
      [[ $# -lt 2 ]] && { echo "Missing value for --rad-duration-scale-pct" >&2; exit 2; }
      RAD_DURATION_SCALE_PCT="$2"
      shift
      ;;
    --rad-timeout-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --rad-timeout-seconds" >&2; exit 2; }
      RAD_TIMEOUT_SECONDS="$2"
      shift
      ;;
    --camera-radar-stress)
      CAMERA_RADAR_STRESS=1
      ;;
    --no-camera-radar-stress)
      CAMERA_RADAR_STRESS=0
      ;;
    --no-transition-soak)
      SOAK_ENABLE_TRANSITION_QUAL=0
      ;;
    --transition-flap-cycles)
      [[ $# -lt 2 ]] && { echo "Missing value for --transition-flap-cycles" >&2; exit 2; }
      SOAK_TRANSITION_FLAP_CYCLES="$2"
      SOAK_TRANSITION_MIN_PROXY_ADV_OFF_TRANSITIONS="$2"
      shift
      ;;
    --transition-drive-interval-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --transition-drive-interval-seconds" >&2; exit 2; }
      SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS="$2"
      shift
      ;;
    --transition-max-recovery-ms)
      [[ $# -lt 2 ]] && { echo "Missing value for --transition-max-recovery-ms" >&2; exit 2; }
      SOAK_TRANSITION_MAX_PROXY_RECOVERY_MS="$2"
      shift
      ;;
    --transition-max-samples)
      [[ $# -lt 2 ]] && { echo "Missing value for --transition-max-samples" >&2; exit 2; }
      SOAK_TRANSITION_MAX_SAMPLES_TO_STABLE="$2"
      shift
      ;;
    --soak-minima-tail-exclude-samples)
      [[ $# -lt 2 ]] && { echo "Missing value for --soak-minima-tail-exclude-samples" >&2; exit 2; }
      SOAK_MINIMA_TAIL_EXCLUDE_SAMPLES="$2"
      shift
      ;;
    --ignore-gps-errors)
      IGNORE_GPS_ERRORS=1
      ;;
    --no-auto-kill-monitor)
      AUTO_KILL_MONITOR=0
      ;;
    --out-dir)
      [[ $# -lt 2 ]] && { echo "Missing value for --out-dir" >&2; exit 2; }
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

for n in \
  "$DURATION_SECONDS" \
  "$HTTP_TIMEOUT_SECONDS" \
  "$METRICS_ENDPOINT_ATTEMPTS" \
  "$METRICS_ENDPOINT_RETRY_DELAY_SECONDS" \
  "$RAD_MIN_RX_DELTA" \
  "$RAD_MIN_PARSE_SUCCESS_DELTA" \
  "$RAD_MIN_DISPLAY_UPDATES_DELTA" \
  "$RAD_DURATION_SCALE_PCT" \
  "$SOAK_MINIMA_TAIL_EXCLUDE_SAMPLES" \
  "$SOAK_TRANSITION_FLAP_CYCLES" \
  "$SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS" \
  "$SOAK_TRANSITION_MAX_PROXY_RECOVERY_MS" \
  "$SOAK_TRANSITION_MAX_SAMPLES_TO_STABLE"
do
  if ! is_uint "$n"; then
    echo "Invalid numeric option value '$n'." >&2
    exit 2
  fi
done
if [[ "$DURATION_SECONDS" -lt 1 ]]; then
  echo "--duration-seconds must be >= 1." >&2
  exit 2
fi
if [[ "$HTTP_TIMEOUT_SECONDS" -lt 1 ]]; then
  echo "--http-timeout-seconds must be >= 1." >&2
  exit 2
fi
if [[ "$METRICS_ENDPOINT_ATTEMPTS" -lt 1 ]]; then
  echo "metrics endpoint attempts must be >= 1." >&2
  exit 2
fi
if [[ "$RAD_DURATION_SCALE_PCT" -lt 25 || "$RAD_DURATION_SCALE_PCT" -gt 1000 ]]; then
  echo "--rad-duration-scale-pct must be in 25..1000." >&2
  exit 2
fi
if [[ "$SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS" -lt 1 ]]; then
  echo "--transition-drive-interval-seconds must be >= 1." >&2
  exit 2
fi
if [[ -n "$RAD_TIMEOUT_SECONDS" ]]; then
  if ! is_uint "$RAD_TIMEOUT_SECONDS"; then
    echo "--rad-timeout-seconds must be a positive integer." >&2
    exit 2
  fi
  if [[ "$RAD_TIMEOUT_SECONDS" -lt 1 ]]; then
    echo "--rad-timeout-seconds must be >= 1." >&2
    exit 2
  fi
fi

RESOLVED_RAD_TIMEOUT_SECONDS="$(resolve_rad_timeout_seconds)"
METRICS_PRECHECK_MAX_WAIT_SECONDS=$((METRICS_ENDPOINT_ATTEMPTS * HTTP_TIMEOUT_SECONDS))
if [[ "$METRICS_ENDPOINT_ATTEMPTS" -gt 1 ]]; then
  METRICS_PRECHECK_MAX_WAIT_SECONDS=$((METRICS_PRECHECK_MAX_WAIT_SECONDS + (METRICS_ENDPOINT_ATTEMPTS - 1) * METRICS_ENDPOINT_RETRY_DELAY_SECONDS))
fi

if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="$ROOT_DIR/.artifacts/test_reports/device_test_$(date +%Y%m%d_%H%M%S)"
fi
mkdir -p "$OUT_DIR"

TSV_PATH="$OUT_DIR/results.tsv"
SUMMARY_MD="$OUT_DIR/summary.md"
printf "test\tstatus\tmetrics\tartifact\n" > "$TSV_PATH"

echo "╔════════════════════════════════════════════════════╗"
echo "║                 Device Test Suite                 ║"
echo "╚════════════════════════════════════════════════════╝"
echo ""
echo "Config:"
echo "  metrics url: $METRICS_URL"
echo "  duration: ${DURATION_SECONDS}s"
echo "  http timeout: ${HTTP_TIMEOUT_SECONDS}s"
echo "  metrics endpoint retries: attempts=${METRICS_ENDPOINT_ATTEMPTS} delay=${METRICS_ENDPOINT_RETRY_DELAY_SECONDS}s boundedWait<=${METRICS_PRECHECK_MAX_WAIT_SECONDS}s"
echo "  skip flash: $SKIP_FLASH"
echo "  port: ${TEST_PORT:-auto}"
echo "  suite profile: $SUITE_PROFILE_VERSION"
echo "  soak profile: $SOAK_PROFILE"
echo "  soak robust gate: mode=$SOAK_LATENCY_GATE_MODE minSamples=$SOAK_LATENCY_ROBUST_MIN_SAMPLES maxExceedPct=$SOAK_LATENCY_ROBUST_MAX_EXCEED_PCT wifiSkipFirst=$SOAK_WIFI_ROBUST_SKIP_FIRST_SAMPLES"
echo "  soak minima tail exclusion: ${SOAK_MINIMA_TAIL_EXCLUDE_SAMPLES} sample(s)"
echo "  soak require-metrics: yes (min ok samples=$SOAK_MIN_METRICS_OK_SAMPLES)"
echo "  camera smoke: api/ui/render on hardware"
echo "  display drive: displayInterval=${SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS}s minDisplayUpdatesDelta=$SOAK_MIN_DISPLAY_UPDATES_DELTA"
echo "  transition qual: enabled=$SOAK_ENABLE_TRANSITION_QUAL flapCycles=$SOAK_TRANSITION_FLAP_CYCLES interval=${SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS}s maxRecoveryMs=$SOAK_TRANSITION_MAX_PROXY_RECOVERY_MS maxSamples=$SOAK_TRANSITION_MAX_SAMPLES_TO_STABLE"
echo "  RAD scenario: $RAD_SCENARIO_ID scalePct=$RAD_DURATION_SCALE_PCT timeout=${RESOLVED_RAD_TIMEOUT_SECONDS}s (rx>=$RAD_MIN_RX_DELTA parse>=$RAD_MIN_PARSE_SUCCESS_DELTA display>=$RAD_MIN_DISPLAY_UPDATES_DELTA parseFail==0)"
echo "  camera+radar stress: enabled=$CAMERA_RADAR_STRESS mode=both flapCycles=$SOAK_TRANSITION_FLAP_CYCLES flapInterval=${SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS}s"
echo "  out dir: $OUT_DIR"
echo ""

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo -e "${BLUE}Dry run complete (no tests executed).${NC}"
  exit 0
fi

if [[ "$AUTO_KILL_MONITOR" -eq 1 ]]; then
  monitor_pids="$(pgrep -f 'pio device monitor -e waveshare-349' || true)"
  if [[ -n "$monitor_pids" ]]; then
    echo -e "${YELLOW}Stopping active pio monitor(s): $monitor_pids${NC}"
    # shellcheck disable=SC2086
    kill $monitor_pids || true
    sleep 1
  fi
fi

check_uptime_continuity "before metrics_endpoint"
run_metrics_endpoint_test
check_uptime_continuity "after metrics_endpoint"

if [[ -n "$HARNESS_PRECHECK_CODE" ]]; then
  SUITE_EXIT_REASON="$HARNESS_PRECHECK_CODE"
  echo -e "${YELLOW}Preflight failed (${HARNESS_PRECHECK_CODE}); aborting remaining test items to avoid non-actionable failures.${NC}"
else
  check_uptime_continuity "before camera_smoke"
  run_camera_smoke_test
  check_uptime_continuity "after camera_smoke"

  check_uptime_continuity "before rad_short"
  run_rad_short_test
  check_uptime_continuity "after rad_short"

  if [[ "$CAMERA_RADAR_STRESS" -eq 1 ]]; then
    check_uptime_continuity "before camera_radar_overlap"
    run_camera_radar_overlap_test
    check_uptime_continuity "after camera_radar_overlap"
  fi

  check_uptime_continuity "before soak_display"
  run_soak_test "soak_display" \
    --drive-display-preview \
    --display-drive-interval-seconds "$SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS" \
    --min-display-updates-delta "$SOAK_MIN_DISPLAY_UPDATES_DELTA"
  check_uptime_continuity "after soak_display"

  check_uptime_continuity "before soak_core"
  run_soak_test "soak_core"
  check_uptime_continuity "after soak_core"

  if [[ "$SOAK_ENABLE_TRANSITION_QUAL" -eq 1 ]]; then
    check_uptime_continuity "before soak_transition"
    run_soak_test "soak_transition" \
      --drive-transition-flaps \
      --transition-drive-interval-seconds "$SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS" \
      --transition-flap-cycles "$SOAK_TRANSITION_FLAP_CYCLES" \
      --min-proxy-adv-off-transitions "$SOAK_TRANSITION_MIN_PROXY_ADV_OFF_TRANSITIONS" \
      --max-time-to-stable-ms-after-proxy-adv-off "$SOAK_TRANSITION_MAX_PROXY_RECOVERY_MS" \
      --max-samples-to-stable "$SOAK_TRANSITION_MAX_SAMPLES_TO_STABLE"
    check_uptime_continuity "after soak_transition"
  fi
fi

# If reboots were detected between test items, add a synthetic failure.
if [[ "$suite_reboot_count" -gt 0 ]]; then
  add_result "uptime_continuity" "FAIL" \
    "Device rebooted ${suite_reboot_count} time(s) during suite (detected via uptimeMs regression between test items)" \
    "$OUT_DIR/summary.md" \
    "uptime continuity check"
fi

pass_count=0
fail_count=0

echo ""
echo "Per-item results:"
for i in "${!TEST_NAMES[@]}"; do
  name="${TEST_NAMES[$i]}"
  status="${TEST_STATUS[$i]}"
  metrics="${TEST_METRICS[$i]}"
  artifact="${TEST_ARTIFACTS[$i]}"
  if [[ "$status" == "PASS" ]]; then
    pass_count=$((pass_count + 1))
  else
    fail_count=$((fail_count + 1))
  fi
  printf "  %-24s %s\n" "$name" "$(status_color "$status")"
  printf "    metrics: %s\n" "$metrics"
  printf "    artifact: %s\n" "$artifact"
  printf "%s\t%s\t%s\t%s\n" "$name" "$status" "$metrics" "$artifact" >> "$TSV_PATH"
done

overall_result="PASS"
if [[ "$fail_count" -gt 0 ]]; then
  overall_result="FAIL"
fi

if [[ -z "$SUITE_EXIT_REASON" ]]; then
  if [[ "$overall_result" == "PASS" ]]; then
    SUITE_EXIT_REASON="PASS"
  else
    SUITE_EXIT_REASON="TEST_ITEM_FAILURES"
  fi
fi

{
  echo "# Device Test Summary"
  echo ""
  echo "- Result: **$overall_result**"
  echo "- Suite profile: \`$SUITE_PROFILE_VERSION\`"
  echo "- Timestamp (UTC): $(date -u +"%Y-%m-%dT%H:%M:%SZ")"
  echo "- Metrics URL: \`$METRICS_URL\`"
  echo "- Soak duration (s): $DURATION_SECONDS"
  echo "- Skip flash: $SKIP_FLASH"
  echo "- Port: \`${TEST_PORT:-auto}\`"
  echo "- Passed: $pass_count"
  echo "- Failed: $fail_count"
  echo "- Suite reboot detections: $suite_reboot_count"
  echo "- Exit reason: \`$SUITE_EXIT_REASON\`"
  if [[ -n "$suite_reboot_log" ]]; then
    echo ""
    echo "## Reboot Detections"
    echo ""
    printf "%b" "$suite_reboot_log" | while IFS= read -r line; do
      echo "- $line"
    done
  fi
  if [[ -n "$HARNESS_PRECHECK_CODE" ]]; then
    echo ""
    echo "## Harness Preflight Classification"
    echo ""
    echo "- Classification: \`$HARNESS_PRECHECK_CLASS\`"
    echo "- Code: \`$HARNESS_PRECHECK_CODE\`"
    echo "- Reason: $HARNESS_PRECHECK_REASON"
  fi
  echo ""
  echo "## Standardized Gates"
  echo ""
  echo "- Soak profile: \`$SOAK_PROFILE\`"
  echo "- Soak metrics required: yes (\`--min-metrics-ok-samples $SOAK_MIN_METRICS_OK_SAMPLES\`)"
  echo "- Camera smoke: \`scripts/camera_device_smoke.py\` (settings API, /cameras UI render, debug camera display render)"
  echo "- Soak robust latency gate: \`mode=$SOAK_LATENCY_GATE_MODE min_samples=$SOAK_LATENCY_ROBUST_MIN_SAMPLES max_exceed_pct=$SOAK_LATENCY_ROBUST_MAX_EXCEED_PCT wifi_skip_first=$SOAK_WIFI_ROBUST_SKIP_FIRST_SAMPLES\`"
  echo "- Display drive defaults: \`display_interval_s=$SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS min_display_updates_delta=$SOAK_MIN_DISPLAY_UPDATES_DELTA\`"
  echo "- Camera+radar overlap: \`enabled=$CAMERA_RADAR_STRESS scenario=$RAD_SCENARIO_ID scale_pct=$RAD_DURATION_SCALE_PCT mode=both flap_cycles=$SOAK_TRANSITION_FLAP_CYCLES flap_interval_s=$SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS\`"
  echo "- Transition qualification: \`enabled=$SOAK_ENABLE_TRANSITION_QUAL flap_cycles=$SOAK_TRANSITION_FLAP_CYCLES interval_s=$SOAK_TRANSITION_DRIVE_INTERVAL_SECONDS min_proxy_off_transitions=$SOAK_TRANSITION_MIN_PROXY_ADV_OFF_TRANSITIONS max_proxy_recovery_ms=$SOAK_TRANSITION_MAX_PROXY_RECOVERY_MS max_samples_to_stable=$SOAK_TRANSITION_MAX_SAMPLES_TO_STABLE\`"
  echo "- RAD short default gates: \`scenario=$RAD_SCENARIO_ID duration_scale_pct=$RAD_DURATION_SCALE_PCT timeout_s=$RESOLVED_RAD_TIMEOUT_SECONDS rx_delta>=$RAD_MIN_RX_DELTA parse_success_delta>=$RAD_MIN_PARSE_SUCCESS_DELTA display_updates_delta>=$RAD_MIN_DISPLAY_UPDATES_DELTA parse_fail_delta==0\`"
  echo ""
  echo "## Item Results"
  echo ""
  echo "| Item | Result | Artifact |"
  echo "|------|--------|----------|"
  for i in "${!TEST_NAMES[@]}"; do
    echo "| ${TEST_NAMES[$i]} | **${TEST_STATUS[$i]}** | \`${TEST_ARTIFACTS[$i]}\` |"
  done
  echo ""
  echo "## Item Metrics"
  echo ""
  for i in "${!TEST_NAMES[@]}"; do
    echo "### ${TEST_NAMES[$i]}"
    echo ""
    echo "- Result: **${TEST_STATUS[$i]}**"
    echo "- Metrics: \`${TEST_METRICS[$i]}\`"
    if [[ -n "${TEST_COMMANDS[$i]}" ]]; then
      echo "- Command: \`${TEST_COMMANDS[$i]}\`"
    fi
    echo "- Artifact: \`${TEST_ARTIFACTS[$i]}\`"
    echo ""
  done
  echo "## Failures"
  echo ""
  if [[ "$fail_count" -eq 0 ]]; then
    echo "- none"
  else
    for i in "${!TEST_NAMES[@]}"; do
      if [[ "${TEST_STATUS[$i]}" == "FAIL" ]]; then
        echo "- ${TEST_NAMES[$i]}: \`${TEST_METRICS[$i]}\`"
      fi
    done
  fi
  echo ""
  echo "## Artifacts"
  echo ""
  echo "- Summary: \`$SUMMARY_MD\`"
  echo "- TSV: \`$TSV_PATH\`"
} > "$SUMMARY_MD"

echo ""
echo "Summary files:"
echo "  $SUMMARY_MD"
echo "  $TSV_PATH"

if [[ "$fail_count" -gt 0 ]]; then
  echo -e "${RED}Device test suite FAILED ($fail_count item(s)); reason=$SUITE_EXIT_REASON.${NC}"
  exit 1
fi

echo -e "${GREEN}Device test suite PASSED ($pass_count item(s)).${NC}"
exit 0
