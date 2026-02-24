#!/usr/bin/env bash
#
# device-test.sh - Hardware test suite runner with per-item PASS/FAIL output
# and metrics evidence for each decision.
#
# Intended workflow:
#   1) Sanity check debug metrics endpoint
#   2) Run a short radar scenario (default: RAD-03) and verify parser/display deltas
#   3) Run real firmware soak (display + camera stress)
#   4) Run real firmware soak (core only)
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
DURATION_SECONDS=60
SKIP_FLASH=1
TEST_PORT="${DEVICE_PORT:-}"
AUTO_KILL_MONITOR=1
DRY_RUN=0

SUITE_PROFILE_VERSION="device_v1"
SOAK_PROFILE="drive_wifi_ap"
SOAK_MIN_METRICS_OK_SAMPLES=3
SOAK_LATENCY_GATE_MODE="hybrid"
SOAK_LATENCY_ROBUST_MIN_SAMPLES=8
SOAK_LATENCY_ROBUST_MAX_EXCEED_PCT=5
SOAK_WIFI_ROBUST_SKIP_FIRST_SAMPLES=2
SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS=1
SOAK_CAMERA_DRIVE_INTERVAL_SECONDS=2
SOAK_CAMERA_DEMO_DURATION_MS=3800
SOAK_MIN_DISPLAY_UPDATES_DELTA=1

RAD_SCENARIO_ID="RAD-03"
RAD_MIN_RX_DELTA=20
RAD_MIN_PARSE_SUCCESS_DELTA=20
RAD_MIN_DISPLAY_UPDATES_DELTA=10

OUT_DIR=""

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
    --out-dir "$item_out_dir")
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
    local rx parse_ok parse_fail wifi_gate disp_pipe dma_min dma_largest fail_reasons
    rx="$(extract_soak_metric "$summary" rx)"
    parse_ok="$(extract_soak_metric "$summary" parse_ok)"
    parse_fail="$(extract_soak_metric "$summary" parse_fail)"
    wifi_gate="$(extract_soak_metric "$summary" wifi_gate)"
    disp_pipe="$(extract_soak_metric "$summary" disp_pipe)"
    dma_min="$(extract_soak_metric "$summary" dma_min)"
    dma_largest="$(extract_soak_metric "$summary" dma_largest)"
    fail_reasons="$(extract_soak_metric "$summary" fail_reasons)"
    metrics+=" rx=${rx:-n/a} parseOK=${parse_ok:-n/a} parseFail=${parse_fail:-n/a} wifiGate=${wifi_gate:-n/a} dispPipe=${disp_pipe:-n/a} dmaMin=${dma_min:-n/a} dmaLargest=${dma_largest:-n/a}"
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

  local payload
  payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$endpoint" 2>/dev/null || true)"
  if [[ -z "$payload" ]]; then
    add_result "$test_name" "FAIL" "No response from $endpoint" "$endpoint" "curl -fsS --max-time $HTTP_TIMEOUT_SECONDS $endpoint"
    return
  fi

  local parse_out
  local rc
  parse_out="$(python3 - "$payload" <<'PY'
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
)"
  rc=$?

  if [[ "$rc" -eq 0 ]]; then
    add_result "$test_name" "PASS" "$parse_out" "$endpoint" "curl -fsS --max-time $HTTP_TIMEOUT_SECONDS $endpoint"
  else
    add_result "$test_name" "FAIL" "$parse_out" "$endpoint" "curl -fsS --max-time $HTTP_TIMEOUT_SECONDS $endpoint"
  fi
}

run_rad_short_test() {
  local test_name="rad_short_${RAD_SCENARIO_ID}"
  local debug_base="${METRICS_URL%%\?*}"
  debug_base="${debug_base%/api/debug/metrics}"

  local rad_log="$OUT_DIR/${test_name}.log"
  local cmd_text
  cmd_text="$(shell_join python3 - "$debug_base" "$RAD_SCENARIO_ID" "$RAD_MIN_RX_DELTA" "$RAD_MIN_PARSE_SUCCESS_DELTA" "$RAD_MIN_DISPLAY_UPDATES_DELTA" "$HTTP_TIMEOUT_SECONDS")"
  local rc
  if python3 - "$debug_base" "$RAD_SCENARIO_ID" "$RAD_MIN_RX_DELTA" "$RAD_MIN_PARSE_SUCCESS_DELTA" "$RAD_MIN_DISPLAY_UPDATES_DELTA" "$HTTP_TIMEOUT_SECONDS" >"$rad_log" <<'PY'
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

try:
    reset_resp = post_json("/api/debug/metrics/reset")
    pre = subset(get_json("/api/debug/metrics?soak=1"))
    start_resp = post_json("/api/debug/v1-scenario/start", {
        "id": scenario_id,
        "loop": False,
        "streamRepeatMs": 700,
        "durationScalePct": 100,
    })

    status = {}
    t0 = time.time()
    while True:
        status = get_json("/api/debug/v1-scenario/status")
        if not status.get("running", False):
            break
        if time.time() - t0 > 25:
            reasons.append("scenario timeout waiting for completion")
            ok = False
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

    print(f"scenario={scenario_id}")
    print(f"start_success={int(bool(start_resp.get('success')))} reset_success={int(bool(reset_resp.get('success')))} completedRuns={status.get('completedRuns', 'n/a')} events={status.get('eventsEmitted', 'n/a')}/{status.get('eventsTotal', 'n/a')} durationMs={status.get('durationMs', 'n/a')}")
    print(f"delta_rxPackets={delta['rxPackets']} delta_parseSuccesses={delta['parseSuccesses']} delta_parseFailures={delta['parseFailures']} delta_displayUpdates={delta['displayUpdates']}")
    print(f"peak_dispPipeMaxUs={post['dispPipeMaxUs']} peak_wifiMaxUs={post['wifiMaxUs']}")
    if reasons:
        print("reasons=" + "; ".join(reasons))
except Exception as exc:
    ok = False
    print(f"reasons=exception: {exc}")

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

for n in "$DURATION_SECONDS" "$HTTP_TIMEOUT_SECONDS" "$RAD_MIN_RX_DELTA" "$RAD_MIN_PARSE_SUCCESS_DELTA" "$RAD_MIN_DISPLAY_UPDATES_DELTA"; do
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
echo "  skip flash: $SKIP_FLASH"
echo "  port: ${TEST_PORT:-auto}"
echo "  suite profile: $SUITE_PROFILE_VERSION"
echo "  soak profile: $SOAK_PROFILE"
echo "  soak robust gate: mode=$SOAK_LATENCY_GATE_MODE minSamples=$SOAK_LATENCY_ROBUST_MIN_SAMPLES maxExceedPct=$SOAK_LATENCY_ROBUST_MAX_EXCEED_PCT wifiSkipFirst=$SOAK_WIFI_ROBUST_SKIP_FIRST_SAMPLES"
echo "  soak require-metrics: yes (min ok samples=$SOAK_MIN_METRICS_OK_SAMPLES)"
echo "  display/camera drive: displayInterval=${SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS}s cameraInterval=${SOAK_CAMERA_DRIVE_INTERVAL_SECONDS}s cameraDurationMs=$SOAK_CAMERA_DEMO_DURATION_MS minDisplayUpdatesDelta=$SOAK_MIN_DISPLAY_UPDATES_DELTA"
echo "  RAD scenario: $RAD_SCENARIO_ID (rx>=$RAD_MIN_RX_DELTA parse>=$RAD_MIN_PARSE_SUCCESS_DELTA display>=$RAD_MIN_DISPLAY_UPDATES_DELTA parseFail==0)"
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

run_metrics_endpoint_test
run_rad_short_test
run_soak_test "soak_display_camera" \
  --drive-display-preview \
  --display-drive-interval-seconds "$SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS" \
  --min-display-updates-delta "$SOAK_MIN_DISPLAY_UPDATES_DELTA" \
  --drive-camera-demo \
  --camera-drive-interval-seconds "$SOAK_CAMERA_DRIVE_INTERVAL_SECONDS" \
  --camera-demo-duration-ms "$SOAK_CAMERA_DEMO_DURATION_MS"
run_soak_test "soak_core"

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
  echo ""
  echo "## Standardized Gates"
  echo ""
  echo "- Soak profile: \`$SOAK_PROFILE\`"
  echo "- Soak metrics required: yes (\`--min-metrics-ok-samples $SOAK_MIN_METRICS_OK_SAMPLES\`)"
  echo "- Soak robust latency gate: \`mode=$SOAK_LATENCY_GATE_MODE min_samples=$SOAK_LATENCY_ROBUST_MIN_SAMPLES max_exceed_pct=$SOAK_LATENCY_ROBUST_MAX_EXCEED_PCT wifi_skip_first=$SOAK_WIFI_ROBUST_SKIP_FIRST_SAMPLES\`"
  echo "- Display/camera drive defaults: \`display_interval_s=$SOAK_DISPLAY_DRIVE_INTERVAL_SECONDS camera_interval_s=$SOAK_CAMERA_DRIVE_INTERVAL_SECONDS camera_duration_ms=$SOAK_CAMERA_DEMO_DURATION_MS min_display_updates_delta=$SOAK_MIN_DISPLAY_UPDATES_DELTA\`"
  echo "- RAD short default gates: \`scenario=$RAD_SCENARIO_ID rx_delta>=$RAD_MIN_RX_DELTA parse_success_delta>=$RAD_MIN_PARSE_SUCCESS_DELTA display_updates_delta>=$RAD_MIN_DISPLAY_UPDATES_DELTA parse_fail_delta==0\`"
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
  echo -e "${RED}Device test suite FAILED ($fail_count item(s)).${NC}"
  exit 1
fi

echo -e "${GREEN}Device test suite PASSED ($pass_count item(s)).${NC}"
exit 0
