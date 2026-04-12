#!/usr/bin/env bash
#
# run_real_fw_soak.sh - Flash production firmware and soak-test the real runtime
# on hardware with serial crash detection and optional debug API polling.
#
# Usage examples:
#   ./scripts/run_real_fw_soak.sh --duration-seconds 600
#   ./scripts/run_real_fw_soak.sh --duration-seconds 1800 --metrics-url http://<DEVICE_IP>/api/debug/metrics
#   ./scripts/run_real_fw_soak.sh --skip-flash --duration-seconds 900 --metrics-url http://<DEVICE_IP>/api/debug/metrics --drive-display-preview
#   ./scripts/run_real_fw_soak.sh --skip-flash --duration-seconds 300 --no-metrics
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

ENV_NAME="waveshare-349"
TEST_PORT="${DEVICE_PORT:-}"
PORT_LOCKED=0
if [[ -n "$TEST_PORT" ]]; then
  PORT_LOCKED=1
fi

DURATION_SECONDS=300
POLL_SECONDS=5
SERIAL_BAUD="${REAL_FW_SERIAL_BAUD:-115200}"
HTTP_TIMEOUT_SECONDS="${REAL_FW_HTTP_TIMEOUT_SECONDS:-2}"
METRICS_ENDPOINT_RETRY_DELAY_SECONDS="${REAL_FW_METRICS_ENDPOINT_RETRY_DELAY_SECONDS:-2}"
METRICS_RECOVERY_MAX_WAIT_SECONDS="${REAL_FW_METRICS_RECOVERY_MAX_WAIT_SECONDS:-90}"
STARTUP_SETTLE_MIN_SECONDS="${REAL_FW_STARTUP_SETTLE_MIN_SECONDS:-15}"
STARTUP_SETTLE_MAX_SECONDS="${REAL_FW_STARTUP_SETTLE_MAX_SECONDS:-20}"
STARTUP_STABLE_CONSECUTIVE_SAMPLES="${REAL_FW_STARTUP_STABLE_CONSECUTIVE_SAMPLES:-2}"
UPLOAD_FS=0
SKIP_FLASH=0
METRICS_URL="${REAL_FW_METRICS_URL:-}"
PANIC_URL="${REAL_FW_PANIC_URL:-}"
METRICS_RESET_URL="${REAL_FW_METRICS_RESET_URL:-}"
METRICS_SOAK_MODE="${REAL_FW_METRICS_SOAK_MODE:-1}"
METRICS_POLL_URL=""
PANIC_POLL_URL=""
METRICS_REQUIRED=0
MIN_METRICS_OK_SAMPLES=1
MIN_RX_PACKETS_DELTA=1
MIN_PARSE_SUCCESSES_DELTA=1
MAX_PARSE_FAILURES_DELTA=0
MAX_QUEUE_DROPS_DELTA=0
MAX_PERF_DROPS_DELTA=0
MAX_EVENT_DROPS_DELTA=0
MAX_FLUSH_MAX_US=0
MAX_LOOP_MAX_US=0
MAX_WIFI_MAX_US=0
MAX_BLE_DRAIN_MAX_US=0
MAX_SD_MAX_US=0
MAX_FS_MAX_US=0
MAX_OVERSIZE_DROPS_DELTA=0
MAX_QUEUE_HIGH_WATER=0
MAX_WIFI_CONNECT_DEFERRED=0
MIN_DMA_FREE=0
MIN_DMA_LARGEST=0
MAX_BLE_PROCESS_MAX_US=0
MAX_DISP_PIPE_MAX_US=0
MAX_BLE_MUTEX_TIMEOUT_DELTA=0
SOAK_PROFILE=""
LATENCY_GATE_MODE="${REAL_FW_LATENCY_GATE_MODE:-strict}"
LATENCY_ROBUST_MIN_SAMPLES="${REAL_FW_LATENCY_ROBUST_MIN_SAMPLES:-8}"
LATENCY_ROBUST_MAX_EXCEED_PCT="${REAL_FW_LATENCY_ROBUST_MAX_EXCEED_PCT:-5}"
WIFI_ROBUST_SKIP_FIRST_SAMPLES="${REAL_FW_WIFI_ROBUST_SKIP_FIRST_SAMPLES:-2}"
MINIMA_TAIL_EXCLUDE_SAMPLES="${REAL_FW_MINIMA_TAIL_EXCLUDE_SAMPLES:-0}"
CLI_OVERRIDE_MAX_FLUSH_MAX_US=0
CLI_OVERRIDE_MAX_LOOP_MAX_US=0
CLI_OVERRIDE_MAX_WIFI_MAX_US=0
CLI_OVERRIDE_MAX_BLE_DRAIN_MAX_US=0
CLI_OVERRIDE_MAX_SD_MAX_US=0
CLI_OVERRIDE_MAX_FS_MAX_US=0
CLI_OVERRIDE_MAX_QUEUE_HIGH_WATER=0
CLI_OVERRIDE_MAX_WIFI_CONNECT_DEFERRED=0
CLI_OVERRIDE_MIN_DMA_FREE=0
CLI_OVERRIDE_MIN_DMA_LARGEST=0
CLI_OVERRIDE_MAX_BLE_PROCESS_MAX_US=0
CLI_OVERRIDE_MAX_DISP_PIPE_MAX_US=0
CLI_OVERRIDE_MAX_BLE_MUTEX_TIMEOUT_DELTA=0
BASELINE_PERF_CSV=""
BASELINE_PERF_SESSION="last-connected"
BASELINE_LATENCY_FACTOR="1.0"
BASELINE_THROUGHPUT_FACTOR="0.50"
BASELINE_PROFILE=""
BASELINE_STRESS_CLASS=""
RUN_STRESS_CLASS=""
BASELINE_GATES_APPLIED=0
BASELINE_GATES_KV_FILE=""
BASELINE_SELECTED_SESSION=""
BASELINE_SELECTED_ROWS=""
BASELINE_SELECTED_DURATION_MS=""
BASELINE_RX_RATE_PER_SEC=""
BASELINE_PARSE_RATE_PER_SEC=""
BASELINE_PEAK_LOOP_US=""
BASELINE_PEAK_FLUSH_US=""
BASELINE_PEAK_WIFI_US=""
BASELINE_PEAK_BLE_DRAIN_US=""
BASELINE_DERIVED_MIN_RX_DELTA=""
BASELINE_DERIVED_MIN_PARSE_DELTA=""
BASELINE_DERIVED_MAX_LOOP_US=""
BASELINE_DERIVED_MAX_FLUSH_US=""
BASELINE_DERIVED_MAX_WIFI_US=""
BASELINE_DERIVED_MAX_BLE_DRAIN_US=""
ALLOW_INCONCLUSIVE=0
DRY_RUN=0
DISPLAY_DRIVE_ENABLED=0
DISPLAY_DRIVE_INTERVAL_SECONDS=7
DISPLAY_PREVIEW_URL="${REAL_FW_DISPLAY_PREVIEW_URL:-}"
DISPLAY_CLEAR_URL="${REAL_FW_DISPLAY_CLEAR_URL:-}"
DISPLAY_PREVIEW_HOLD_SECONDS="${REAL_FW_DISPLAY_PREVIEW_HOLD_SECONDS:-7}"
DISPLAY_MIN_UPDATES_DELTA=1
TRANSITION_DRIVE_ENABLED=0
TRANSITION_DRIVE_INTERVAL_SECONDS=15
TRANSITION_FLAP_CYCLES=3
TRANSITION_CONTROL_URL="${REAL_FW_TRANSITION_CONTROL_URL:-${REAL_FW_SETTINGS_URL:-}}"
TRANSITION_STABLE_CONSECUTIVE_SAMPLES="${REAL_FW_STABLE_CONSECUTIVE_SAMPLES:-2}"
CONNECT_BURST_STABLE_CONSECUTIVE_SAMPLES="${REAL_FW_CONNECT_BURST_STABLE_CONSECUTIVE_SAMPLES:-3}"
MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN=0
MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF=0
MAX_SAMPLES_TO_STABLE=0
MAX_AP_TRANSITION_CHURN_DELTA=0
MAX_PROXY_ADV_TRANSITION_CHURN_DELTA=0
MIN_AP_DOWN_TRANSITIONS=0
MIN_PROXY_ADV_OFF_TRANSITIONS=0
OUT_DIR=""
COMPARE_TO_MANIFESTS=()
METRICS_RECOVERY_ATTEMPTS=0
METRICS_RECOVERY_ELAPSED_SECONDS=0
METRICS_RECOVERY_BUDGET_SECONDS=0
METRICS_RECOVERY_URL=""
METRICS_RECOVERY_REASON="not_attempted"

is_uint() {
  local value="${1:-}"
  [[ "$value" =~ ^[0-9]+$ ]]
}

is_int() {
  local value="${1:-}"
  [[ "$value" =~ ^-?[0-9]+$ ]]
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --duration-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --duration-seconds" >&2
        exit 2
      fi
      DURATION_SECONDS="$2"
      shift
      ;;
    --poll-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --poll-seconds" >&2
        exit 2
      fi
      POLL_SECONDS="$2"
      shift
      ;;
    --serial-baud)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --serial-baud" >&2
        exit 2
      fi
      SERIAL_BAUD="$2"
      shift
      ;;
    --http-timeout-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --http-timeout-seconds" >&2
        exit 2
      fi
      HTTP_TIMEOUT_SECONDS="$2"
      shift
      ;;
    --metrics-recovery-max-wait-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --metrics-recovery-max-wait-seconds" >&2
        exit 2
      fi
      METRICS_RECOVERY_MAX_WAIT_SECONDS="$2"
      shift
      ;;
    --startup-settle-min-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --startup-settle-min-seconds" >&2
        exit 2
      fi
      STARTUP_SETTLE_MIN_SECONDS="$2"
      shift
      ;;
    --startup-settle-max-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --startup-settle-max-seconds" >&2
        exit 2
      fi
      STARTUP_SETTLE_MAX_SECONDS="$2"
      shift
      ;;
    --env)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --env" >&2
        exit 2
      fi
      ENV_NAME="$2"
      shift
      ;;
    --port)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --port" >&2
        exit 2
      fi
      TEST_PORT="$2"
      PORT_LOCKED=1
      shift
      ;;
    --with-fs)
      UPLOAD_FS=1
      ;;
    --skip-flash)
      SKIP_FLASH=1
      ;;
    --metrics-url)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --metrics-url" >&2
        exit 2
      fi
      METRICS_URL="$2"
      shift
      ;;
    --panic-url)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --panic-url" >&2
        exit 2
      fi
      PANIC_URL="$2"
      shift
      ;;
    --no-metrics)
      METRICS_URL=""
      PANIC_URL=""
      ;;
    --require-metrics)
      METRICS_REQUIRED=1
      ;;
    --min-metrics-ok-samples)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-metrics-ok-samples" >&2
        exit 2
      fi
      MIN_METRICS_OK_SAMPLES="$2"
      shift
      ;;
    --min-rx-packets-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-rx-packets-delta" >&2
        exit 2
      fi
      MIN_RX_PACKETS_DELTA="$2"
      shift
      ;;
    --min-parse-successes-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-parse-successes-delta" >&2
        exit 2
      fi
      MIN_PARSE_SUCCESSES_DELTA="$2"
      shift
      ;;
    --max-parse-failures-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-parse-failures-delta" >&2
        exit 2
      fi
      MAX_PARSE_FAILURES_DELTA="$2"
      shift
      ;;
    --max-queue-drops-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-queue-drops-delta" >&2
        exit 2
      fi
      MAX_QUEUE_DROPS_DELTA="$2"
      shift
      ;;
    --max-perf-drops-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-perf-drops-delta" >&2
        exit 2
      fi
      MAX_PERF_DROPS_DELTA="$2"
      shift
      ;;
    --max-event-drops-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-event-drops-delta" >&2
        exit 2
      fi
      MAX_EVENT_DROPS_DELTA="$2"
      shift
      ;;
    --max-flush-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-flush-max-us" >&2
        exit 2
      fi
      MAX_FLUSH_MAX_US="$2"
      CLI_OVERRIDE_MAX_FLUSH_MAX_US=1
      shift
      ;;
    --max-loop-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-loop-max-us" >&2
        exit 2
      fi
      MAX_LOOP_MAX_US="$2"
      CLI_OVERRIDE_MAX_LOOP_MAX_US=1
      shift
      ;;
    --max-wifi-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-wifi-max-us" >&2
        exit 2
      fi
      MAX_WIFI_MAX_US="$2"
      CLI_OVERRIDE_MAX_WIFI_MAX_US=1
      shift
      ;;
    --max-ble-drain-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-ble-drain-max-us" >&2
        exit 2
      fi
      MAX_BLE_DRAIN_MAX_US="$2"
      CLI_OVERRIDE_MAX_BLE_DRAIN_MAX_US=1
      shift
      ;;
    --max-sd-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-sd-max-us" >&2
        exit 2
      fi
      MAX_SD_MAX_US="$2"
      CLI_OVERRIDE_MAX_SD_MAX_US=1
      shift
      ;;
    --max-fs-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-fs-max-us" >&2
        exit 2
      fi
      MAX_FS_MAX_US="$2"
      CLI_OVERRIDE_MAX_FS_MAX_US=1
      shift
      ;;
    --max-oversize-drops-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-oversize-drops-delta" >&2
        exit 2
      fi
      MAX_OVERSIZE_DROPS_DELTA="$2"
      shift
      ;;
    --max-queue-high-water)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-queue-high-water" >&2
        exit 2
      fi
      MAX_QUEUE_HIGH_WATER="$2"
      CLI_OVERRIDE_MAX_QUEUE_HIGH_WATER=1
      shift
      ;;
    --max-wifi-connect-deferred)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-wifi-connect-deferred" >&2
        exit 2
      fi
      MAX_WIFI_CONNECT_DEFERRED="$2"
      CLI_OVERRIDE_MAX_WIFI_CONNECT_DEFERRED=1
      shift
      ;;
    --min-dma-free)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-dma-free" >&2
        exit 2
      fi
      MIN_DMA_FREE="$2"
      CLI_OVERRIDE_MIN_DMA_FREE=1
      shift
      ;;
    --min-dma-largest)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-dma-largest" >&2
        exit 2
      fi
      MIN_DMA_LARGEST="$2"
      CLI_OVERRIDE_MIN_DMA_LARGEST=1
      shift
      ;;
    --max-ble-process-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-ble-process-max-us" >&2
        exit 2
      fi
      MAX_BLE_PROCESS_MAX_US="$2"
      CLI_OVERRIDE_MAX_BLE_PROCESS_MAX_US=1
      shift
      ;;
    --max-disp-pipe-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-disp-pipe-max-us" >&2
        exit 2
      fi
      MAX_DISP_PIPE_MAX_US="$2"
      CLI_OVERRIDE_MAX_DISP_PIPE_MAX_US=1
      shift
      ;;
    --max-ble-mutex-timeout-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-ble-mutex-timeout-delta" >&2
        exit 2
      fi
      MAX_BLE_MUTEX_TIMEOUT_DELTA="$2"
      CLI_OVERRIDE_MAX_BLE_MUTEX_TIMEOUT_DELTA=1
      shift
      ;;
    --profile)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --profile" >&2
        exit 2
      fi
      SOAK_PROFILE="$2"
      shift
      ;;
    --latency-gate-mode)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --latency-gate-mode" >&2
        exit 2
      fi
      LATENCY_GATE_MODE="$2"
      shift
      ;;
    --latency-robust-min-samples)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --latency-robust-min-samples" >&2
        exit 2
      fi
      LATENCY_ROBUST_MIN_SAMPLES="$2"
      shift
      ;;
    --latency-robust-max-exceed-pct)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --latency-robust-max-exceed-pct" >&2
        exit 2
      fi
      LATENCY_ROBUST_MAX_EXCEED_PCT="$2"
      shift
      ;;
    --wifi-robust-skip-first-samples)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --wifi-robust-skip-first-samples" >&2
        exit 2
      fi
      WIFI_ROBUST_SKIP_FIRST_SAMPLES="$2"
      shift
      ;;
    --exclude-tail-samples-for-minima)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --exclude-tail-samples-for-minima" >&2
        exit 2
      fi
      MINIMA_TAIL_EXCLUDE_SAMPLES="$2"
      shift
      ;;
    --baseline-perf-csv)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --baseline-perf-csv" >&2
        exit 2
      fi
      BASELINE_PERF_CSV="$2"
      shift
      ;;
    --baseline-perf-session)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --baseline-perf-session" >&2
        exit 2
      fi
      BASELINE_PERF_SESSION="$2"
      shift
      ;;
    --baseline-profile)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --baseline-profile" >&2
        exit 2
      fi
      BASELINE_PROFILE="$2"
      shift
      ;;
    --baseline-stress-class)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --baseline-stress-class" >&2
        exit 2
      fi
      BASELINE_STRESS_CLASS="$2"
      shift
      ;;
    --baseline-latency-factor)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --baseline-latency-factor" >&2
        exit 2
      fi
      BASELINE_LATENCY_FACTOR="$2"
      shift
      ;;
    --baseline-throughput-factor)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --baseline-throughput-factor" >&2
        exit 2
      fi
      BASELINE_THROUGHPUT_FACTOR="$2"
      shift
      ;;
    --allow-inconclusive)
      ALLOW_INCONCLUSIVE=1
      ;;
    --compare-to)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --compare-to" >&2
        exit 2
      fi
      COMPARE_TO_MANIFESTS+=("$2")
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      ;;
    --drive-display-preview)
      DISPLAY_DRIVE_ENABLED=1
      ;;
    --display-drive-interval-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --display-drive-interval-seconds" >&2
        exit 2
      fi
      DISPLAY_DRIVE_INTERVAL_SECONDS="$2"
      shift
      ;;
    --display-preview-url)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --display-preview-url" >&2
        exit 2
      fi
      DISPLAY_PREVIEW_URL="$2"
      shift
      ;;
    --min-display-updates-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-display-updates-delta" >&2
        exit 2
      fi
      DISPLAY_MIN_UPDATES_DELTA="$2"
      shift
      ;;
    --drive-transition-flaps)
      TRANSITION_DRIVE_ENABLED=1
      ;;
    --transition-drive-interval-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --transition-drive-interval-seconds" >&2
        exit 2
      fi
      TRANSITION_DRIVE_INTERVAL_SECONDS="$2"
      shift
      ;;
    --transition-flap-cycles)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --transition-flap-cycles" >&2
        exit 2
      fi
      TRANSITION_FLAP_CYCLES="$2"
      shift
      ;;
    --transition-control-url|--transition-settings-url)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --transition-control-url" >&2
        exit 2
      fi
      TRANSITION_CONTROL_URL="$2"
      shift
      ;;
    --stable-consecutive-samples)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --stable-consecutive-samples" >&2
        exit 2
      fi
      TRANSITION_STABLE_CONSECUTIVE_SAMPLES="$2"
      shift
      ;;
    --max-time-to-stable-ms-after-ap-down)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-time-to-stable-ms-after-ap-down" >&2
        exit 2
      fi
      MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN="$2"
      shift
      ;;
    --max-time-to-stable-ms-after-proxy-adv-off)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-time-to-stable-ms-after-proxy-adv-off" >&2
        exit 2
      fi
      MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF="$2"
      shift
      ;;
    --max-samples-to-stable)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-samples-to-stable" >&2
        exit 2
      fi
      MAX_SAMPLES_TO_STABLE="$2"
      shift
      ;;
    --max-ap-transition-churn-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-ap-transition-churn-delta" >&2
        exit 2
      fi
      MAX_AP_TRANSITION_CHURN_DELTA="$2"
      shift
      ;;
    --max-proxy-adv-transition-churn-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-proxy-adv-transition-churn-delta" >&2
        exit 2
      fi
      MAX_PROXY_ADV_TRANSITION_CHURN_DELTA="$2"
      shift
      ;;
    --min-ap-down-transitions)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-ap-down-transitions" >&2
        exit 2
      fi
      MIN_AP_DOWN_TRANSITIONS="$2"
      shift
      ;;
    --min-proxy-adv-off-transitions)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --min-proxy-adv-off-transitions" >&2
        exit 2
      fi
      MIN_PROXY_ADV_OFF_TRANSITIONS="$2"
      shift
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
      cat <<'EOF'
Usage: ./scripts/run_real_fw_soak.sh [options]

Options:
  --duration-seconds N   Soak duration after flashing (default: 300)
  --poll-seconds N       Debug API poll interval (default: 5)
  --serial-baud N        Serial capture baud rate (default: 115200)
  --http-timeout-seconds N
                        curl timeout for metrics/drive calls (default: 2)
  --metrics-recovery-max-wait-seconds N
                        Maximum time to wait for metrics recovery after flash
                        before failing the run (default: 90)
  --startup-settle-min-seconds N
                        Minimum runtime settle floor after metrics recovery
                        before scoring starts (default: 15)
  --startup-settle-max-seconds N
                        Maximum runtime settle wait after metrics recovery
                        before scoring starts (default: 20)
  --env NAME             PlatformIO env to flash (default: waveshare-349)
  --port PATH            Fixed serial port (default: auto-detect; or set DEVICE_PORT)
  --with-fs              Stage and upload LittleFS via build.sh before firmware upload
  --skip-flash           Skip flashing and only run soak collection
  --metrics-url URL      Poll debug metrics endpoint (or set REAL_FW_METRICS_URL)
  --panic-url URL        Poll debug panic endpoint (default: derived from metrics URL)
  --no-metrics           Disable debug API polling (serial-only soak)
  --require-metrics      Fail run if no successful metrics samples are captured
  --min-metrics-ok-samples N
                        Minimum parsed metrics successes when --require-metrics (default: 1)
  --min-rx-packets-delta N
                        Minimum rxPackets delta across soak (default: 1)
  --min-parse-successes-delta N
                        Minimum parseSuccesses delta across soak (default: 1)
  --max-parse-failures-delta N
                        Maximum parseFailures delta across soak (default: 0)
  --max-queue-drops-delta N
                        Maximum queueDrops delta across soak (default: 0)
  --max-perf-drops-delta N
                        Maximum perfDrop delta across soak (default: 0)
  --max-event-drops-delta N
                        Maximum eventBus drop delta across soak (default: 0)
  --max-flush-max-us N  Maximum observed flushMaxUs peak (0 disables gate)
  --max-loop-max-us N   Maximum observed loopMaxUs peak (0 disables gate)
  --max-wifi-max-us N   Maximum observed wifiMaxUs peak (0 disables gate)
  --max-ble-drain-max-us N
                        Maximum observed bleDrainMaxUs peak (0 disables gate)
  --baseline-perf-csv PATH
                        Derive runtime gates from a known-good perf_boot CSV
  --baseline-perf-session MODE
                        Session selector: last-connected (default), last,
                        longest-connected, longest, or 1-based index
  --baseline-profile PROFILE
                        Declared baseline capture profile: drive_wifi_off or
                        drive_wifi_ap (required with --baseline-perf-csv)
  --baseline-stress-class CLASS
                        Declared baseline stress class: core or display
                        (required with --baseline-perf-csv)
  --baseline-latency-factor F
                        Multiply baseline peaks for max gates (default: 1.0)
  --baseline-throughput-factor F
                        Fraction of baseline rx/parse rates used for min deltas
                        (default: 0.50)
  --drive-display-preview
                        Repeatedly call display preview endpoint during soak
  --display-drive-interval-seconds N
                        Interval between display preview triggers (default: 7)
  --display-preview-url URL
                        Display preview endpoint URL (default: derived from metrics URL)
  --min-display-updates-delta N
                        Fail when parsed displayUpdates delta is below N (default: 1)
  --drive-transition-flaps
                        Toggle proxy BLE advertising off/on during soak
                        using /api/debug/proxy-advertising for transition
                        recovery checks
  --transition-drive-interval-seconds N
                        Interval between transition flap actions (default: 15)
  --transition-flap-cycles N
                        Number of off/on flap cycles to attempt (default: 3)
  --transition-control-url URL
                        Runtime proxy-control endpoint used by transition drive
                        (default: derived from metrics URL)
  --stable-consecutive-samples N
                        Consecutive in-threshold samples required to mark
                        transition stabilized (default: 2)
  --max-time-to-stable-ms-after-ap-down N
                        Maximum allowed recovery time after AP down transition
                        (0 disables gate)
  --max-time-to-stable-ms-after-proxy-adv-off N
                        Maximum allowed recovery time after proxy advertising
                        off transition (0 disables gate)
  --max-samples-to-stable N
                        Maximum allowed samples-to-stable after transition
                        (0 disables gate)
  --max-ap-transition-churn-delta N
                        Maximum allowed AP down transitions in steady-state
                        runs (default: 0)
  --max-proxy-adv-transition-churn-delta N
                        Maximum allowed proxy advertising on/off transitions in
                        steady-state runs (default: 0)
  --min-ap-down-transitions N
                        Minimum required AP down transitions in transition-drive
                        runs (default: 0)
  --min-proxy-adv-off-transitions N
                        Minimum required proxy advertising off transitions in
                        transition-drive runs (default: 0)
  --max-sd-max-us N     Maximum observed sdMaxUs peak (0 disables gate)
  --max-fs-max-us N     Maximum observed fsMaxUs peak (0 disables gate)
  --max-oversize-drops-delta N
                        Maximum oversizeDrops delta across soak (default: 0)
  --max-queue-high-water N
                        Maximum queueHighWater observed (0 disables gate)
  --max-wifi-connect-deferred N
                        Maximum wifiConnectDeferred delta for drive_wifi_ap
                        (0 disables this gate in drive_wifi_ap)
                        drive_wifi_off always enforces delta 0
  --min-dma-free N      Minimum heapDmaMin floor (0 disables gate)
  --min-dma-largest N   Minimum heapDmaLargestMin floor (0 disables gate)
  --max-ble-process-max-us N
                        Maximum bleProcessMaxUs peak (0 disables gate)
  --max-disp-pipe-max-us N
                        Maximum dispPipeMaxUs peak (0 disables gate)
  --max-ble-mutex-timeout-delta N
                        Maximum bleMutexTimeout delta (default: 0)
  --profile PROFILE     Apply PERF_SLOS.md gates: drive_wifi_ap (default when
                        metrics enabled) or drive_wifi_off
  --latency-gate-mode MODE
                        Latency classification mode for wifi/disp gates:
                        strict (peak-only, default), robust (N-of-M), hybrid
  --latency-robust-min-samples N
                        Minimum samples required to evaluate robust mode
                        (default: 8)
  --latency-robust-max-exceed-pct N
                        Allowed percent of samples above gate in robust mode
                        (default: 5)
  --wifi-robust-skip-first-samples N
                        Robust wifi warmup exclusion (default: 2)
  --exclude-tail-samples-for-minima N
                        Ignore the last N metrics samples for DMA floor minima
                        (default: 0)
  --dry-run              Print resolved config/gates and exit
  --allow-inconclusive   Exit 0 even when no telemetry signals were captured
  --compare-to PATH      Compare this run against a prior manifest.json (repeatable)
  --out-dir PATH         Write artifacts to PATH
  -h, --help             Show this help
EOF
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
  shift
done

derive_stress_class() {
  local display_enabled="$1"
  local transition_enabled="$2"
  if [[ "$transition_enabled" -eq 1 ]]; then
    echo "transition"
  elif [[ "$display_enabled" -eq 1 ]]; then
    echo "display_preview"
  else
    echo "core"
  fi
}

RUN_STRESS_CLASS="$(derive_stress_class "$DISPLAY_DRIVE_ENABLED" "$TRANSITION_DRIVE_ENABLED")"

# ------------------------------------------------------------------
# Profile resolution: apply PERF_SLOS.md hard limits as defaults.
# The soak test polls over WiFi so drive_wifi_ap is the realistic default.
# CLI-provided overrides take precedence (they were set above).
# ------------------------------------------------------------------
if [[ -z "$SOAK_PROFILE" && -n "$METRICS_URL" ]]; then
  SOAK_PROFILE="drive_wifi_ap"
elif [[ -z "$SOAK_PROFILE" && -z "$METRICS_URL" ]]; then
  SOAK_PROFILE="drive_wifi_off"
fi

if [[ -n "$SOAK_PROFILE" ]]; then
  case "$SOAK_PROFILE" in
    drive_wifi_off)
      [[ "$CLI_OVERRIDE_MAX_LOOP_MAX_US" -eq 0 && "$MAX_LOOP_MAX_US" -eq 0 ]] && MAX_LOOP_MAX_US=250000
      [[ "$CLI_OVERRIDE_MAX_BLE_DRAIN_MAX_US" -eq 0 && "$MAX_BLE_DRAIN_MAX_US" -eq 0 ]] && MAX_BLE_DRAIN_MAX_US=10000
      [[ "$CLI_OVERRIDE_MAX_FLUSH_MAX_US" -eq 0 && "$MAX_FLUSH_MAX_US" -eq 0 ]] && MAX_FLUSH_MAX_US=100000
      [[ "$CLI_OVERRIDE_MAX_SD_MAX_US" -eq 0 && "$MAX_SD_MAX_US" -eq 0 ]] && MAX_SD_MAX_US=50000
      [[ "$CLI_OVERRIDE_MAX_FS_MAX_US" -eq 0 && "$MAX_FS_MAX_US" -eq 0 ]] && MAX_FS_MAX_US=50000
      [[ "$CLI_OVERRIDE_MAX_WIFI_MAX_US" -eq 0 && "$MAX_WIFI_MAX_US" -eq 0 ]] && MAX_WIFI_MAX_US=1000
      [[ "$CLI_OVERRIDE_MAX_QUEUE_HIGH_WATER" -eq 0 && "$MAX_QUEUE_HIGH_WATER" -eq 0 ]] && MAX_QUEUE_HIGH_WATER=12
      [[ "$CLI_OVERRIDE_MIN_DMA_FREE" -eq 0 && "$MIN_DMA_FREE" -eq 0 ]] && MIN_DMA_FREE=20000
      [[ "$CLI_OVERRIDE_MIN_DMA_LARGEST" -eq 0 && "$MIN_DMA_LARGEST" -eq 0 ]] && MIN_DMA_LARGEST=10000
      # wifiConnectDeferred must be 0 for wifi-off profile
      [[ "$CLI_OVERRIDE_MAX_WIFI_CONNECT_DEFERRED" -eq 0 && "$MAX_WIFI_CONNECT_DEFERRED" -eq 0 ]] && MAX_WIFI_CONNECT_DEFERRED=0
      [[ "$CLI_OVERRIDE_MAX_BLE_PROCESS_MAX_US" -eq 0 && "$MAX_BLE_PROCESS_MAX_US" -eq 0 ]] && MAX_BLE_PROCESS_MAX_US=120000
      [[ "$CLI_OVERRIDE_MAX_DISP_PIPE_MAX_US" -eq 0 && "$MAX_DISP_PIPE_MAX_US" -eq 0 ]] && MAX_DISP_PIPE_MAX_US=80000
      [[ "$CLI_OVERRIDE_MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 && "$MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 ]] && MAX_BLE_MUTEX_TIMEOUT_DELTA=0
      ;;
    drive_wifi_ap)
      [[ "$CLI_OVERRIDE_MAX_LOOP_MAX_US" -eq 0 && "$MAX_LOOP_MAX_US" -eq 0 ]] && MAX_LOOP_MAX_US=250000
      [[ "$CLI_OVERRIDE_MAX_BLE_DRAIN_MAX_US" -eq 0 && "$MAX_BLE_DRAIN_MAX_US" -eq 0 ]] && MAX_BLE_DRAIN_MAX_US=10000
      [[ "$CLI_OVERRIDE_MAX_FLUSH_MAX_US" -eq 0 && "$MAX_FLUSH_MAX_US" -eq 0 ]] && MAX_FLUSH_MAX_US=100000
      [[ "$CLI_OVERRIDE_MAX_SD_MAX_US" -eq 0 && "$MAX_SD_MAX_US" -eq 0 ]] && MAX_SD_MAX_US=50000
      [[ "$CLI_OVERRIDE_MAX_FS_MAX_US" -eq 0 && "$MAX_FS_MAX_US" -eq 0 ]] && MAX_FS_MAX_US=50000
      [[ "$CLI_OVERRIDE_MAX_WIFI_MAX_US" -eq 0 && "$MAX_WIFI_MAX_US" -eq 0 ]] && MAX_WIFI_MAX_US=5000
      [[ "$CLI_OVERRIDE_MAX_QUEUE_HIGH_WATER" -eq 0 && "$MAX_QUEUE_HIGH_WATER" -eq 0 ]] && MAX_QUEUE_HIGH_WATER=12
      [[ "$CLI_OVERRIDE_MIN_DMA_FREE" -eq 0 && "$MIN_DMA_FREE" -eq 0 ]] && MIN_DMA_FREE=20000
      [[ "$CLI_OVERRIDE_MIN_DMA_LARGEST" -eq 0 && "$MIN_DMA_LARGEST" -eq 0 ]] && MIN_DMA_LARGEST=10000
      # wifiConnectDeferred <= 5 for wifi-ap profile
      [[ "$CLI_OVERRIDE_MAX_WIFI_CONNECT_DEFERRED" -eq 0 && "$MAX_WIFI_CONNECT_DEFERRED" -eq 0 ]] && MAX_WIFI_CONNECT_DEFERRED=5
      [[ "$CLI_OVERRIDE_MAX_BLE_PROCESS_MAX_US" -eq 0 && "$MAX_BLE_PROCESS_MAX_US" -eq 0 ]] && MAX_BLE_PROCESS_MAX_US=120000
      [[ "$CLI_OVERRIDE_MAX_DISP_PIPE_MAX_US" -eq 0 && "$MAX_DISP_PIPE_MAX_US" -eq 0 ]] && MAX_DISP_PIPE_MAX_US=80000
      [[ "$CLI_OVERRIDE_MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 && "$MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 ]] && MAX_BLE_MUTEX_TIMEOUT_DELTA=0
      ;;
    *)
      echo "Unknown --profile '$SOAK_PROFILE'. Use 'drive_wifi_off' or 'drive_wifi_ap'." >&2
      exit 2
      ;;
  esac
fi

if [[ "$TRANSITION_DRIVE_ENABLED" -eq 1 ]]; then
  # Transition-drive runs intentionally introduce proxy advertising churn.
  if [[ "$MIN_PROXY_ADV_OFF_TRANSITIONS" -eq 0 && "$TRANSITION_FLAP_CYCLES" -gt 0 ]]; then
    MIN_PROXY_ADV_OFF_TRANSITIONS="$TRANSITION_FLAP_CYCLES"
  fi
  if [[ "$MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF" -eq 0 ]]; then
    MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF=30000
  fi
  if [[ "$MAX_SAMPLES_TO_STABLE" -eq 0 ]]; then
    MAX_SAMPLES_TO_STABLE=6
  fi
fi

if ! [[ "$DURATION_SECONDS" =~ ^[0-9]+$ ]] || [[ "$DURATION_SECONDS" -lt 1 ]]; then
  echo "Invalid --duration-seconds value '$DURATION_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$POLL_SECONDS" =~ ^[0-9]+$ ]] || [[ "$POLL_SECONDS" -lt 1 ]]; then
  echo "Invalid --poll-seconds value '$POLL_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$SERIAL_BAUD" =~ ^[0-9]+$ ]] || [[ "$SERIAL_BAUD" -lt 1 ]]; then
  echo "Invalid --serial-baud value '$SERIAL_BAUD' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$HTTP_TIMEOUT_SECONDS" =~ ^[0-9]+$ ]] || [[ "$HTTP_TIMEOUT_SECONDS" -lt 1 ]]; then
  echo "Invalid --http-timeout-seconds value '$HTTP_TIMEOUT_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$METRICS_RECOVERY_MAX_WAIT_SECONDS" =~ ^[0-9]+$ ]] || [[ "$METRICS_RECOVERY_MAX_WAIT_SECONDS" -lt 1 ]]; then
  echo "Invalid --metrics-recovery-max-wait-seconds value '$METRICS_RECOVERY_MAX_WAIT_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$STARTUP_SETTLE_MIN_SECONDS" =~ ^[0-9]+$ ]]; then
  echo "Invalid REAL_FW_STARTUP_SETTLE_MIN_SECONDS value '$STARTUP_SETTLE_MIN_SECONDS' (expected non-negative integer)." >&2
  exit 2
fi

if ! [[ "$STARTUP_SETTLE_MAX_SECONDS" =~ ^[0-9]+$ ]]; then
  echo "Invalid REAL_FW_STARTUP_SETTLE_MAX_SECONDS value '$STARTUP_SETTLE_MAX_SECONDS' (expected non-negative integer)." >&2
  exit 2
fi

if [[ "$STARTUP_SETTLE_MIN_SECONDS" -gt "$STARTUP_SETTLE_MAX_SECONDS" ]]; then
  echo "Invalid startup settle window: min ${STARTUP_SETTLE_MIN_SECONDS}s is greater than max ${STARTUP_SETTLE_MAX_SECONDS}s." >&2
  exit 2
fi

if ! [[ "$STARTUP_STABLE_CONSECUTIVE_SAMPLES" =~ ^[0-9]+$ ]] || [[ "$STARTUP_STABLE_CONSECUTIVE_SAMPLES" -lt 1 ]]; then
  echo "Invalid REAL_FW_STARTUP_STABLE_CONSECUTIVE_SAMPLES value '$STARTUP_STABLE_CONSECUTIVE_SAMPLES' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$DISPLAY_DRIVE_INTERVAL_SECONDS" =~ ^[0-9]+$ ]] || [[ "$DISPLAY_DRIVE_INTERVAL_SECONDS" -lt 1 ]]; then
  echo "Invalid --display-drive-interval-seconds value '$DISPLAY_DRIVE_INTERVAL_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$DISPLAY_MIN_UPDATES_DELTA" =~ ^[0-9]+$ ]]; then
  echo "Invalid --min-display-updates-delta value '$DISPLAY_MIN_UPDATES_DELTA' (expected non-negative integer)." >&2
  exit 2
fi

if ! [[ "$TRANSITION_DRIVE_INTERVAL_SECONDS" =~ ^[0-9]+$ ]] || [[ "$TRANSITION_DRIVE_INTERVAL_SECONDS" -lt 1 ]]; then
  echo "Invalid --transition-drive-interval-seconds value '$TRANSITION_DRIVE_INTERVAL_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$TRANSITION_FLAP_CYCLES" =~ ^[0-9]+$ ]]; then
  echo "Invalid --transition-flap-cycles value '$TRANSITION_FLAP_CYCLES' (expected non-negative integer)." >&2
  exit 2
fi

if ! [[ "$TRANSITION_STABLE_CONSECUTIVE_SAMPLES" =~ ^[0-9]+$ ]] || [[ "$TRANSITION_STABLE_CONSECUTIVE_SAMPLES" -lt 1 ]]; then
  echo "Invalid --stable-consecutive-samples value '$TRANSITION_STABLE_CONSECUTIVE_SAMPLES' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$MIN_METRICS_OK_SAMPLES" =~ ^[0-9]+$ ]]; then
  echo "Invalid --min-metrics-ok-samples value '$MIN_METRICS_OK_SAMPLES' (expected non-negative integer)." >&2
  exit 2
fi

case "$LATENCY_GATE_MODE" in
  strict|robust|hybrid) ;;
  *)
    echo "Invalid --latency-gate-mode value '$LATENCY_GATE_MODE' (expected strict, robust, or hybrid)." >&2
    exit 2
    ;;
esac

if ! [[ "$LATENCY_ROBUST_MIN_SAMPLES" =~ ^[0-9]+$ ]]; then
  echo "Invalid --latency-robust-min-samples value '$LATENCY_ROBUST_MIN_SAMPLES' (expected non-negative integer)." >&2
  exit 2
fi

if ! [[ "$LATENCY_ROBUST_MAX_EXCEED_PCT" =~ ^[0-9]+$ ]] || [[ "$LATENCY_ROBUST_MAX_EXCEED_PCT" -gt 100 ]]; then
  echo "Invalid --latency-robust-max-exceed-pct value '$LATENCY_ROBUST_MAX_EXCEED_PCT' (expected integer in 0..100)." >&2
  exit 2
fi

if ! [[ "$WIFI_ROBUST_SKIP_FIRST_SAMPLES" =~ ^[0-9]+$ ]]; then
  echo "Invalid --wifi-robust-skip-first-samples value '$WIFI_ROBUST_SKIP_FIRST_SAMPLES' (expected non-negative integer)." >&2
  exit 2
fi

if ! [[ "$MINIMA_TAIL_EXCLUDE_SAMPLES" =~ ^[0-9]+$ ]]; then
  echo "Invalid --exclude-tail-samples-for-minima value '$MINIMA_TAIL_EXCLUDE_SAMPLES' (expected non-negative integer)." >&2
  exit 2
fi

if ! [[ "$METRICS_SOAK_MODE" =~ ^[01]$ ]]; then
  echo "Invalid REAL_FW_METRICS_SOAK_MODE value '$METRICS_SOAK_MODE' (expected 0 or 1)." >&2
  exit 2
fi

for gate_var in \
  MIN_RX_PACKETS_DELTA \
  MIN_PARSE_SUCCESSES_DELTA \
  MAX_PARSE_FAILURES_DELTA \
  MAX_QUEUE_DROPS_DELTA \
  MAX_PERF_DROPS_DELTA \
  MAX_EVENT_DROPS_DELTA \
  MAX_FLUSH_MAX_US \
  MAX_LOOP_MAX_US \
  MAX_WIFI_MAX_US \
  MAX_BLE_DRAIN_MAX_US \
  MAX_SD_MAX_US \
  MAX_FS_MAX_US \
  MAX_OVERSIZE_DROPS_DELTA \
  MAX_QUEUE_HIGH_WATER \
  MAX_WIFI_CONNECT_DEFERRED \
  MIN_DMA_FREE \
  MIN_DMA_LARGEST \
  MAX_BLE_PROCESS_MAX_US \
  MAX_DISP_PIPE_MAX_US \
  MAX_BLE_MUTEX_TIMEOUT_DELTA \
  MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN \
  MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF \
  MAX_SAMPLES_TO_STABLE \
  MAX_AP_TRANSITION_CHURN_DELTA \
  MAX_PROXY_ADV_TRANSITION_CHURN_DELTA \
  MIN_AP_DOWN_TRANSITIONS \
  MIN_PROXY_ADV_OFF_TRANSITIONS
do
  gate_val="${!gate_var}"
  if ! [[ "$gate_val" =~ ^[0-9]+$ ]]; then
    echo "Invalid $gate_var value '$gate_val' (expected non-negative integer)." >&2
    exit 2
  fi
done

if [[ -n "$BASELINE_PERF_CSV" && ! -f "$BASELINE_PERF_CSV" ]]; then
  echo "Baseline perf CSV not found: '$BASELINE_PERF_CSV'" >&2
  exit 2
fi

if [[ -z "$BASELINE_PERF_CSV" && ( -n "$BASELINE_PROFILE" || -n "$BASELINE_STRESS_CLASS" ) ]]; then
  echo "--baseline-profile/--baseline-stress-class require --baseline-perf-csv." >&2
  exit 2
fi

if [[ -n "$BASELINE_PERF_CSV" ]]; then
  if [[ -z "$BASELINE_PROFILE" ]]; then
    echo "Missing --baseline-profile when --baseline-perf-csv is set." >&2
    exit 2
  fi
  case "$BASELINE_PROFILE" in
    drive_wifi_off|drive_wifi_ap) ;;
    *)
      echo "Invalid --baseline-profile '$BASELINE_PROFILE'. Use 'drive_wifi_off' or 'drive_wifi_ap'." >&2
      exit 2
      ;;
  esac
  if [[ "$BASELINE_PROFILE" != "$SOAK_PROFILE" ]]; then
    echo "Baseline profile mismatch: baseline='$BASELINE_PROFILE' run='$SOAK_PROFILE'." >&2
    echo "Use a profile-matched baseline CSV (or adjust --profile)." >&2
    exit 2
  fi

  if [[ -z "$BASELINE_STRESS_CLASS" ]]; then
    echo "Missing --baseline-stress-class when --baseline-perf-csv is set." >&2
    exit 2
  fi
  case "$BASELINE_STRESS_CLASS" in
    core|display) ;;
    *)
      echo "Invalid --baseline-stress-class '$BASELINE_STRESS_CLASS'. Use core|display." >&2
      exit 2
      ;;
  esac
  if [[ "$BASELINE_STRESS_CLASS" != "$RUN_STRESS_CLASS" ]]; then
    echo "Baseline stress-class mismatch: baseline='$BASELINE_STRESS_CLASS' run='$RUN_STRESS_CLASS'." >&2
    echo "Use a stress-matched baseline CSV (same display drive class)." >&2
    exit 2
  fi

  if ! [[ "$BASELINE_LATENCY_FACTOR" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "Invalid --baseline-latency-factor value '$BASELINE_LATENCY_FACTOR' (expected positive number)." >&2
    exit 2
  fi
  if ! awk -v v="$BASELINE_LATENCY_FACTOR" 'BEGIN { exit (v > 0) ? 0 : 1 }'; then
    echo "Invalid --baseline-latency-factor value '$BASELINE_LATENCY_FACTOR' (expected > 0)." >&2
    exit 2
  fi

  if ! [[ "$BASELINE_THROUGHPUT_FACTOR" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "Invalid --baseline-throughput-factor value '$BASELINE_THROUGHPUT_FACTOR' (expected positive number)." >&2
    exit 2
  fi
  if ! awk -v v="$BASELINE_THROUGHPUT_FACTOR" 'BEGIN { exit (v > 0) ? 0 : 1 }'; then
    echo "Invalid --baseline-throughput-factor value '$BASELINE_THROUGHPUT_FACTOR' (expected > 0)." >&2
    exit 2
  fi
fi

if [[ "$DRY_RUN" -ne 1 ]]; then
  if ! command -v pio >/dev/null 2>&1; then
    echo "PlatformIO (pio) is required but not found in PATH." >&2
    exit 1
  fi
fi

METRICS_POLL_URL="$METRICS_URL"
PANIC_POLL_URL="$PANIC_URL"
if [[ -n "$METRICS_URL" ]]; then
  metrics_url_base="${METRICS_URL%%\?*}"

  if [[ -z "$PANIC_URL" ]]; then
    PANIC_URL="${metrics_url_base%/api/debug/metrics}/api/debug/panic"
  fi
  PANIC_POLL_URL="$PANIC_URL"
  if [[ -z "$METRICS_RESET_URL" ]]; then
    METRICS_RESET_URL="${metrics_url_base%/api/debug/metrics}/api/debug/metrics/reset"
  fi

  if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && -z "$DISPLAY_PREVIEW_URL" ]]; then
    DISPLAY_PREVIEW_URL="${metrics_url_base%/api/debug/metrics}/api/display/preview"
  fi
  if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && -z "$DISPLAY_CLEAR_URL" ]]; then
    DISPLAY_CLEAR_URL="${metrics_url_base%/api/debug/metrics}/api/display/preview/clear"
  fi
  if [[ "$TRANSITION_DRIVE_ENABLED" -eq 1 && -z "$TRANSITION_CONTROL_URL" ]]; then
    TRANSITION_CONTROL_URL="${metrics_url_base%/api/debug/metrics}/api/debug/proxy-advertising"
  fi

  if [[ "$METRICS_SOAK_MODE" -eq 1 && "$METRICS_URL" != *"soak="* ]]; then
    if [[ "$METRICS_URL" == *"?"* ]]; then
      METRICS_POLL_URL="${METRICS_URL}&soak=1"
    else
      METRICS_POLL_URL="${METRICS_URL}?soak=1"
    fi
  fi
  if [[ "$METRICS_SOAK_MODE" -eq 1 && -n "$PANIC_URL" && "$PANIC_URL" != *"soak="* ]]; then
    if [[ "$PANIC_URL" == *"?"* ]]; then
      PANIC_POLL_URL="${PANIC_URL}&soak=1"
    else
      PANIC_POLL_URL="${PANIC_URL}?soak=1"
    fi
  fi
fi

if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && -z "$DISPLAY_PREVIEW_URL" ]]; then
  echo "Display drive is enabled but no preview URL was provided or derivable." >&2
  echo "Set --display-preview-url or provide --metrics-url ending in /api/debug/metrics." >&2
  exit 2
fi

if [[ "$TRANSITION_DRIVE_ENABLED" -eq 1 && -z "$TRANSITION_CONTROL_URL" ]]; then
  echo "Transition drive is enabled but no control URL was provided or derivable." >&2
  echo "Set --transition-control-url or provide --metrics-url ending in /api/debug/metrics." >&2
  exit 2
fi

detect_usb_port() {
  local detected=""

  shopt -s nullglob
  local usb_ports=(
    /dev/cu.usbmodem*
    /dev/tty.usbmodem*
    /dev/ttyACM*
    /dev/ttyUSB*
    /dev/cu.usbserial*
    /dev/cu.SLAB_USBtoUART*
    /dev/tty.SLAB_USBtoUART*
  )
  shopt -u nullglob

  if [[ ${#usb_ports[@]} -gt 0 ]]; then
    detected="${usb_ports[0]}"
  else
    detected="$(pio device list | awk '/^\/dev\// {print $1}' \
      | grep -E 'usbmodem|ttyACM|ttyUSB|usbserial|SLAB_USBtoUART' \
      | head -n1 || true)"
  fi

  if [[ -n "$detected" ]]; then
    echo "$detected"
    return 0
  fi
  return 1
}

resolve_port() {
  if [[ "$PORT_LOCKED" -eq 1 ]]; then
    echo "$TEST_PORT"
    return 0
  fi
  detect_usb_port || return 1
}

wait_for_port() {
  local port="$1"
  local timeout_s="${2:-20}"
  local elapsed=0
  while (( elapsed < timeout_s )); do
    if [[ -e "$port" ]]; then
      return 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  return 1
}

find_python_for_csv() {
  if command -v python3 >/dev/null 2>&1; then
    echo "python3"
    return 0
  fi

  local pio_python="$HOME/.platformio/penv/bin/python"
  if [[ -x "$pio_python" ]]; then
    echo "$pio_python"
    return 0
  fi

  return 1
}

tighten_max_gate() {
  local current="$1"
  local derived="$2"

  if [[ -z "$derived" ]] || ! [[ "$derived" =~ ^[0-9]+$ ]] || [[ "$derived" -le 0 ]]; then
    echo "$current"
    return 0
  fi

  if [[ "$current" -le 0 ]]; then
    echo "$derived"
    return 0
  fi

  if [[ "$derived" -lt "$current" ]]; then
    echo "$derived"
  else
    echo "$current"
  fi
}

ensure_port_unlocked() {
  local port="$1"
  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi

  local pids
  pids="$(lsof -t "$port" 2>/dev/null | tr '\n' ' ' | xargs || true)"
  if [[ -n "$pids" ]]; then
    echo "Serial port '$port' is in use by another process." >&2
    echo "Close the serial monitor and retry." >&2
    # shellcheck disable=SC2086
    ps -o pid=,command= -p $pids >&2 || true
    return 1
  fi
  return 0
}

if [[ "$DRY_RUN" -ne 1 ]]; then
  if [[ -z "$TEST_PORT" ]]; then
    TEST_PORT="$(detect_usb_port || true)"
  fi
  if [[ -z "$TEST_PORT" ]]; then
    echo "No USB serial device detected. Connect the board and retry." >&2
    exit 1
  fi
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="$ROOT_DIR/.artifacts/test_reports/real_fw_soak_$timestamp"
fi
mkdir -p "$OUT_DIR"

RUN_LOG="$OUT_DIR/run.log"
SERIAL_LOG="$OUT_DIR/serial.log"
SERIAL_CAPTURE_ERR="$OUT_DIR/serial_capture.stderr"
METRICS_JSONL="$OUT_DIR/metrics.jsonl"
PANIC_JSONL="$OUT_DIR/panic.jsonl"
TREND_METRICS_NDJSON="$OUT_DIR/metrics.ndjson"
TREND_METRICS_KV="$OUT_DIR/hardware_metrics_kv.txt"
MANIFEST_JSON="$OUT_DIR/manifest.json"
TREND_SCORING_JSON="$OUT_DIR/scoring.json"
TREND_SCORING_STDERR="$OUT_DIR/scoring.stderr.log"
TREND_SUMMARY_MD="$OUT_DIR/trend_summary.md"
FINAL_RESULT_JSON="$OUT_DIR/final_result.json"
SUMMARY_BODY_MD="$OUT_DIR/summary_body.md"
SUMMARY_MD="$OUT_DIR/summary.md"
: > "$RUN_LOG"
: > "$SERIAL_LOG"
: > "$SERIAL_CAPTURE_ERR"
: > "$METRICS_JSONL"
: > "$PANIC_JSONL"
: > "$TREND_METRICS_NDJSON"
: > "$TREND_METRICS_KV"
: > "$TREND_SCORING_STDERR"

GIT_SHA_SHORT="$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
GIT_REF_NAME="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
RUN_ID="real_fw_soak_${timestamp}_${GIT_SHA_SHORT}"
BOARD_ID="${DEVICE_BOARD_ID:-unknown}"

if [[ -n "$BASELINE_PERF_CSV" ]]; then
  BASELINE_PYTHON="$(find_python_for_csv || true)"
  if [[ -z "$BASELINE_PYTHON" ]]; then
    echo "Cannot derive baseline gates: no Python interpreter found." >&2
    exit 1
  fi

  BASELINE_GATES_KV_FILE="$OUT_DIR/baseline_perf_gates_kv.txt"
  if ! "$BASELINE_PYTHON" "$ROOT_DIR/tools/soak_parse_baseline.py" \
      "$BASELINE_PERF_CSV" \
      "$BASELINE_PERF_SESSION" \
      "$DURATION_SECONDS" \
      "$BASELINE_LATENCY_FACTOR" \
      "$BASELINE_THROUGHPUT_FACTOR" > "$BASELINE_GATES_KV_FILE"
  then
    echo "Failed to derive baseline gates from '$BASELINE_PERF_CSV'." >&2
    exit 2
  fi

  while IFS='=' read -r key value; do
    case "$key" in
      session_index) BASELINE_SELECTED_SESSION="$value" ;;
      rows) BASELINE_SELECTED_ROWS="$value" ;;
      duration_ms) BASELINE_SELECTED_DURATION_MS="$value" ;;
      rx_rate_per_sec) BASELINE_RX_RATE_PER_SEC="$value" ;;
      parse_rate_per_sec) BASELINE_PARSE_RATE_PER_SEC="$value" ;;
      peak_loop_us) BASELINE_PEAK_LOOP_US="$value" ;;
      peak_flush_us) BASELINE_PEAK_FLUSH_US="$value" ;;
      peak_wifi_us) BASELINE_PEAK_WIFI_US="$value" ;;
      peak_ble_drain_us) BASELINE_PEAK_BLE_DRAIN_US="$value" ;;
      derived_min_rx_delta) BASELINE_DERIVED_MIN_RX_DELTA="$value" ;;
      derived_min_parse_delta) BASELINE_DERIVED_MIN_PARSE_DELTA="$value" ;;
      derived_max_loop_us) BASELINE_DERIVED_MAX_LOOP_US="$value" ;;
      derived_max_flush_us) BASELINE_DERIVED_MAX_FLUSH_US="$value" ;;
      derived_max_wifi_us) BASELINE_DERIVED_MAX_WIFI_US="$value" ;;
      derived_max_ble_drain_us) BASELINE_DERIVED_MAX_BLE_DRAIN_US="$value" ;;
    esac
  done < "$BASELINE_GATES_KV_FILE"

  for required in \
    BASELINE_SELECTED_SESSION \
    BASELINE_SELECTED_ROWS \
    BASELINE_SELECTED_DURATION_MS \
    BASELINE_DERIVED_MIN_RX_DELTA \
    BASELINE_DERIVED_MIN_PARSE_DELTA \
    BASELINE_DERIVED_MAX_LOOP_US \
    BASELINE_DERIVED_MAX_FLUSH_US \
    BASELINE_DERIVED_MAX_WIFI_US \
    BASELINE_DERIVED_MAX_BLE_DRAIN_US
  do
    required_val="${!required}"
    if [[ -z "$required_val" ]]; then
      echo "Baseline gate derivation missing required key '$required'." >&2
      exit 2
    fi
  done

  if [[ "$BASELINE_DERIVED_MIN_RX_DELTA" -gt "$MIN_RX_PACKETS_DELTA" ]]; then
    MIN_RX_PACKETS_DELTA="$BASELINE_DERIVED_MIN_RX_DELTA"
  fi
  if [[ "$BASELINE_DERIVED_MIN_PARSE_DELTA" -gt "$MIN_PARSE_SUCCESSES_DELTA" ]]; then
    MIN_PARSE_SUCCESSES_DELTA="$BASELINE_DERIVED_MIN_PARSE_DELTA"
  fi

  MAX_FLUSH_MAX_US="$(tighten_max_gate "$MAX_FLUSH_MAX_US" "$BASELINE_DERIVED_MAX_FLUSH_US")"
  MAX_LOOP_MAX_US="$(tighten_max_gate "$MAX_LOOP_MAX_US" "$BASELINE_DERIVED_MAX_LOOP_US")"
  MAX_WIFI_MAX_US="$(tighten_max_gate "$MAX_WIFI_MAX_US" "$BASELINE_DERIVED_MAX_WIFI_US")"
  MAX_BLE_DRAIN_MAX_US="$(tighten_max_gate "$MAX_BLE_DRAIN_MAX_US" "$BASELINE_DERIVED_MAX_BLE_DRAIN_US")"

  BASELINE_GATES_APPLIED=1
fi

if [[ "$DRY_RUN" -eq 1 ]]; then
  echo "==> Dry run (no flash, no soak)"
  echo "    env: $ENV_NAME"
  echo "    port: ${TEST_PORT:-auto-detect (skipped in dry-run)}"
  echo "    profile: ${SOAK_PROFILE:-none}"
  echo "    run stress class: ${RUN_STRESS_CLASS}"
  echo "    duration: ${DURATION_SECONDS}s"
  echo "    poll: ${POLL_SECONDS}s"
  echo "    serial baud: ${SERIAL_BAUD}"
  echo "    http timeout: ${HTTP_TIMEOUT_SECONDS}s"
  echo "    startup settle: min=${STARTUP_SETTLE_MIN_SECONDS}s max=${STARTUP_SETTLE_MAX_SECONDS}s stableSamples=${STARTUP_STABLE_CONSECUTIVE_SAMPLES}"
  echo "    metrics url: ${METRICS_URL:-disabled}"
  echo "    metrics poll url: ${METRICS_POLL_URL:-disabled}"
  echo "    metrics soak mode: ${METRICS_SOAK_MODE}"
  echo "    metrics reset url: ${METRICS_RESET_URL:-disabled}"
  echo "    panic url: ${PANIC_URL:-disabled}"
  echo "    panic poll url: ${PANIC_POLL_URL:-disabled}"
  echo "    metrics required: ${METRICS_REQUIRED}"
  echo "    min metrics successes: ${MIN_METRICS_OK_SAMPLES}"
  echo "    runtime gates: minRxDelta=${MIN_RX_PACKETS_DELTA} minParseSuccessDelta=${MIN_PARSE_SUCCESSES_DELTA} maxParseFailDelta=${MAX_PARSE_FAILURES_DELTA} maxQueueDropDelta=${MAX_QUEUE_DROPS_DELTA} maxPerfDropDelta=${MAX_PERF_DROPS_DELTA} maxEventDropDelta=${MAX_EVENT_DROPS_DELTA} maxOversizeDropDelta=${MAX_OVERSIZE_DROPS_DELTA}"
  echo "    latency gates: maxFlush=${MAX_FLUSH_MAX_US} maxLoop=${MAX_LOOP_MAX_US} maxWifi=${MAX_WIFI_MAX_US} maxBleDrain=${MAX_BLE_DRAIN_MAX_US} maxSd=${MAX_SD_MAX_US} maxFs=${MAX_FS_MAX_US} (0 disables)"
  echo "    latency gate mode: ${LATENCY_GATE_MODE} (robust minSamples=${LATENCY_ROBUST_MIN_SAMPLES} maxExceedPct=${LATENCY_ROBUST_MAX_EXCEED_PCT} wifiSkipFirst=${WIFI_ROBUST_SKIP_FIRST_SAMPLES})"
  echo "    firmware gates: maxBleProcessMax=${MAX_BLE_PROCESS_MAX_US} maxDispPipeMax=${MAX_DISP_PIPE_MAX_US} (0 disables)"
  echo "    counter gates: maxBleMutexTimeoutDelta=${MAX_BLE_MUTEX_TIMEOUT_DELTA}"
  echo "    resource gates: maxQueueHighWater=${MAX_QUEUE_HIGH_WATER} maxWifiConnDeferred=${MAX_WIFI_CONNECT_DEFERRED} minDmaFree=${MIN_DMA_FREE} minDmaLargest=${MIN_DMA_LARGEST} (0 disables except drive_wifi_off requires 0)"
  echo "    minima floor tail exclusion: ${MINIMA_TAIL_EXCLUDE_SAMPLES} sample(s)"
  echo "    transition gates: maxTimeToStableApDownMs=${MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN} maxTimeToStableProxyOffMs=${MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF} maxSamplesToStable=${MAX_SAMPLES_TO_STABLE} maxApTransitionChurn=${MAX_AP_TRANSITION_CHURN_DELTA} maxProxyAdvTransitionChurn=${MAX_PROXY_ADV_TRANSITION_CHURN_DELTA} minApDownTransitions=${MIN_AP_DOWN_TRANSITIONS} minProxyAdvOffTransitions=${MIN_PROXY_ADV_OFF_TRANSITIONS}"
  echo "    display drive: enabled=${DISPLAY_DRIVE_ENABLED} url=${DISPLAY_PREVIEW_URL:-disabled} interval=${DISPLAY_DRIVE_INTERVAL_SECONDS}s minDisplayUpdatesDelta=${DISPLAY_MIN_UPDATES_DELTA}"
  echo "    transition drive: enabled=${TRANSITION_DRIVE_ENABLED} controlUrl=${TRANSITION_CONTROL_URL:-disabled} interval=${TRANSITION_DRIVE_INTERVAL_SECONDS}s cycles=${TRANSITION_FLAP_CYCLES} stableConsecutiveSamples=${TRANSITION_STABLE_CONSECUTIVE_SAMPLES}"
  echo "    out dir: $OUT_DIR"
  if [[ "$BASELINE_GATES_APPLIED" -eq 1 ]]; then
    echo "    baseline csv: ${BASELINE_PERF_CSV} (profile=${BASELINE_PROFILE}, stressClass=${BASELINE_STRESS_CLASS}, runStressClass=${RUN_STRESS_CLASS}, session=${BASELINE_SELECTED_SESSION}, rows=${BASELINE_SELECTED_ROWS}, durationMs=${BASELINE_SELECTED_DURATION_MS})"
    echo "    baseline factors: latency x${BASELINE_LATENCY_FACTOR}, throughput x${BASELINE_THROUGHPUT_FACTOR}; rates rx=${BASELINE_RX_RATE_PER_SEC}/s parse=${BASELINE_PARSE_RATE_PER_SEC}/s"
  else
    echo "    baseline csv: disabled"
  fi
  exit 0
fi

run_and_log() {
  if "$@" 2>&1 | tee -a "$RUN_LOG"; then
    return 0
  fi
  return "${PIPESTATUS[0]}"
}

metrics_endpoint_max_wait_seconds() {
  echo "$METRICS_RECOVERY_MAX_WAIT_SECONDS"
}

validate_json_payload() {
  python3 -c 'import json, sys; json.load(sys.stdin)' >/dev/null 2>&1
}

extract_json_uint_field() {
  local payload="$1"
  local field="$2"
  printf "%s" "$payload" \
    | tr -d '\r\n' \
    | sed -n "s/.*\"${field}\":[[:space:]]*\\([0-9][0-9]*\\).*/\\1/p" \
    | head -n1
}

read_finalize_result_fields() {
  local path="$1"
  python3 - "$path" <<'PY'
import json
import sys
from pathlib import Path

payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
for field in ("result", "runtime_result", "trend_status", "trend_result", "exit_code", "trend_error"):
    value = payload.get(field, "")
    if value is None:
        value = ""
    print(value)
PY
}

read_endpoint_wait_fields() {
  local path="$1"
  python3 - "$path" <<'PY'
import json
import sys
from pathlib import Path

payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
for field in ("ok", "attempts", "elapsed_seconds", "reason"):
    value = payload.get(field, "")
    if value is None:
        value = ""
    print(value)
PY
}

metrics_reset_max_wait_seconds() {
  local budget=$((HTTP_TIMEOUT_SECONDS + METRICS_ENDPOINT_RETRY_DELAY_SECONDS * 2))
  local recovery_budget
  recovery_budget="$(metrics_endpoint_max_wait_seconds)"
  if [[ "$recovery_budget" =~ ^[0-9]+$ ]] && [[ "$recovery_budget" -gt 0 ]] && [[ "$recovery_budget" -lt "$budget" ]]; then
    budget="$recovery_budget"
  fi
  if [[ "$budget" -lt 1 ]]; then
    budget=1
  fi
  echo "$budget"
}

attempt_metrics_reset() {
  local endpoint_url="$1"
  local max_wait_seconds="$2"
  local attempts=0
  local deadline_epoch
  local now_epoch

  deadline_epoch=$(( $(date +%s) + max_wait_seconds ))
  while :; do
    attempts=$((attempts + 1))
    metrics_reset_http_code="$(curl -sS --max-time "$HTTP_TIMEOUT_SECONDS" -o /dev/null -w "%{http_code}" -X POST "$endpoint_url" 2>/dev/null || true)"
    if [[ "$metrics_reset_http_code" == "200" ]]; then
      metrics_reset_success=1
      metrics_reset_reason="ok"
      if [[ "$attempts" -eq 1 ]]; then
        echo "    metrics reset: OK (HTTP 200)" | tee -a "$RUN_LOG"
      else
        echo "    metrics reset: OK after retry (${attempts} attempt(s), HTTP 200)" | tee -a "$RUN_LOG"
      fi
      return 0
    fi

    if [[ -n "$metrics_reset_http_code" ]]; then
      metrics_reset_reason="http_${metrics_reset_http_code}"
    else
      metrics_reset_reason="no_response"
    fi

    now_epoch="$(date +%s)"
    if [[ "$now_epoch" -ge "$deadline_epoch" ]]; then
      break
    fi
    sleep "$METRICS_ENDPOINT_RETRY_DELAY_SECONDS"
  done

  if [[ -n "$metrics_reset_http_code" ]]; then
    echo "    metrics reset: WARN (HTTP ${metrics_reset_http_code} after ${attempts} attempt(s))" | tee -a "$RUN_LOG"
  else
    echo "    metrics reset: WARN (no response after ${attempts} attempt(s))" | tee -a "$RUN_LOG"
  fi
  return 1
}

wait_for_json_endpoint() {
  local endpoint_name="$1"
  local endpoint_url="$2"
  local required="${3:-1}"
  local wait_result_path="$OUT_DIR/endpoint_wait_result.json"
  local helper_exit=0
  local max_wait_seconds
  max_wait_seconds="$(metrics_endpoint_max_wait_seconds)"

  if [[ -z "$endpoint_url" ]]; then
    METRICS_RECOVERY_URL="disabled"
    METRICS_RECOVERY_BUDGET_SECONDS=0
    METRICS_RECOVERY_REASON="metrics polling disabled"
    return 0
  fi

  METRICS_RECOVERY_URL="$endpoint_url"
  METRICS_RECOVERY_BUDGET_SECONDS="$max_wait_seconds"
  echo "==> Waiting for ${endpoint_name} recovery (retry ${METRICS_ENDPOINT_RETRY_DELAY_SECONDS}s, bounded wait <= ${max_wait_seconds}s, url=${endpoint_url})" | tee -a "$RUN_LOG"
  set +e
  python3 "$ROOT_DIR/tools/wait_for_json_endpoint.py" \
    --endpoint-name "$endpoint_name" \
    --url "$endpoint_url" \
    --timeout-seconds "$HTTP_TIMEOUT_SECONDS" \
    --retry-delay-seconds "$METRICS_ENDPOINT_RETRY_DELAY_SECONDS" \
    --max-wait-seconds "$max_wait_seconds" > "$wait_result_path"
  helper_exit=$?
  set -e

  local endpoint_wait_fields=()
  while IFS= read -r line; do
    endpoint_wait_fields+=("$line")
  done < <(read_endpoint_wait_fields "$wait_result_path")
  local ok="${endpoint_wait_fields[0]:-False}"
  local attempts="${endpoint_wait_fields[1]:-0}"
  local elapsed_seconds="${endpoint_wait_fields[2]:-0}"
  local reason="${endpoint_wait_fields[3]:-}"

  METRICS_RECOVERY_ATTEMPTS="$attempts"
  METRICS_RECOVERY_ELAPSED_SECONDS="$elapsed_seconds"
  if [[ -n "$reason" ]]; then
    METRICS_RECOVERY_REASON="$reason"
  fi

  if [[ "$helper_exit" -eq 0 && "$ok" == "True" ]]; then
    METRICS_RECOVERY_REASON="recovered"
    if [[ "$attempts" -eq 1 ]]; then
      echo "    ${endpoint_name}: OK (${elapsed_seconds}s elapsed)" | tee -a "$RUN_LOG"
    else
      echo "    ${endpoint_name}: OK after retry (${attempts} attempt(s), ${elapsed_seconds}s elapsed)" | tee -a "$RUN_LOG"
    fi
    return 0
  fi

  if [[ -z "$reason" ]]; then
    reason="Timed out waiting for ${endpoint_name} within ${max_wait_seconds}s."
  fi
  if [[ "$required" -eq 1 ]]; then
    echo "$reason" | tee -a "$RUN_LOG" >&2
    return 1
  fi
  echo "[WARN] $reason" | tee -a "$RUN_LOG"
  return 0
}

startup_settle_enabled() {
  if [[ -z "$METRICS_POLL_URL" ]]; then
    return 1
  fi
  if [[ "$SKIP_FLASH" -ne 0 ]]; then
    return 1
  fi
  if [[ "$STARTUP_SETTLE_MAX_SECONDS" -le 0 ]]; then
    return 1
  fi
  if [[ "$MAX_LOOP_MAX_US" -le 0 && "$MAX_DISP_PIPE_MAX_US" -le 0 && "$MAX_WIFI_MAX_US" -le 0 ]]; then
    return 1
  fi
  return 0
}

STARTUP_SETTLE_APPLIED=0
STARTUP_SETTLE_SUCCEEDED=1
STARTUP_SETTLE_ATTEMPTS_USED=0
STARTUP_SETTLE_ELAPSED_SECONDS=0
STARTUP_SETTLE_LAST_UPTIME_MS=""
STARTUP_SETTLE_LAST_LOOP_MAX_US=""
STARTUP_SETTLE_LAST_DISP_PIPE_MAX_US=""
STARTUP_SETTLE_LAST_WIFI_MAX_US=""

wait_for_runtime_settle() {
  local endpoint_url="$1"
  local consecutive=0
  local attempt=0
  local settle_start_epoch
  local deadline_epoch
  local payload=""
  local loop_val=""
  local disp_val=""
  local wifi_val=""
  local uptime_val=""
  local sample_stable=0

  if ! startup_settle_enabled; then
    return 0
  fi

  STARTUP_SETTLE_APPLIED=1
  STARTUP_SETTLE_SUCCEEDED=0
  settle_start_epoch="$(date +%s)"
  deadline_epoch=$((settle_start_epoch + STARTUP_SETTLE_MAX_SECONDS))

  echo "==> Waiting for runtime settle (${STARTUP_STABLE_CONSECUTIVE_SAMPLES} consecutive stable sample(s), min ${STARTUP_SETTLE_MIN_SECONDS}s, max ${STARTUP_SETTLE_MAX_SECONDS}s)" | tee -a "$RUN_LOG"
  while [[ "$(date +%s)" -lt "$deadline_epoch" ]]; do
    attempt=$((attempt + 1))
    payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$endpoint_url" 2>/dev/null || true)"
    sample_stable=1

    if [[ -z "$payload" ]] || ! printf "%s" "$payload" | validate_json_payload; then
      sample_stable=0
      consecutive=0
      echo "    startup settle sample ${attempt}: no valid metrics payload" | tee -a "$RUN_LOG"
    else
      loop_val="$(extract_json_uint_field "$payload" "loopMaxUs")"
      disp_val="$(extract_json_uint_field "$payload" "dispPipeMaxUs")"
      wifi_val="$(extract_json_uint_field "$payload" "wifiMaxUs")"
      uptime_val="$(extract_json_uint_field "$payload" "uptimeMs")"

      if [[ "$MAX_LOOP_MAX_US" -gt 0 ]] && ( ! [[ "$loop_val" =~ ^[0-9]+$ ]] || [[ "$loop_val" -gt "$MAX_LOOP_MAX_US" ]] ); then
        sample_stable=0
      fi
      if [[ "$MAX_DISP_PIPE_MAX_US" -gt 0 ]] && ( ! [[ "$disp_val" =~ ^[0-9]+$ ]] || [[ "$disp_val" -gt "$MAX_DISP_PIPE_MAX_US" ]] ); then
        sample_stable=0
      fi
      if [[ "$MAX_WIFI_MAX_US" -gt 0 ]] && ( ! [[ "$wifi_val" =~ ^[0-9]+$ ]] || [[ "$wifi_val" -gt "$MAX_WIFI_MAX_US" ]] ); then
        sample_stable=0
      fi

      STARTUP_SETTLE_LAST_UPTIME_MS="$uptime_val"
      STARTUP_SETTLE_LAST_LOOP_MAX_US="$loop_val"
      STARTUP_SETTLE_LAST_DISP_PIPE_MAX_US="$disp_val"
      STARTUP_SETTLE_LAST_WIFI_MAX_US="$wifi_val"

      if [[ "$sample_stable" -eq 1 ]]; then
        consecutive=$((consecutive + 1))
      else
        consecutive=0
      fi

      echo "    startup settle sample ${attempt}: stable=${sample_stable} consecutive=${consecutive}/${STARTUP_STABLE_CONSECUTIVE_SAMPLES} uptimeMs=${uptime_val:-n/a} loopMaxUs=${loop_val:-n/a} dispPipeMaxUs=${disp_val:-n/a} wifiMaxUs=${wifi_val:-n/a}" | tee -a "$RUN_LOG"
      if [[ "$consecutive" -ge "$STARTUP_STABLE_CONSECUTIVE_SAMPLES" ]]; then
        STARTUP_SETTLE_ELAPSED_SECONDS=$(( $(date +%s) - settle_start_epoch ))
        if [[ "$STARTUP_SETTLE_ELAPSED_SECONDS" -lt "$STARTUP_SETTLE_MIN_SECONDS" ]]; then
          echo "    runtime settle: stable threshold reached, holding until minimum ${STARTUP_SETTLE_MIN_SECONDS}s floor (${STARTUP_SETTLE_ELAPSED_SECONDS}s elapsed)" | tee -a "$RUN_LOG"
        else
          STARTUP_SETTLE_SUCCEEDED=1
          STARTUP_SETTLE_ATTEMPTS_USED="$attempt"
          echo "    runtime settle: OK after ${STARTUP_SETTLE_ELAPSED_SECONDS}s" | tee -a "$RUN_LOG"
          return 0
        fi
      fi
    fi

    sleep "$POLL_SECONDS"
  done

  STARTUP_SETTLE_ATTEMPTS_USED="$attempt"
  STARTUP_SETTLE_ELAPSED_SECONDS=$(( $(date +%s) - settle_start_epoch ))
  echo "[WARN] Runtime did not settle within ${STARTUP_SETTLE_MAX_SECONDS}s after metrics recovery; continuing with scored soak window." | tee -a "$RUN_LOG"
  return 1
}

find_serial_python() {
  if command -v python3 >/dev/null 2>&1; then
    if python3 - <<'PY' >/dev/null 2>&1
import importlib.util
raise SystemExit(0 if importlib.util.find_spec("serial") else 1)
PY
    then
      echo "python3"
      return 0
    fi
  fi

  local pio_python="$HOME/.platformio/penv/bin/python"
  if [[ -x "$pio_python" ]]; then
    if "$pio_python" - <<'PY' >/dev/null 2>&1
import importlib.util
raise SystemExit(0 if importlib.util.find_spec("serial") else 1)
PY
    then
      echo "$pio_python"
      return 0
    fi
  fi

  return 1
}

echo "==> Real firmware soak starting" | tee -a "$RUN_LOG"
echo "    env: $ENV_NAME" | tee -a "$RUN_LOG"
echo "    port: $TEST_PORT" | tee -a "$RUN_LOG"
echo "    profile: ${SOAK_PROFILE:-none}" | tee -a "$RUN_LOG"
echo "    run stress class: ${RUN_STRESS_CLASS}" | tee -a "$RUN_LOG"
echo "    duration: ${DURATION_SECONDS}s" | tee -a "$RUN_LOG"
echo "    poll: ${POLL_SECONDS}s" | tee -a "$RUN_LOG"
echo "    serial baud: ${SERIAL_BAUD}" | tee -a "$RUN_LOG"
echo "    http timeout: ${HTTP_TIMEOUT_SECONDS}s" | tee -a "$RUN_LOG"
echo "    metrics recovery: maxWait=${METRICS_RECOVERY_MAX_WAIT_SECONDS}s retryDelay=${METRICS_ENDPOINT_RETRY_DELAY_SECONDS}s" | tee -a "$RUN_LOG"
echo "    startup settle: min=${STARTUP_SETTLE_MIN_SECONDS}s max=${STARTUP_SETTLE_MAX_SECONDS}s stableSamples=${STARTUP_STABLE_CONSECUTIVE_SAMPLES}" | tee -a "$RUN_LOG"
echo "    metrics url: ${METRICS_URL:-disabled}" | tee -a "$RUN_LOG"
echo "    metrics poll url: ${METRICS_POLL_URL:-disabled}" | tee -a "$RUN_LOG"
echo "    metrics soak mode: ${METRICS_SOAK_MODE}" | tee -a "$RUN_LOG"
echo "    metrics reset url: ${METRICS_RESET_URL:-disabled}" | tee -a "$RUN_LOG"
echo "    panic poll url: ${PANIC_POLL_URL:-disabled}" | tee -a "$RUN_LOG"
if [[ "$METRICS_REQUIRED" -eq 1 ]]; then
  echo "    metrics gate: require >= ${MIN_METRICS_OK_SAMPLES} parsed successes" | tee -a "$RUN_LOG"
fi
echo "    runtime gates: minRxDelta=${MIN_RX_PACKETS_DELTA} minParseSuccessDelta=${MIN_PARSE_SUCCESSES_DELTA} maxParseFailDelta=${MAX_PARSE_FAILURES_DELTA} maxQueueDropDelta=${MAX_QUEUE_DROPS_DELTA} maxPerfDropDelta=${MAX_PERF_DROPS_DELTA} maxEventDropDelta=${MAX_EVENT_DROPS_DELTA} maxOversizeDropDelta=${MAX_OVERSIZE_DROPS_DELTA}" | tee -a "$RUN_LOG"
echo "    latency gates: maxFlush=${MAX_FLUSH_MAX_US} maxLoop=${MAX_LOOP_MAX_US} maxWifi=${MAX_WIFI_MAX_US} maxBleDrain=${MAX_BLE_DRAIN_MAX_US} maxSd=${MAX_SD_MAX_US} maxFs=${MAX_FS_MAX_US} (0 disables)" | tee -a "$RUN_LOG"
echo "    latency gate mode: ${LATENCY_GATE_MODE} (robust minSamples=${LATENCY_ROBUST_MIN_SAMPLES} maxExceedPct=${LATENCY_ROBUST_MAX_EXCEED_PCT} wifiSkipFirst=${WIFI_ROBUST_SKIP_FIRST_SAMPLES})" | tee -a "$RUN_LOG"
echo "    firmware gates: maxBleProcessMax=${MAX_BLE_PROCESS_MAX_US} maxDispPipeMax=${MAX_DISP_PIPE_MAX_US} (0 disables)" | tee -a "$RUN_LOG"
echo "    counter gates: maxBleMutexTimeoutDelta=${MAX_BLE_MUTEX_TIMEOUT_DELTA}" | tee -a "$RUN_LOG"
echo "    resource gates: maxQueueHighWater=${MAX_QUEUE_HIGH_WATER} maxWifiConnDeferred=${MAX_WIFI_CONNECT_DEFERRED} minDmaFree=${MIN_DMA_FREE} minDmaLargest=${MIN_DMA_LARGEST} (0 disables except drive_wifi_off requires 0)" | tee -a "$RUN_LOG"
echo "    minima floor tail exclusion: ${MINIMA_TAIL_EXCLUDE_SAMPLES} sample(s)" | tee -a "$RUN_LOG"
echo "    transition gates: maxTimeToStableApDownMs=${MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN} maxTimeToStableProxyOffMs=${MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF} maxSamplesToStable=${MAX_SAMPLES_TO_STABLE} maxApTransitionChurn=${MAX_AP_TRANSITION_CHURN_DELTA} maxProxyAdvTransitionChurn=${MAX_PROXY_ADV_TRANSITION_CHURN_DELTA} minApDownTransitions=${MIN_AP_DOWN_TRANSITIONS} minProxyAdvOffTransitions=${MIN_PROXY_ADV_OFF_TRANSITIONS}" | tee -a "$RUN_LOG"
if [[ "$BASELINE_GATES_APPLIED" -eq 1 ]]; then
  echo "    baseline csv: ${BASELINE_PERF_CSV} (profile=${BASELINE_PROFILE}, stressClass=${BASELINE_STRESS_CLASS}, runStressClass=${RUN_STRESS_CLASS}, session=${BASELINE_SELECTED_SESSION}, rows=${BASELINE_SELECTED_ROWS}, durationMs=${BASELINE_SELECTED_DURATION_MS})" | tee -a "$RUN_LOG"
  echo "    baseline factors: latency x${BASELINE_LATENCY_FACTOR}, throughput x${BASELINE_THROUGHPUT_FACTOR}; rates rx=${BASELINE_RX_RATE_PER_SEC}/s parse=${BASELINE_PARSE_RATE_PER_SEC}/s" | tee -a "$RUN_LOG"
fi
if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]]; then
  echo "    display drive: enabled (${DISPLAY_PREVIEW_URL}) every ${DISPLAY_DRIVE_INTERVAL_SECONDS}s, min displayUpdates delta=${DISPLAY_MIN_UPDATES_DELTA}" | tee -a "$RUN_LOG"
else
  echo "    display drive: disabled" | tee -a "$RUN_LOG"
fi
if [[ "$TRANSITION_DRIVE_ENABLED" -eq 1 ]]; then
  echo "    transition drive: enabled (${TRANSITION_CONTROL_URL}) every ${TRANSITION_DRIVE_INTERVAL_SECONDS}s, cycles=${TRANSITION_FLAP_CYCLES}, stableConsecutiveSamples=${TRANSITION_STABLE_CONSECUTIVE_SAMPLES}" | tee -a "$RUN_LOG"
else
  echo "    transition drive: disabled" | tee -a "$RUN_LOG"
fi
echo "    out dir: $OUT_DIR" | tee -a "$RUN_LOG"
echo "" | tee -a "$RUN_LOG"

if ! wait_for_port "$TEST_PORT" 20; then
  echo "Timed out waiting for serial port '$TEST_PORT'." >&2
  exit 1
fi
if ! ensure_port_unlocked "$TEST_PORT"; then
  exit 1
fi

if [[ "$SKIP_FLASH" -eq 0 ]]; then
  if [[ "$UPLOAD_FS" -eq 1 ]]; then
    echo "==> Staging and uploading filesystem image via build.sh..." | tee -a "$RUN_LOG"
    run_and_log ./build.sh --env "$ENV_NAME" --upload-fs --upload-port "$TEST_PORT"
  fi

  echo "==> Uploading production firmware..." | tee -a "$RUN_LOG"
  run_and_log pio run -e "$ENV_NAME" -t upload --upload-port "$TEST_PORT"
else
  echo "==> Skipping flash (--skip-flash)" | tee -a "$RUN_LOG"
fi

MONITOR_PORT="$TEST_PORT"
if [[ "$PORT_LOCKED" -eq 0 ]]; then
  MONITOR_PORT="$(resolve_port || true)"
  if [[ -z "$MONITOR_PORT" ]]; then
    MONITOR_PORT="$TEST_PORT"
  fi
fi

if ! wait_for_port "$MONITOR_PORT" 20; then
  echo "Timed out waiting for monitor port '$MONITOR_PORT'." >&2
  exit 1
fi
if ! ensure_port_unlocked "$MONITOR_PORT"; then
  exit 1
fi

metrics_reset_attempted=0
metrics_reset_success=0
metrics_reset_http_code=""
metrics_reset_reason="not-attempted"
if [[ "$SKIP_FLASH" -eq 1 && -n "$METRICS_RESET_URL" ]]; then
  metrics_reset_max_wait_budget="$(metrics_reset_max_wait_seconds)"
  metrics_reset_attempted=1
  echo "==> Resetting debug metrics counters via ${METRICS_RESET_URL} (retry ${METRICS_ENDPOINT_RETRY_DELAY_SECONDS}s, bounded wait <= ${metrics_reset_max_wait_budget}s)" | tee -a "$RUN_LOG"
  attempt_metrics_reset "$METRICS_RESET_URL" "$metrics_reset_max_wait_budget" || true
fi

echo "==> Starting serial capture on $MONITOR_PORT" | tee -a "$RUN_LOG"
SERIAL_PYTHON="$(find_serial_python || true)"
if [[ -z "$SERIAL_PYTHON" ]]; then
  echo "No Python interpreter with pyserial is available." >&2
  echo "Install pyserial (or use PlatformIO penv with pyserial) and retry." >&2
  exit 1
fi

serial_capture_call_budget=0
if [[ -n "$METRICS_URL" ]]; then
  serial_capture_call_budget=$((serial_capture_call_budget + 1))
fi
if [[ -n "$PANIC_POLL_URL" ]]; then
  serial_capture_call_budget=$((serial_capture_call_budget + 1))
fi
if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]]; then
  # Account for the preview retry path in the same loop iteration.
  serial_capture_call_budget=$((serial_capture_call_budget + 2))
fi
if [[ "$TRANSITION_DRIVE_ENABLED" -eq 1 ]]; then
  serial_capture_call_budget=$((serial_capture_call_budget + 1))
fi
SERIAL_CAPTURE_GRACE_SECONDS=$((HTTP_TIMEOUT_SECONDS * serial_capture_call_budget + POLL_SECONDS + 15))
if [[ -n "$METRICS_POLL_URL" ]]; then
  SERIAL_CAPTURE_GRACE_SECONDS=$((SERIAL_CAPTURE_GRACE_SECONDS + METRICS_RECOVERY_MAX_WAIT_SECONDS))
fi
if startup_settle_enabled; then
  SERIAL_CAPTURE_GRACE_SECONDS=$((SERIAL_CAPTURE_GRACE_SECONDS + STARTUP_SETTLE_MAX_SECONDS))
fi
if [[ "$SERIAL_CAPTURE_GRACE_SECONDS" -lt 20 ]]; then
  SERIAL_CAPTURE_GRACE_SECONDS=20
fi
SERIAL_CAPTURE_DURATION_SECONDS=$((DURATION_SECONDS + SERIAL_CAPTURE_GRACE_SECONDS))
echo "    serial capture duration: ${SERIAL_CAPTURE_DURATION_SECONDS}s (grace ${SERIAL_CAPTURE_GRACE_SECONDS}s)" | tee -a "$RUN_LOG"

"$SERIAL_PYTHON" - "$MONITOR_PORT" "$SERIAL_BAUD" "$SERIAL_LOG" "$SERIAL_CAPTURE_DURATION_SECONDS" > "$SERIAL_CAPTURE_ERR" 2>&1 <<'PY' &
import serial
import sys
import time

port = sys.argv[1]
baud = int(sys.argv[2])
log_path = sys.argv[3]
duration = float(sys.argv[4])

# USB CDC can briefly disappear right after flash/reset; allow short open retries.
open_deadline = time.time() + 20.0
last_error = None
ser = None
while time.time() < open_deadline:
    try:
        ser = serial.Serial(port=port, baudrate=baud, timeout=0.25)
        break
    except (serial.SerialException, OSError) as exc:
        last_error = exc
        time.sleep(0.25)

if ser is None:
    if last_error is not None:
        raise last_error
    raise RuntimeError(f"could not open serial port {port}")

end_time = time.time() + duration
with open(log_path, "a", encoding="utf-8", errors="replace") as f:
    while time.time() < end_time:
        chunk = ser.readline()
        if not chunk:
            continue
        f.write(chunk.decode("utf-8", errors="replace"))
        f.flush()
ser.close()
PY
MONITOR_PID=$!

cleanup() {
  if [[ -n "${MONITOR_PID:-}" ]] && kill -0 "$MONITOR_PID" >/dev/null 2>&1; then
    kill "$MONITOR_PID" >/dev/null 2>&1 || true
    wait "$MONITOR_PID" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

sleep 1
monitor_died_early=0
if ! kill -0 "$MONITOR_PID" >/dev/null 2>&1; then
  monitor_died_early=1
fi

if [[ -n "$METRICS_POLL_URL" ]]; then
  if ! wait_for_json_endpoint "metrics endpoint" "$METRICS_POLL_URL" 1; then
    exit 1
  fi
fi
if [[ -n "$METRICS_POLL_URL" ]]; then
  wait_for_runtime_settle "$METRICS_POLL_URL" || true
fi

soak_start_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
soak_start_epoch="$(date +%s)"
soak_end_epoch=$((soak_start_epoch + DURATION_SECONDS))

metrics_samples=0
metrics_ok_samples=0
panic_samples=0
panic_ok_samples=0
display_drive_calls=0
display_drive_errors=0
display_drive_start_misses=0
display_drive_skipped_while_active=0
display_drive_restore_errors=0
display_drive_active_until_epoch=0
next_display_drive_epoch="$soak_start_epoch"
transition_drive_calls=0
transition_drive_errors=0
transition_flap_off_calls=0
transition_flap_on_calls=0
transition_flap_restore_calls=0
transition_restore_attempts=0
transition_restore_max_attempts=3
transition_restore_retry_delay_seconds=1
transition_flap_cycles_completed=0
transition_target_enabled=1
next_transition_drive_epoch="$soak_start_epoch"

echo "==> Soaking for ${DURATION_SECONDS}s..." | tee -a "$RUN_LOG"

while [[ "$(date +%s)" -lt "$soak_end_epoch" ]]; do
  now_epoch="$(date +%s)"
  now_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"

  if [[ "$monitor_died_early" -eq 0 ]] && ! kill -0 "$MONITOR_PID" >/dev/null 2>&1; then
    monitor_died_early=1
    echo "[WARN] Serial capture process exited unexpectedly during soak." | tee -a "$RUN_LOG"
  fi

  if [[ -n "$METRICS_POLL_URL" ]]; then
    metrics_samples=$((metrics_samples + 1))
    payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$METRICS_POLL_URL" 2>/dev/null || true)"
    if [[ -n "$payload" ]]; then
      metrics_ok_samples=$((metrics_ok_samples + 1))
      payload_oneline="$(printf "%s" "$payload" | tr -d '\r\n')"
      printf '{"ts":"%s","ok":true,"data":%s}\n' "$now_utc" "$payload_oneline" >> "$METRICS_JSONL"
    else
      printf '{"ts":"%s","ok":false}\n' "$now_utc" >> "$METRICS_JSONL"
    fi
  fi

  if [[ -n "$PANIC_POLL_URL" ]]; then
    panic_samples=$((panic_samples + 1))
    panic_payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$PANIC_POLL_URL" 2>/dev/null || true)"
    if [[ -n "$panic_payload" ]]; then
      panic_ok_samples=$((panic_ok_samples + 1))
      panic_oneline="$(printf "%s" "$panic_payload" | tr -d '\r\n')"
      printf '{"ts":"%s","ok":true,"data":%s}\n' "$now_utc" "$panic_oneline" >> "$PANIC_JSONL"
    else
      printf '{"ts":"%s","ok":false}\n' "$now_utc" >> "$PANIC_JSONL"
    fi
  fi

  if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && "$now_epoch" -ge "$next_display_drive_epoch" ]]; then
    if [[ "$display_drive_active_until_epoch" -gt "$now_epoch" ]]; then
      display_drive_skipped_while_active=$((display_drive_skipped_while_active + 1))
      next_display_drive_epoch="$display_drive_active_until_epoch"
    else
      display_drive_calls=$((display_drive_calls + 1))
      drive_resp="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" -X POST "$DISPLAY_PREVIEW_URL" 2>/dev/null || true)"
      if [[ -z "$drive_resp" ]]; then
        display_drive_errors=$((display_drive_errors + 1))
        echo "[WARN] Display drive call failed (no response)." | tee -a "$RUN_LOG"
        next_display_drive_epoch=$((now_epoch + DISPLAY_DRIVE_INTERVAL_SECONDS))
      elif [[ "$drive_resp" == *'"active":true'* ]]; then
        display_drive_active_until_epoch=$((now_epoch + DISPLAY_PREVIEW_HOLD_SECONDS))
        next_display_drive_epoch="$display_drive_active_until_epoch"
      else
        display_drive_start_misses=$((display_drive_start_misses + 1))
        echo "[WARN] Display preview was already active during a start attempt; suppressing retry churn." | tee -a "$RUN_LOG"
        display_drive_active_until_epoch=$((now_epoch + DISPLAY_PREVIEW_HOLD_SECONDS))
        next_display_drive_epoch="$display_drive_active_until_epoch"
      fi
    fi
  fi

  if [[ "$TRANSITION_DRIVE_ENABLED" -eq 1 &&
        "$now_epoch" -ge "$next_transition_drive_epoch" &&
        "$transition_flap_cycles_completed" -lt "$TRANSITION_FLAP_CYCLES" ]]; then
    transition_drive_calls=$((transition_drive_calls + 1))
    transition_resp="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" \
      -X POST --data "enabled=${transition_target_enabled}" \
      "$TRANSITION_CONTROL_URL" 2>/dev/null || true)"
    if [[ -z "$transition_resp" || "$transition_resp" != *'"success":true'* ]]; then
      transition_drive_errors=$((transition_drive_errors + 1))
      transition_resp_compact="$(printf "%s" "$transition_resp" | tr -d '\r\n')"
      transition_resp_compact="${transition_resp_compact:0:180}"
      if [[ -n "$transition_resp_compact" ]]; then
        echo "[WARN] Transition drive action failed (enabled=${transition_target_enabled} response=${transition_resp_compact})." | tee -a "$RUN_LOG"
      else
        echo "[WARN] Transition drive action failed (enabled=${transition_target_enabled})." | tee -a "$RUN_LOG"
      fi
    else
      if [[ "$transition_target_enabled" -eq 0 ]]; then
        transition_flap_off_calls=$((transition_flap_off_calls + 1))
        transition_flap_cycles_completed=$((transition_flap_cycles_completed + 1))
        transition_target_enabled=1
      else
        transition_flap_on_calls=$((transition_flap_on_calls + 1))
        transition_target_enabled=0
      fi
    fi
    next_transition_drive_epoch=$((now_epoch + TRANSITION_DRIVE_INTERVAL_SECONDS))
  fi

  sleep "$POLL_SECONDS"
done

if [[ "$TRANSITION_DRIVE_ENABLED" -eq 1 ]]; then
  transition_restore_ok=0
  transition_restore_resp=""
  transition_restore_attempts=0
  while [[ "$transition_restore_attempts" -lt "$transition_restore_max_attempts" ]]; do
    transition_restore_attempts=$((transition_restore_attempts + 1))
    transition_restore_resp="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" \
      -X POST --data "enabled=1" "$TRANSITION_CONTROL_URL" 2>/dev/null || true)"
    if [[ -n "$transition_restore_resp" && "$transition_restore_resp" == *'"success":true'* ]]; then
      transition_flap_restore_calls=$((transition_flap_restore_calls + 1))
      transition_restore_ok=1
      break
    fi
    if [[ "$transition_restore_attempts" -lt "$transition_restore_max_attempts" ]]; then
      sleep "$transition_restore_retry_delay_seconds"
    fi
  done
  if [[ "$transition_restore_ok" -ne 1 ]]; then
    transition_drive_errors=$((transition_drive_errors + 1))
    transition_restore_resp_compact="$(printf "%s" "$transition_restore_resp" | tr -d '\r\n')"
    transition_restore_resp_compact="${transition_restore_resp_compact:0:180}"
    if [[ -n "$transition_restore_resp_compact" ]]; then
      echo "[WARN] Transition drive restore failed (enabled=1 attempts=${transition_restore_attempts}/${transition_restore_max_attempts} response=${transition_restore_resp_compact})." | tee -a "$RUN_LOG"
    else
      echo "[WARN] Transition drive restore failed (enabled=1 attempts=${transition_restore_attempts}/${transition_restore_max_attempts})." | tee -a "$RUN_LOG"
    fi
  elif [[ "$transition_restore_attempts" -gt 1 ]]; then
    echo "[INFO] Transition drive restore succeeded after retry (${transition_restore_attempts}/${transition_restore_max_attempts})." | tee -a "$RUN_LOG"
  fi
fi

if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && -n "$DISPLAY_CLEAR_URL" ]]; then
  display_restore_resp="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" -X POST "$DISPLAY_CLEAR_URL" 2>/dev/null || true)"
  if [[ -z "$display_restore_resp" ]]; then
    display_drive_restore_errors=$((display_drive_restore_errors + 1))
    echo "[WARN] Display drive restore failed (no response)." | tee -a "$RUN_LOG"
  fi
fi

# Capture one final sample at/near soak end so throughput deltas span the full
# run window instead of ending early due poll/sleep cadence.
final_sample_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
if [[ -n "$METRICS_POLL_URL" ]]; then
  metrics_samples=$((metrics_samples + 1))
  final_payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$METRICS_POLL_URL" 2>/dev/null || true)"
  if [[ -n "$final_payload" ]]; then
    metrics_ok_samples=$((metrics_ok_samples + 1))
    final_payload_oneline="$(printf "%s" "$final_payload" | tr -d '\r\n')"
    printf '{"ts":"%s","ok":true,"data":%s}\n' "$final_sample_utc" "$final_payload_oneline" >> "$METRICS_JSONL"
  else
    printf '{"ts":"%s","ok":false}\n' "$final_sample_utc" >> "$METRICS_JSONL"
  fi
fi
if [[ -n "$PANIC_POLL_URL" ]]; then
  panic_samples=$((panic_samples + 1))
  final_panic_payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$PANIC_POLL_URL" 2>/dev/null || true)"
  if [[ -n "$final_panic_payload" ]]; then
    panic_ok_samples=$((panic_ok_samples + 1))
    final_panic_oneline="$(printf "%s" "$final_panic_payload" | tr -d '\r\n')"
    printf '{"ts":"%s","ok":true,"data":%s}\n' "$final_sample_utc" "$final_panic_oneline" >> "$PANIC_JSONL"
  else
    printf '{"ts":"%s","ok":false}\n' "$final_sample_utc" >> "$PANIC_JSONL"
  fi
fi

soak_end_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
soak_end_epoch_actual="$(date +%s)"
soak_elapsed_s=$((soak_end_epoch_actual - soak_start_epoch))

if wait "$MONITOR_PID"; then
  serial_capture_status=0
else
  serial_capture_status=$?
fi
if [[ "$serial_capture_status" -ne 0 ]]; then
  monitor_died_early=1
fi

trap - EXIT

serial_reset_count="$(grep -c 'rst:0x' "$SERIAL_LOG" || true)"
serial_wdt_or_panic_count="$(grep -Eic 'task watchdog|task_wdt|Guru Meditation|panic|abort\(' "$SERIAL_LOG" || true)"
serial_guru_count="$(grep -Eic 'Guru Meditation' "$SERIAL_LOG" || true)"

metrics_kv="$OUT_DIR/metrics_kv.txt"
panic_kv="$OUT_DIR/panic_kv.txt"

PARSER_PYTHON="$(find_python_for_csv || true)"
if [[ -z "$PARSER_PYTHON" ]]; then
  echo "No Python interpreter is available for soak JSON parsing." >&2
  exit 1
fi
metrics_parser_args=(
  "$ROOT_DIR/tools/soak_parse_metrics.py"
  "$METRICS_JSONL"
  "--skip-first-wifi-samples" "$WIFI_ROBUST_SKIP_FIRST_SAMPLES"
  "--stable-consecutive-samples" "$TRANSITION_STABLE_CONSECUTIVE_SAMPLES"
  "--connect-burst-consecutive-samples" "$CONNECT_BURST_STABLE_CONSECUTIVE_SAMPLES"
  "--exclude-tail-samples-for-minima" "$MINIMA_TAIL_EXCLUDE_SAMPLES"
)
if [[ "$MAX_BLE_PROCESS_MAX_US" -gt 0 ]]; then
  metrics_parser_args+=(--ble-threshold "$MAX_BLE_PROCESS_MAX_US")
fi
if [[ "$MAX_WIFI_MAX_US" -gt 0 ]]; then
  metrics_parser_args+=(--wifi-threshold "$MAX_WIFI_MAX_US")
fi
if [[ "$MAX_DISP_PIPE_MAX_US" -gt 0 ]]; then
  metrics_parser_args+=(--disp-threshold "$MAX_DISP_PIPE_MAX_US")
  metrics_parser_args+=(--connect-burst-disp-threshold "$MAX_DISP_PIPE_MAX_US")
fi
if [[ "$MIN_DMA_LARGEST" -gt 0 ]]; then
  metrics_parser_args+=(--dma-largest-floor "$MIN_DMA_LARGEST")
fi
if ! "$PARSER_PYTHON" "${metrics_parser_args[@]}" > "$metrics_kv"; then
  echo "Failed to parse soak metrics JSONL ($METRICS_JSONL)." >&2
  exit 1
fi
if ! "$PARSER_PYTHON" "$ROOT_DIR/tools/soak_parse_panic.py" "$PANIC_JSONL" > "$panic_kv"; then
  echo "Failed to parse soak panic JSONL ($PANIC_JSONL)." >&2
  exit 1
fi

metrics_samples_parsed=""
metrics_ok_samples_parsed=""
heap_free_min=""
heap_min_free_min=""
heap_dma_min=""
heap_dma_largest_min=""
latency_max_peak=""
proxy_drop_peak=""
display_updates_first=""
display_updates_last=""
display_updates_delta=""
display_skips_first=""
display_skips_last=""
display_skips_delta=""
flush_max_peak=""
loop_max_peak=""
wifi_max_peak=""
wifi_max_peak_excluding_first=""
ble_drain_max_peak=""
loop_peak_ts=""
loop_peak_wifi=""
loop_peak_flush=""
loop_peak_ble_drain=""
loop_peak_display_updates=""
loop_peak_rx_packets=""
wifi_peak_ts=""
wifi_peak_excluding_first_ts=""
wifi_peak_loop=""
wifi_peak_flush=""
wifi_peak_ble_drain=""
wifi_peak_display_updates=""
wifi_peak_rx_packets=""
flush_peak_ts=""
ble_drain_peak_ts=""
rx_packets_delta=""
parse_successes_delta=""
parse_failures_delta=""
queue_drops_delta=""
perf_drop_delta=""
event_publish_delta=""
event_drop_delta=""
event_size_peak=""
core_guard_tripped_count=""
oversize_drops_delta=""
sd_max_peak=""
fs_max_peak=""
queue_high_water_first=""
queue_high_water_peak=""
wifi_connect_deferred_delta=""
reconnects_delta=""
disconnects_delta=""
dma_free_min_parsed=""
dma_largest_min_parsed=""
dma_free_min_raw_parsed=""
dma_largest_min_raw_parsed=""
dma_largest_current_sample_count=""
dma_largest_below_floor_samples=""
dma_largest_below_floor_pct=""
dma_largest_below_floor_longest_streak=""
dma_largest_to_free_pct_min=""
dma_largest_to_free_pct_p05=""
dma_largest_to_free_pct_p50=""
dma_fragmentation_pct_p50=""
dma_fragmentation_pct_p95=""
dma_fragmentation_pct_max=""
minima_tail_samples_excluded=""
minima_samples_considered=""
inherited_counter_suspect=""
ble_process_max_peak=""
disp_pipe_max_peak=""
wifi_sample_count=""
wifi_sample_count_excluding_first=""
wifi_p95_raw=""
wifi_p95_excluding_first=""
wifi_over_limit_count_raw=""
wifi_over_limit_count_excluding_first=""
disp_pipe_sample_count=""
disp_pipe_p95=""
disp_pipe_over_limit_count=""
ble_mutex_timeout_delta=""
wifi_ap_up_transitions_delta=""
wifi_ap_down_transitions_delta=""
proxy_adv_on_transitions_delta=""
proxy_adv_off_transitions_delta=""
wifi_ap_active_samples=""
wifi_ap_inactive_samples=""
proxy_adv_on_samples=""
proxy_adv_off_samples=""
wifi_peak_ap_active=""
wifi_peak_ap_inactive=""
wifi_peak_proxy_adv_on=""
wifi_peak_proxy_adv_off=""
wifi_ap_last_reason_code=""
wifi_ap_last_reason=""
proxy_adv_last_reason_code=""
proxy_adv_last_reason=""
stable_consecutive_samples_required=""
wifi_ap_down_events_observed=""
wifi_ap_down_events_stabilized=""
wifi_ap_down_events_unstable=""
samples_to_stable_after_ap_down=""
time_to_stable_ms_after_ap_down=""
proxy_adv_off_events_observed=""
proxy_adv_off_events_stabilized=""
proxy_adv_off_events_unstable=""
samples_to_stable_after_proxy_adv_off=""
time_to_stable_ms_after_proxy_adv_off=""
transition_primary_source=""
transition_primary_event_index=""
transition_primary_stable_index=""
transition_primary_stabilized=""
samples_to_stable=""
time_to_stable_ms=""
window_pre_samples=""
window_pre_wifi_peak=""
window_pre_wifi_p95=""
window_pre_loop_peak=""
window_pre_loop_p95=""
window_pre_disp_pipe_peak=""
window_pre_disp_pipe_p95=""
window_transition_samples=""
window_transition_wifi_peak=""
window_transition_wifi_p95=""
window_transition_loop_peak=""
window_transition_loop_p95=""
window_transition_disp_pipe_peak=""
window_transition_disp_pipe_p95=""
window_post_stable_samples=""
window_post_stable_wifi_peak=""
window_post_stable_wifi_p95=""
window_post_stable_loop_peak=""
window_post_stable_loop_p95=""
window_post_stable_disp_pipe_peak=""
window_post_stable_disp_pipe_p95=""
connect_burst_detected=""
connect_burst_event_index=""
connect_burst_stable_index=""
connect_burst_stabilized=""
connect_burst_event_ble_state=""
connect_burst_event_subscribe_step=""
connect_burst_event_proxy_advertising=""
connect_burst_stable_consecutive_samples_required=""
connect_burst_samples_to_stable=""
connect_burst_time_to_stable_ms=""
connect_burst_pre_ble_process_peak=""
connect_burst_pre_disp_pipe_peak=""
connect_burst_ble_followup_request_alert_peak=""
connect_burst_ble_followup_request_version_peak=""
connect_burst_ble_connect_stable_callback_peak=""
connect_burst_ble_proxy_start_peak=""
connect_burst_disp_render_peak=""
connect_burst_display_voice_peak=""
connect_burst_display_gap_recover_peak=""

while IFS='=' read -r key value; do
  case "$key" in
    samples) metrics_samples_parsed="$value" ;;
    ok_samples) metrics_ok_samples_parsed="$value" ;;
    heap_free_min) heap_free_min="$value" ;;
    heap_min_free_min) heap_min_free_min="$value" ;;
    heap_dma_min) heap_dma_min="$value" ;;
    heap_dma_largest_min) heap_dma_largest_min="$value" ;;
    latency_max_peak) latency_max_peak="$value" ;;
    proxy_drop_peak) proxy_drop_peak="$value" ;;
    display_updates_first) display_updates_first="$value" ;;
    display_updates_last) display_updates_last="$value" ;;
    display_updates_delta) display_updates_delta="$value" ;;
    display_skips_first) display_skips_first="$value" ;;
    display_skips_last) display_skips_last="$value" ;;
    display_skips_delta) display_skips_delta="$value" ;;
    flush_max_peak) flush_max_peak="$value" ;;
    loop_max_peak) loop_max_peak="$value" ;;
    wifi_max_peak) wifi_max_peak="$value" ;;
    wifi_max_peak_excluding_first) wifi_max_peak_excluding_first="$value" ;;
    ble_drain_max_peak) ble_drain_max_peak="$value" ;;
    loop_peak_ts) loop_peak_ts="$value" ;;
    loop_peak_wifi) loop_peak_wifi="$value" ;;
    loop_peak_flush) loop_peak_flush="$value" ;;
    loop_peak_ble_drain) loop_peak_ble_drain="$value" ;;
    loop_peak_display_updates) loop_peak_display_updates="$value" ;;
    loop_peak_rx_packets) loop_peak_rx_packets="$value" ;;
    wifi_peak_ts) wifi_peak_ts="$value" ;;
    wifi_peak_excluding_first_ts) wifi_peak_excluding_first_ts="$value" ;;
    wifi_peak_loop) wifi_peak_loop="$value" ;;
    wifi_peak_flush) wifi_peak_flush="$value" ;;
    wifi_peak_ble_drain) wifi_peak_ble_drain="$value" ;;
    wifi_peak_display_updates) wifi_peak_display_updates="$value" ;;
    wifi_peak_rx_packets) wifi_peak_rx_packets="$value" ;;
    flush_peak_ts) flush_peak_ts="$value" ;;
    ble_drain_peak_ts) ble_drain_peak_ts="$value" ;;
    rx_packets_delta) rx_packets_delta="$value" ;;
    parse_successes_delta) parse_successes_delta="$value" ;;
    parse_failures_delta) parse_failures_delta="$value" ;;
    queue_drops_delta) queue_drops_delta="$value" ;;
    perf_drop_delta) perf_drop_delta="$value" ;;
    event_publish_delta) event_publish_delta="$value" ;;
    event_drop_delta) event_drop_delta="$value" ;;
    event_size_peak) event_size_peak="$value" ;;
    core_guard_tripped_count) core_guard_tripped_count="$value" ;;
    oversize_drops_delta) oversize_drops_delta="$value" ;;
    sd_max_peak) sd_max_peak="$value" ;;
    fs_max_peak) fs_max_peak="$value" ;;
    queue_high_water_first) queue_high_water_first="$value" ;;
    queue_high_water_peak) queue_high_water_peak="$value" ;;
    wifi_connect_deferred_delta) wifi_connect_deferred_delta="$value" ;;
    reconnects_delta) reconnects_delta="$value" ;;
    disconnects_delta) disconnects_delta="$value" ;;
    dma_free_min) dma_free_min_parsed="$value" ;;
    dma_largest_min) dma_largest_min_parsed="$value" ;;
    dma_free_min_raw) dma_free_min_raw_parsed="$value" ;;
    dma_largest_min_raw) dma_largest_min_raw_parsed="$value" ;;
    dma_largest_current_sample_count) dma_largest_current_sample_count="$value" ;;
    dma_largest_below_floor_samples) dma_largest_below_floor_samples="$value" ;;
    dma_largest_below_floor_pct) dma_largest_below_floor_pct="$value" ;;
    dma_largest_below_floor_longest_streak) dma_largest_below_floor_longest_streak="$value" ;;
    dma_largest_to_free_pct_min) dma_largest_to_free_pct_min="$value" ;;
    dma_largest_to_free_pct_p05) dma_largest_to_free_pct_p05="$value" ;;
    dma_largest_to_free_pct_p50) dma_largest_to_free_pct_p50="$value" ;;
    dma_fragmentation_pct_p50) dma_fragmentation_pct_p50="$value" ;;
    dma_fragmentation_pct_p95) dma_fragmentation_pct_p95="$value" ;;
    dma_fragmentation_pct_max) dma_fragmentation_pct_max="$value" ;;
    minima_tail_samples_excluded) minima_tail_samples_excluded="$value" ;;
    minima_samples_considered) minima_samples_considered="$value" ;;
    inherited_counter_suspect) inherited_counter_suspect="$value" ;;
    ble_process_max_peak) ble_process_max_peak="$value" ;;
    disp_pipe_max_peak) disp_pipe_max_peak="$value" ;;
    wifi_sample_count) wifi_sample_count="$value" ;;
    wifi_sample_count_excluding_first) wifi_sample_count_excluding_first="$value" ;;
    wifi_p95_raw) wifi_p95_raw="$value" ;;
    wifi_p95_excluding_first) wifi_p95_excluding_first="$value" ;;
    wifi_over_limit_count_raw) wifi_over_limit_count_raw="$value" ;;
    wifi_over_limit_count_excluding_first) wifi_over_limit_count_excluding_first="$value" ;;
    disp_pipe_sample_count) disp_pipe_sample_count="$value" ;;
    disp_pipe_p95) disp_pipe_p95="$value" ;;
    disp_pipe_over_limit_count) disp_pipe_over_limit_count="$value" ;;
    ble_mutex_timeout_delta) ble_mutex_timeout_delta="$value" ;;
    wifi_ap_up_transitions_delta) wifi_ap_up_transitions_delta="$value" ;;
    wifi_ap_down_transitions_delta) wifi_ap_down_transitions_delta="$value" ;;
    proxy_adv_on_transitions_delta) proxy_adv_on_transitions_delta="$value" ;;
    proxy_adv_off_transitions_delta) proxy_adv_off_transitions_delta="$value" ;;
    wifi_ap_active_samples) wifi_ap_active_samples="$value" ;;
    wifi_ap_inactive_samples) wifi_ap_inactive_samples="$value" ;;
    proxy_adv_on_samples) proxy_adv_on_samples="$value" ;;
    proxy_adv_off_samples) proxy_adv_off_samples="$value" ;;
    wifi_peak_ap_active) wifi_peak_ap_active="$value" ;;
    wifi_peak_ap_inactive) wifi_peak_ap_inactive="$value" ;;
    wifi_peak_proxy_adv_on) wifi_peak_proxy_adv_on="$value" ;;
    wifi_peak_proxy_adv_off) wifi_peak_proxy_adv_off="$value" ;;
    wifi_ap_last_reason_code) wifi_ap_last_reason_code="$value" ;;
    wifi_ap_last_reason) wifi_ap_last_reason="$value" ;;
    proxy_adv_last_reason_code) proxy_adv_last_reason_code="$value" ;;
    proxy_adv_last_reason) proxy_adv_last_reason="$value" ;;
    stable_consecutive_samples_required) stable_consecutive_samples_required="$value" ;;
    wifi_ap_down_events_observed) wifi_ap_down_events_observed="$value" ;;
    wifi_ap_down_events_stabilized) wifi_ap_down_events_stabilized="$value" ;;
    wifi_ap_down_events_unstable) wifi_ap_down_events_unstable="$value" ;;
    samples_to_stable_after_ap_down) samples_to_stable_after_ap_down="$value" ;;
    time_to_stable_ms_after_ap_down) time_to_stable_ms_after_ap_down="$value" ;;
    proxy_adv_off_events_observed) proxy_adv_off_events_observed="$value" ;;
    proxy_adv_off_events_stabilized) proxy_adv_off_events_stabilized="$value" ;;
    proxy_adv_off_events_unstable) proxy_adv_off_events_unstable="$value" ;;
    samples_to_stable_after_proxy_adv_off) samples_to_stable_after_proxy_adv_off="$value" ;;
    time_to_stable_ms_after_proxy_adv_off) time_to_stable_ms_after_proxy_adv_off="$value" ;;
    transition_primary_source) transition_primary_source="$value" ;;
    transition_primary_event_index) transition_primary_event_index="$value" ;;
    transition_primary_stable_index) transition_primary_stable_index="$value" ;;
    transition_primary_stabilized) transition_primary_stabilized="$value" ;;
    samples_to_stable) samples_to_stable="$value" ;;
    time_to_stable_ms) time_to_stable_ms="$value" ;;
    window_pre_samples) window_pre_samples="$value" ;;
    window_pre_wifi_peak) window_pre_wifi_peak="$value" ;;
    window_pre_wifi_p95) window_pre_wifi_p95="$value" ;;
    window_pre_loop_peak) window_pre_loop_peak="$value" ;;
    window_pre_loop_p95) window_pre_loop_p95="$value" ;;
    window_pre_disp_pipe_peak) window_pre_disp_pipe_peak="$value" ;;
    window_pre_disp_pipe_p95) window_pre_disp_pipe_p95="$value" ;;
    window_transition_samples) window_transition_samples="$value" ;;
    window_transition_wifi_peak) window_transition_wifi_peak="$value" ;;
    window_transition_wifi_p95) window_transition_wifi_p95="$value" ;;
    window_transition_loop_peak) window_transition_loop_peak="$value" ;;
    window_transition_loop_p95) window_transition_loop_p95="$value" ;;
    window_transition_disp_pipe_peak) window_transition_disp_pipe_peak="$value" ;;
    window_transition_disp_pipe_p95) window_transition_disp_pipe_p95="$value" ;;
    window_post_stable_samples) window_post_stable_samples="$value" ;;
    window_post_stable_wifi_peak) window_post_stable_wifi_peak="$value" ;;
    window_post_stable_wifi_p95) window_post_stable_wifi_p95="$value" ;;
    window_post_stable_loop_peak) window_post_stable_loop_peak="$value" ;;
    window_post_stable_loop_p95) window_post_stable_loop_p95="$value" ;;
    window_post_stable_disp_pipe_peak) window_post_stable_disp_pipe_peak="$value" ;;
    window_post_stable_disp_pipe_p95) window_post_stable_disp_pipe_p95="$value" ;;
    connect_burst_detected) connect_burst_detected="$value" ;;
    connect_burst_event_index) connect_burst_event_index="$value" ;;
    connect_burst_stable_index) connect_burst_stable_index="$value" ;;
    connect_burst_stabilized) connect_burst_stabilized="$value" ;;
    connect_burst_event_ble_state) connect_burst_event_ble_state="$value" ;;
    connect_burst_event_subscribe_step) connect_burst_event_subscribe_step="$value" ;;
    connect_burst_event_proxy_advertising) connect_burst_event_proxy_advertising="$value" ;;
    connect_burst_stable_consecutive_samples_required) connect_burst_stable_consecutive_samples_required="$value" ;;
    connect_burst_samples_to_stable) connect_burst_samples_to_stable="$value" ;;
    connect_burst_time_to_stable_ms) connect_burst_time_to_stable_ms="$value" ;;
    connect_burst_pre_ble_process_peak) connect_burst_pre_ble_process_peak="$value" ;;
    connect_burst_pre_disp_pipe_peak) connect_burst_pre_disp_pipe_peak="$value" ;;
    connect_burst_ble_followup_request_alert_peak) connect_burst_ble_followup_request_alert_peak="$value" ;;
    connect_burst_ble_followup_request_version_peak) connect_burst_ble_followup_request_version_peak="$value" ;;
    connect_burst_ble_connect_stable_callback_peak) connect_burst_ble_connect_stable_callback_peak="$value" ;;
    connect_burst_ble_proxy_start_peak) connect_burst_ble_proxy_start_peak="$value" ;;
    connect_burst_disp_render_peak) connect_burst_disp_render_peak="$value" ;;
    connect_burst_display_voice_peak) connect_burst_display_voice_peak="$value" ;;
    connect_burst_display_gap_recover_peak) connect_burst_display_gap_recover_peak="$value" ;;
    connect_burst_display_*_peak|display_*_delta|display_*_peak|display_*_px) printf -v "$key" '%s' "$value" ;;
  esac
done < "$metrics_kv"

panic_samples_parsed=""
panic_ok_samples_parsed=""
panic_was_crash_true=""
panic_has_panic_file_true=""
panic_first_was_crash=""
panic_last_was_crash=""
panic_first_has_panic_file=""
panic_last_has_panic_file=""
panic_first_reset_reason=""
panic_last_reset_reason=""
panic_state_change_count=""

while IFS='=' read -r key value; do
  case "$key" in
    samples) panic_samples_parsed="$value" ;;
    ok_samples) panic_ok_samples_parsed="$value" ;;
    was_crash_true) panic_was_crash_true="$value" ;;
    has_panic_file_true) panic_has_panic_file_true="$value" ;;
    first_was_crash) panic_first_was_crash="$value" ;;
    last_was_crash) panic_last_was_crash="$value" ;;
    first_has_panic_file) panic_first_has_panic_file="$value" ;;
    last_has_panic_file) panic_last_has_panic_file="$value" ;;
    first_reset_reason) panic_first_reset_reason="$value" ;;
    last_reset_reason) panic_last_reset_reason="$value" ;;
    state_change_count) panic_state_change_count="$value" ;;
  esac
done < "$panic_kv"

reboot_evidence_detected=0
reboot_evidence_detail_parts=()
reboot_evidence_detail="none"
panic_endpoint_new_crash_detected=0
if [[ "$serial_reset_count" -gt 0 ]]; then
  reboot_evidence_detected=1
  reboot_evidence_detail_parts+=("serial_rst=${serial_reset_count}")
fi
if [[ "$serial_wdt_or_panic_count" -gt 0 ]]; then
  reboot_evidence_detected=1
  reboot_evidence_detail_parts+=("serial_panic_signatures=${serial_wdt_or_panic_count}")
fi
if is_uint "$panic_first_was_crash" &&
   is_uint "$panic_last_was_crash" &&
   is_uint "$panic_state_change_count" &&
   [[ "$panic_last_was_crash" -eq 1 ]] &&
   ([[ "$panic_first_was_crash" -eq 0 ]] || [[ "$panic_state_change_count" -gt 0 ]]); then
  panic_endpoint_new_crash_detected=1
  reboot_evidence_detected=1
  reboot_evidence_detail_parts+=(
    "panic_endpoint_runtime_crash=first:${panic_first_was_crash},last:${panic_last_was_crash},changes:${panic_state_change_count}"
  )
fi
if [[ ${#reboot_evidence_detail_parts[@]} -gt 0 ]]; then
  reboot_evidence_detail="${reboot_evidence_detail_parts[0]}"
  for ((i = 1; i < ${#reboot_evidence_detail_parts[@]}; ++i)); do
    reboot_evidence_detail+=", ${reboot_evidence_detail_parts[$i]}"
  done
fi

declare -a fail_reasons=()
add_fail_reason() {
  local reason="$1"
  local existing
  for existing in "${fail_reasons[@]:-}"; do
    if [[ "$existing" == "$reason" ]]; then
      return 0
    fi
  done
  fail_reasons+=("$reason")
}

mark_gate_fail() {
  local gate_var="$1"
  local reason="$2"
  printf -v "$gate_var" '%s' 1
  add_fail_reason "$reason"
}

is_uint() {
  local value="${1:-}"
  [[ "$value" =~ ^[0-9]+$ ]]
}

is_int() {
  local value="${1:-}"
  [[ "$value" =~ ^-?[0-9]+$ ]]
}

calc_allowed_exceeds() {
  local sample_count="${1:-}"
  local exceed_pct="${2:-}"
  if ! is_uint "$sample_count" || ! is_uint "$exceed_pct"; then
    echo ""
    return 1
  fi
  if [[ "$sample_count" -le 0 ]]; then
    echo "0"
    return 0
  fi
  if [[ "$exceed_pct" -eq 0 ]]; then
    echo "0"
    return 0
  fi
  # Integer ceil(sample_count * pct / 100)
  local allowed=$(( (sample_count * exceed_pct + 99) / 100 ))
  if [[ "$allowed" -lt 1 ]]; then
    allowed=1
  fi
  echo "$allowed"
}

result="PASS"
serial_log_bytes="$(wc -c < "$SERIAL_LOG" | tr -d '[:space:]')"
if [[ -z "$serial_log_bytes" ]]; then
  serial_log_bytes=0
fi
signal_sources=0
if [[ "$serial_log_bytes" -gt 0 ]]; then
  signal_sources=$((signal_sources + 1))
fi
if is_uint "$metrics_ok_samples_parsed" && [[ "$metrics_ok_samples_parsed" -gt 0 ]]; then
  signal_sources=$((signal_sources + 1))
fi
if is_uint "$panic_ok_samples_parsed" && [[ "$panic_ok_samples_parsed" -gt 0 ]]; then
  signal_sources=$((signal_sources + 1))
fi

have_metrics_window=0
if [[ -n "$METRICS_URL" ]] && is_uint "$metrics_ok_samples_parsed" && [[ "$metrics_ok_samples_parsed" -gt 0 ]]; then
  have_metrics_window=1
fi

wifi_peak_gate_value="$wifi_max_peak"
wifi_peak_gate_basis="raw_peak"
if [[ ( "$metrics_reset_success" -eq 1 || ( "$STARTUP_SETTLE_APPLIED" -eq 1 && "$STARTUP_SETTLE_SUCCEEDED" -eq 1 ) ) ]] &&
   is_uint "$metrics_ok_samples_parsed" &&
   [[ "$metrics_ok_samples_parsed" -gt 1 ]] &&
   is_uint "$wifi_max_peak_excluding_first"
then
  wifi_peak_gate_value="$wifi_max_peak_excluding_first"
  wifi_peak_gate_basis="excluding_first_sample"
fi

wifi_robust_sample_count="$wifi_sample_count"
wifi_robust_over_limit_count="$wifi_over_limit_count_raw"
wifi_robust_p95="$wifi_p95_raw"
wifi_robust_basis="raw_samples"
if [[ ( "$metrics_reset_success" -eq 1 || ( "$STARTUP_SETTLE_APPLIED" -eq 1 && "$STARTUP_SETTLE_SUCCEEDED" -eq 1 ) ) ]] &&
   is_uint "$metrics_ok_samples_parsed" &&
   [[ "$metrics_ok_samples_parsed" -gt "$WIFI_ROBUST_SKIP_FIRST_SAMPLES" ]] &&
   is_uint "$wifi_sample_count_excluding_first" &&
   [[ "$wifi_sample_count_excluding_first" -gt 0 ]]
then
  wifi_robust_sample_count="$wifi_sample_count_excluding_first"
  wifi_robust_over_limit_count="$wifi_over_limit_count_excluding_first"
  wifi_robust_p95="$wifi_p95_excluding_first"
  wifi_robust_basis="excluding_first_${WIFI_ROBUST_SKIP_FIRST_SAMPLES}_samples"
fi

wifi_robust_allowed_over_limit=""
disp_pipe_robust_allowed_over_limit=""

gate_rx_fail=0
gate_parse_success_fail=0
gate_parse_fail_fail=0
gate_queue_drop_fail=0
gate_perf_drop_fail=0
gate_event_drop_fail=0
gate_flush_fail=0
gate_loop_fail=0
gate_wifi_fail=0
gate_ble_drain_fail=0
gate_metrics_window_fail=0
gate_metrics_min_samples_fail=0
gate_serial_monitor_fail=0
gate_serial_panic_fail=0
gate_serial_reset_fail=0
gate_panic_endpoint_fail=0
gate_display_drive_fail=0
gate_oversize_drop_fail=0
gate_sd_max_fail=0
gate_fs_max_fail=0
gate_queue_high_water_fail=0
gate_wifi_connect_deferred_fail=0
gate_dma_free_fail=0
gate_dma_largest_fail=0
gate_ble_process_max_fail=0
gate_disp_pipe_max_fail=0
gate_ble_mutex_timeout_fail=0
gate_transition_churn_ap_fail=0
gate_transition_churn_proxy_fail=0
gate_transition_min_events_fail=0
gate_transition_recovery_ap_fail=0
gate_transition_recovery_proxy_fail=0
gate_transition_samples_fail=0
gate_transition_drive_fail=0
gate_startup_settle_fail=0

# Advisory SLO tracking (warn-only, do not cause FAIL)
advisory_warnings=()

if [[ "$monitor_died_early" -eq 1 ]]; then
  mark_gate_fail gate_serial_monitor_fail "Serial capture exited during soak."
fi
if [[ "$serial_wdt_or_panic_count" -gt 0 ]]; then
  mark_gate_fail gate_serial_panic_fail "Serial panic/WDT signatures detected (${serial_wdt_or_panic_count})."
fi
if [[ "$serial_reset_count" -gt 0 ]]; then
  mark_gate_fail gate_serial_reset_fail "Serial reset signatures detected (${serial_reset_count} rst:0x lines). Device rebooted during soak."
fi
if [[ "$panic_endpoint_new_crash_detected" -eq 1 ]]; then
  mark_gate_fail gate_panic_endpoint_fail \
    "Panic endpoint changed to/reported a crash during soak (first=${panic_first_was_crash:-n/a} last=${panic_last_was_crash:-n/a} changes=${panic_state_change_count:-n/a})."
fi
if is_uint "$panic_first_was_crash" &&
   is_uint "$panic_last_was_crash" &&
   is_uint "$panic_state_change_count" &&
   [[ "$panic_first_was_crash" -eq 1 ]] &&
   [[ "$panic_last_was_crash" -eq 1 ]] &&
   [[ "$panic_state_change_count" -eq 0 ]]; then
  advisory_warnings+=(
    "Panic endpoint reported a preexisting crash state before soak and it remained unchanged during this run."
  )
fi
if [[ "$reboot_evidence_detected" -eq 0 ]]; then
  if [[ "$METRICS_REQUIRED" -eq 1 ]]; then
    if ! is_uint "$metrics_ok_samples_parsed"; then
      mark_gate_fail gate_metrics_min_samples_fail "Metrics gate could not be evaluated (no parsed samples)."
    elif [[ "$metrics_ok_samples_parsed" -lt "$MIN_METRICS_OK_SAMPLES" ]]; then
      mark_gate_fail gate_metrics_min_samples_fail "Metrics parsed successes ${metrics_ok_samples_parsed} below required ${MIN_METRICS_OK_SAMPLES}."
    fi
  fi

  if [[ -n "$METRICS_URL" ]]; then
    if [[ "$STARTUP_SETTLE_APPLIED" -eq 1 && "$STARTUP_SETTLE_SUCCEEDED" -ne 1 ]]; then
      mark_gate_fail gate_startup_settle_fail \
        "Runtime did not settle within ${STARTUP_SETTLE_MAX_SECONDS}s after metrics recovery (last uptimeMs=${STARTUP_SETTLE_LAST_UPTIME_MS:-n/a} loopMaxUs=${STARTUP_SETTLE_LAST_LOOP_MAX_US:-n/a} dispPipeMaxUs=${STARTUP_SETTLE_LAST_DISP_PIPE_MAX_US:-n/a} wifiMaxUs=${STARTUP_SETTLE_LAST_WIFI_MAX_US:-n/a})."
    fi
    if [[ "$have_metrics_window" -eq 0 ]]; then
      if [[ "$METRICS_REQUIRED" -eq 1 ]]; then
        mark_gate_fail gate_metrics_window_fail "No successful metrics samples captured from ${METRICS_URL}."
      fi
    else
    if ! is_uint "$rx_packets_delta"; then
      mark_gate_fail gate_rx_fail "rxPackets delta ${rx_packets_delta:-n/a} below minimum ${MIN_RX_PACKETS_DELTA}."
    elif [[ "$rx_packets_delta" -lt "$MIN_RX_PACKETS_DELTA" ]]; then
      mark_gate_fail gate_rx_fail "rxPackets delta ${rx_packets_delta} below minimum ${MIN_RX_PACKETS_DELTA}."
    fi
    if ! is_uint "$parse_successes_delta"; then
      mark_gate_fail gate_parse_success_fail "parseSuccesses delta ${parse_successes_delta:-n/a} below minimum ${MIN_PARSE_SUCCESSES_DELTA}."
    elif [[ "$parse_successes_delta" -lt "$MIN_PARSE_SUCCESSES_DELTA" ]]; then
      mark_gate_fail gate_parse_success_fail "parseSuccesses delta ${parse_successes_delta} below minimum ${MIN_PARSE_SUCCESSES_DELTA}."
    fi
    if ! is_uint "$parse_failures_delta"; then
      mark_gate_fail gate_parse_fail_fail "parseFailures delta ${parse_failures_delta:-n/a} above max ${MAX_PARSE_FAILURES_DELTA}."
    elif [[ "$parse_failures_delta" -gt "$MAX_PARSE_FAILURES_DELTA" ]]; then
      mark_gate_fail gate_parse_fail_fail "parseFailures delta ${parse_failures_delta} above max ${MAX_PARSE_FAILURES_DELTA}."
    fi
    if ! is_uint "$queue_drops_delta"; then
      mark_gate_fail gate_queue_drop_fail "queueDrops delta ${queue_drops_delta:-n/a} above max ${MAX_QUEUE_DROPS_DELTA}."
    elif [[ "$queue_drops_delta" -gt "$MAX_QUEUE_DROPS_DELTA" ]]; then
      mark_gate_fail gate_queue_drop_fail "queueDrops delta ${queue_drops_delta} above max ${MAX_QUEUE_DROPS_DELTA}."
    fi
    if ! is_uint "$perf_drop_delta"; then
      mark_gate_fail gate_perf_drop_fail "perfDrop delta ${perf_drop_delta:-n/a} above max ${MAX_PERF_DROPS_DELTA}."
    elif [[ "$perf_drop_delta" -gt "$MAX_PERF_DROPS_DELTA" ]]; then
      mark_gate_fail gate_perf_drop_fail "perfDrop delta ${perf_drop_delta} above max ${MAX_PERF_DROPS_DELTA}."
    fi
    if ! is_uint "$event_drop_delta"; then
      mark_gate_fail gate_event_drop_fail "eventBus drop delta ${event_drop_delta:-n/a} above max ${MAX_EVENT_DROPS_DELTA}."
    elif [[ "$event_drop_delta" -gt "$MAX_EVENT_DROPS_DELTA" ]]; then
      mark_gate_fail gate_event_drop_fail "eventBus drop delta ${event_drop_delta} above max ${MAX_EVENT_DROPS_DELTA}."
    fi

    if [[ "$MAX_FLUSH_MAX_US" -gt 0 ]]; then
      if ! is_uint "$flush_max_peak"; then
        mark_gate_fail gate_flush_fail "flushMaxUs peak ${flush_max_peak:-n/a} above max ${MAX_FLUSH_MAX_US}."
      elif [[ "$flush_max_peak" -gt "$MAX_FLUSH_MAX_US" ]]; then
        mark_gate_fail gate_flush_fail "flushMaxUs peak ${flush_max_peak} above max ${MAX_FLUSH_MAX_US}."
      fi
    fi
    if [[ "$MAX_LOOP_MAX_US" -gt 0 ]]; then
      if ! is_uint "$loop_max_peak"; then
        mark_gate_fail gate_loop_fail "loopMaxUs peak ${loop_max_peak:-n/a} above max ${MAX_LOOP_MAX_US}."
      elif [[ "$loop_max_peak" -gt "$MAX_LOOP_MAX_US" ]]; then
        mark_gate_fail gate_loop_fail "loopMaxUs peak ${loop_max_peak} above max ${MAX_LOOP_MAX_US}."
      fi
    fi
    if [[ "$MAX_WIFI_MAX_US" -gt 0 ]]; then
      wifi_strict_fail=0
      wifi_strict_reason=""
      if ! is_uint "$wifi_peak_gate_value"; then
        wifi_strict_fail=1
        wifi_strict_reason="wifiMaxUs peak (${wifi_peak_gate_basis}) ${wifi_peak_gate_value:-n/a} above max ${MAX_WIFI_MAX_US}."
      elif [[ "$wifi_peak_gate_value" -gt "$MAX_WIFI_MAX_US" ]]; then
        wifi_strict_fail=1
        wifi_strict_reason="wifiMaxUs peak (${wifi_peak_gate_basis}) ${wifi_peak_gate_value} above max ${MAX_WIFI_MAX_US}."
      fi

      wifi_robust_available=1
      wifi_robust_fail=0
      wifi_robust_reason=""
      wifi_robust_allowed_over_limit="$(calc_allowed_exceeds "$wifi_robust_sample_count" "$LATENCY_ROBUST_MAX_EXCEED_PCT" || true)"
      if ! is_uint "$wifi_robust_sample_count" || ! is_uint "$wifi_robust_over_limit_count"; then
        wifi_robust_available=0
        wifi_robust_reason="wifiMaxUs robust gate unavailable (samples=${wifi_robust_sample_count:-n/a} overLimit=${wifi_robust_over_limit_count:-n/a} basis=${wifi_robust_basis})."
      elif [[ "$wifi_robust_sample_count" -lt "$LATENCY_ROBUST_MIN_SAMPLES" ]]; then
        wifi_robust_available=0
        wifi_robust_reason="wifiMaxUs robust gate unavailable (samples=${wifi_robust_sample_count} below min ${LATENCY_ROBUST_MIN_SAMPLES}, basis=${wifi_robust_basis})."
      elif ! is_uint "$wifi_robust_allowed_over_limit"; then
        wifi_robust_available=0
        wifi_robust_reason="wifiMaxUs robust gate unavailable (could not compute allowed over-limit count)."
      elif [[ "$wifi_robust_over_limit_count" -gt "$wifi_robust_allowed_over_limit" ]]; then
        wifi_robust_fail=1
        wifi_robust_reason="wifiMaxUs robust over-limit ${wifi_robust_over_limit_count}/${wifi_robust_sample_count} (allowed ${wifi_robust_allowed_over_limit}, p95=${wifi_robust_p95:-n/a}, basis=${wifi_robust_basis}) above max ${MAX_WIFI_MAX_US}."
      fi

      case "$LATENCY_GATE_MODE" in
        strict)
          if [[ "$wifi_strict_fail" -eq 1 ]]; then
            mark_gate_fail gate_wifi_fail "$wifi_strict_reason"
          fi
          ;;
        robust)
          if [[ "$wifi_robust_available" -eq 0 ]]; then
            mark_gate_fail gate_wifi_fail "$wifi_robust_reason"
          elif [[ "$wifi_robust_fail" -eq 1 ]]; then
            mark_gate_fail gate_wifi_fail "$wifi_robust_reason"
          fi
          ;;
        hybrid)
          if [[ "$wifi_robust_available" -eq 1 ]]; then
            if [[ "$wifi_robust_fail" -eq 1 ]]; then
              mark_gate_fail gate_wifi_fail "$wifi_robust_reason"
            elif [[ "$wifi_strict_fail" -eq 1 ]]; then
              advisory_warnings+=("wifiMaxUs strict peak exceeded (${wifi_peak_gate_value}) but hybrid robust gate passed (over=${wifi_robust_over_limit_count}/${wifi_robust_sample_count}, allowed=${wifi_robust_allowed_over_limit}, p95=${wifi_robust_p95:-n/a}, basis=${wifi_robust_basis}).")
            fi
          elif [[ "$wifi_strict_fail" -eq 1 ]]; then
            mark_gate_fail gate_wifi_fail "${wifi_strict_reason} (robust unavailable: ${wifi_robust_reason})"
          fi
          ;;
      esac
    fi
    if [[ "$MAX_BLE_DRAIN_MAX_US" -gt 0 ]]; then
      if ! is_uint "$ble_drain_max_peak"; then
        mark_gate_fail gate_ble_drain_fail "bleDrainMaxUs peak ${ble_drain_max_peak:-n/a} above max ${MAX_BLE_DRAIN_MAX_US}."
      elif [[ "$ble_drain_max_peak" -gt "$MAX_BLE_DRAIN_MAX_US" ]]; then
        mark_gate_fail gate_ble_drain_fail "bleDrainMaxUs peak ${ble_drain_max_peak} above max ${MAX_BLE_DRAIN_MAX_US}."
      fi
    fi
    if [[ "$MAX_SD_MAX_US" -gt 0 ]]; then
      if ! is_uint "$sd_max_peak"; then
        mark_gate_fail gate_sd_max_fail "sdMaxUs peak ${sd_max_peak:-n/a} above max ${MAX_SD_MAX_US}."
      elif [[ "$sd_max_peak" -gt "$MAX_SD_MAX_US" ]]; then
        mark_gate_fail gate_sd_max_fail "sdMaxUs peak ${sd_max_peak} above max ${MAX_SD_MAX_US}."
      fi
    fi
    if [[ "$MAX_FS_MAX_US" -gt 0 ]]; then
      if ! is_uint "$fs_max_peak"; then
        mark_gate_fail gate_fs_max_fail "fsMaxUs peak ${fs_max_peak:-n/a} above max ${MAX_FS_MAX_US}."
      elif [[ "$fs_max_peak" -gt "$MAX_FS_MAX_US" ]]; then
        mark_gate_fail gate_fs_max_fail "fsMaxUs peak ${fs_max_peak} above max ${MAX_FS_MAX_US}."
      fi
    fi

    # oversizeDrops must be available and within threshold (packet framing safety)
    if ! is_uint "$oversize_drops_delta"; then
      mark_gate_fail gate_oversize_drop_fail "oversizeDrops delta ${oversize_drops_delta:-n/a} above max ${MAX_OVERSIZE_DROPS_DELTA}."
    elif [[ "$oversize_drops_delta" -gt "$MAX_OVERSIZE_DROPS_DELTA" ]]; then
      mark_gate_fail gate_oversize_drop_fail "oversizeDrops delta ${oversize_drops_delta} above max ${MAX_OVERSIZE_DROPS_DELTA}."
    fi

    # queueHighWater (resource gate, 0 disables)
    if [[ "$MAX_QUEUE_HIGH_WATER" -gt 0 ]]; then
      if ! is_uint "$queue_high_water_peak"; then
        mark_gate_fail gate_queue_high_water_fail "queueHighWater peak ${queue_high_water_peak:-n/a} above max ${MAX_QUEUE_HIGH_WATER}."
      elif [[ "$queue_high_water_peak" -gt "$MAX_QUEUE_HIGH_WATER" ]]; then
        mark_gate_fail gate_queue_high_water_fail "queueHighWater peak ${queue_high_water_peak} above max ${MAX_QUEUE_HIGH_WATER}."
      fi
    fi

    # wifiConnectDeferred (profile-specific; drive_wifi_off must be exactly 0)
    if [[ -n "$SOAK_PROFILE" ]]; then
      if ! is_uint "$wifi_connect_deferred_delta"; then
        mark_gate_fail gate_wifi_connect_deferred_fail "wifiConnectDeferred delta ${wifi_connect_deferred_delta:-n/a} unavailable for profile ${SOAK_PROFILE}."
      elif [[ "$SOAK_PROFILE" == "drive_wifi_off" && "$wifi_connect_deferred_delta" -gt 0 ]]; then
        mark_gate_fail gate_wifi_connect_deferred_fail "wifiConnectDeferred delta ${wifi_connect_deferred_delta} must be 0 for profile drive_wifi_off."
      elif [[ "$MAX_WIFI_CONNECT_DEFERRED" -gt 0 && "$wifi_connect_deferred_delta" -gt "$MAX_WIFI_CONNECT_DEFERRED" ]]; then
        mark_gate_fail gate_wifi_connect_deferred_fail "wifiConnectDeferred delta ${wifi_connect_deferred_delta} above max ${MAX_WIFI_CONNECT_DEFERRED}."
      fi
    fi

    # DMA memory floors (0 disables)
    if [[ "$MIN_DMA_FREE" -gt 0 ]]; then
      if ! is_uint "$dma_free_min_parsed"; then
        mark_gate_fail gate_dma_free_fail "heapDmaMin ${dma_free_min_parsed:-n/a} below floor ${MIN_DMA_FREE}."
      elif [[ "$dma_free_min_parsed" -lt "$MIN_DMA_FREE" ]]; then
        mark_gate_fail gate_dma_free_fail "heapDmaMin ${dma_free_min_parsed} below floor ${MIN_DMA_FREE}."
      fi
    fi
    if [[ "$MIN_DMA_LARGEST" -gt 0 ]]; then
      dma_largest_triage=""
      if is_uint "$dma_largest_below_floor_samples" && is_uint "$dma_largest_current_sample_count"; then
        dma_largest_triage+=" currentBelowFloor=${dma_largest_below_floor_samples}/${dma_largest_current_sample_count}"
      fi
      if [[ -n "$dma_largest_below_floor_pct" ]]; then
        dma_largest_triage+=" belowFloorPct=${dma_largest_below_floor_pct}%"
      fi
      if is_uint "$dma_largest_below_floor_longest_streak"; then
        dma_largest_triage+=" longestStreak=${dma_largest_below_floor_longest_streak}"
      fi
      if [[ -n "$dma_largest_to_free_pct_p05" || -n "$dma_largest_to_free_pct_p50" ]]; then
        dma_largest_triage+=" largestToFreePct(p05/p50)=${dma_largest_to_free_pct_p05:-n/a}/${dma_largest_to_free_pct_p50:-n/a}"
      fi
      if [[ -n "$dma_fragmentation_pct_p50" || -n "$dma_fragmentation_pct_p95" ]]; then
        dma_largest_triage+=" fragmentationPct(p50/p95)=${dma_fragmentation_pct_p50:-n/a}/${dma_fragmentation_pct_p95:-n/a}"
      fi
      if [[ -n "$dma_largest_triage" ]]; then
        dma_largest_triage=" [DMA triage:${dma_largest_triage}]"
      fi
      if ! is_uint "$dma_largest_min_parsed"; then
        mark_gate_fail gate_dma_largest_fail "heapDmaLargestMin ${dma_largest_min_parsed:-n/a} below floor ${MIN_DMA_LARGEST}.${dma_largest_triage}"
      elif [[ "$dma_largest_min_parsed" -lt "$MIN_DMA_LARGEST" ]]; then
        mark_gate_fail gate_dma_largest_fail "heapDmaLargestMin ${dma_largest_min_parsed} below floor ${MIN_DMA_LARGEST}.${dma_largest_triage}"
      fi
    fi

    # bleProcessMaxUs (0 disables)
    if [[ "$MAX_BLE_PROCESS_MAX_US" -gt 0 ]]; then
      if ! is_uint "$ble_process_max_peak"; then
        mark_gate_fail gate_ble_process_max_fail "bleProcessMaxUs peak ${ble_process_max_peak:-n/a} above max ${MAX_BLE_PROCESS_MAX_US}."
      elif [[ "$ble_process_max_peak" -gt "$MAX_BLE_PROCESS_MAX_US" ]]; then
        mark_gate_fail gate_ble_process_max_fail "bleProcessMaxUs peak ${ble_process_max_peak} above max ${MAX_BLE_PROCESS_MAX_US}."
      fi
    fi

    # dispPipeMaxUs (0 disables)
    if [[ "$MAX_DISP_PIPE_MAX_US" -gt 0 ]]; then
      disp_pipe_strict_fail=0
      disp_pipe_strict_reason=""
      if ! is_uint "$disp_pipe_max_peak"; then
        disp_pipe_strict_fail=1
        disp_pipe_strict_reason="dispPipeMaxUs peak ${disp_pipe_max_peak:-n/a} above max ${MAX_DISP_PIPE_MAX_US}."
      elif [[ "$disp_pipe_max_peak" -gt "$MAX_DISP_PIPE_MAX_US" ]]; then
        disp_pipe_strict_fail=1
        disp_pipe_strict_reason="dispPipeMaxUs peak ${disp_pipe_max_peak} above max ${MAX_DISP_PIPE_MAX_US}."
      fi

      disp_pipe_robust_available=1
      disp_pipe_robust_fail=0
      disp_pipe_robust_reason=""
      disp_pipe_robust_allowed_over_limit="$(calc_allowed_exceeds "$disp_pipe_sample_count" "$LATENCY_ROBUST_MAX_EXCEED_PCT" || true)"
      if ! is_uint "$disp_pipe_sample_count" || ! is_uint "$disp_pipe_over_limit_count"; then
        disp_pipe_robust_available=0
        disp_pipe_robust_reason="dispPipeMaxUs robust gate unavailable (samples=${disp_pipe_sample_count:-n/a} overLimit=${disp_pipe_over_limit_count:-n/a})."
      elif [[ "$disp_pipe_sample_count" -lt "$LATENCY_ROBUST_MIN_SAMPLES" ]]; then
        disp_pipe_robust_available=0
        disp_pipe_robust_reason="dispPipeMaxUs robust gate unavailable (samples=${disp_pipe_sample_count} below min ${LATENCY_ROBUST_MIN_SAMPLES})."
      elif ! is_uint "$disp_pipe_robust_allowed_over_limit"; then
        disp_pipe_robust_available=0
        disp_pipe_robust_reason="dispPipeMaxUs robust gate unavailable (could not compute allowed over-limit count)."
      elif [[ "$disp_pipe_over_limit_count" -gt "$disp_pipe_robust_allowed_over_limit" ]]; then
        disp_pipe_robust_fail=1
        disp_pipe_robust_reason="dispPipeMaxUs robust over-limit ${disp_pipe_over_limit_count}/${disp_pipe_sample_count} (allowed ${disp_pipe_robust_allowed_over_limit}, p95=${disp_pipe_p95:-n/a}) above max ${MAX_DISP_PIPE_MAX_US}."
      fi

      case "$LATENCY_GATE_MODE" in
        strict)
          if [[ "$disp_pipe_strict_fail" -eq 1 ]]; then
            mark_gate_fail gate_disp_pipe_max_fail "$disp_pipe_strict_reason"
          fi
          ;;
        robust)
          if [[ "$disp_pipe_robust_available" -eq 0 ]]; then
            mark_gate_fail gate_disp_pipe_max_fail "$disp_pipe_robust_reason"
          elif [[ "$disp_pipe_robust_fail" -eq 1 ]]; then
            mark_gate_fail gate_disp_pipe_max_fail "$disp_pipe_robust_reason"
          fi
          ;;
        hybrid)
          if [[ "$disp_pipe_robust_available" -eq 1 ]]; then
            if [[ "$disp_pipe_robust_fail" -eq 1 ]]; then
              mark_gate_fail gate_disp_pipe_max_fail "$disp_pipe_robust_reason"
            elif [[ "$disp_pipe_strict_fail" -eq 1 ]]; then
              advisory_warnings+=("dispPipeMaxUs strict peak exceeded (${disp_pipe_max_peak}) but hybrid robust gate passed (over=${disp_pipe_over_limit_count}/${disp_pipe_sample_count}, allowed=${disp_pipe_robust_allowed_over_limit}, p95=${disp_pipe_p95:-n/a}).")
            fi
          elif [[ "$disp_pipe_strict_fail" -eq 1 ]]; then
            mark_gate_fail gate_disp_pipe_max_fail "${disp_pipe_strict_reason} (robust unavailable: ${disp_pipe_robust_reason})"
          fi
          ;;
      esac
    fi

    # bleMutexTimeout (zero-tolerance counter)
    if ! is_uint "$ble_mutex_timeout_delta"; then
      mark_gate_fail gate_ble_mutex_timeout_fail "bleMutexTimeout delta ${ble_mutex_timeout_delta:-n/a} above max ${MAX_BLE_MUTEX_TIMEOUT_DELTA}."
    elif [[ "$ble_mutex_timeout_delta" -gt "$MAX_BLE_MUTEX_TIMEOUT_DELTA" ]]; then
      mark_gate_fail gate_ble_mutex_timeout_fail "bleMutexTimeout delta ${ble_mutex_timeout_delta} above max ${MAX_BLE_MUTEX_TIMEOUT_DELTA}."
    fi

    # Transition churn/recovery gates (Cycle 3)
    if [[ "$TRANSITION_DRIVE_ENABLED" -eq 0 ]]; then
      if ! is_uint "$wifi_ap_down_transitions_delta"; then
        mark_gate_fail gate_transition_churn_ap_fail "AP down transition delta ${wifi_ap_down_transitions_delta:-n/a} unavailable (max ${MAX_AP_TRANSITION_CHURN_DELTA})."
      elif [[ "$wifi_ap_down_transitions_delta" -gt "$MAX_AP_TRANSITION_CHURN_DELTA" ]]; then
        mark_gate_fail gate_transition_churn_ap_fail "AP down transition delta ${wifi_ap_down_transitions_delta} above steady-state max ${MAX_AP_TRANSITION_CHURN_DELTA}."
      fi

      if ! is_uint "$proxy_adv_off_transitions_delta" || ! is_uint "$proxy_adv_on_transitions_delta"; then
        mark_gate_fail gate_transition_churn_proxy_fail "Proxy advertising transition deltas on/off ${proxy_adv_on_transitions_delta:-n/a}/${proxy_adv_off_transitions_delta:-n/a} unavailable (max ${MAX_PROXY_ADV_TRANSITION_CHURN_DELTA})."
      elif [[ "$proxy_adv_off_transitions_delta" -gt "$MAX_PROXY_ADV_TRANSITION_CHURN_DELTA" || "$proxy_adv_on_transitions_delta" -gt "$MAX_PROXY_ADV_TRANSITION_CHURN_DELTA" ]]; then
        mark_gate_fail gate_transition_churn_proxy_fail "Proxy advertising transition deltas on/off ${proxy_adv_on_transitions_delta}/${proxy_adv_off_transitions_delta} above steady-state max ${MAX_PROXY_ADV_TRANSITION_CHURN_DELTA}."
      fi
    else
      if [[ "$transition_drive_errors" -gt 0 ]]; then
        mark_gate_fail gate_transition_drive_fail "Transition drive reported ${transition_drive_errors} failed action(s)."
      fi
      if [[ "$TRANSITION_FLAP_CYCLES" -gt 0 && "$transition_flap_cycles_completed" -lt "$TRANSITION_FLAP_CYCLES" ]]; then
        mark_gate_fail gate_transition_drive_fail "Transition drive completed ${transition_flap_cycles_completed}/${TRANSITION_FLAP_CYCLES} flap cycle(s)."
      fi

      if [[ "$MIN_AP_DOWN_TRANSITIONS" -gt 0 ]]; then
        if ! is_uint "$wifi_ap_down_transitions_delta"; then
          mark_gate_fail gate_transition_min_events_fail "AP down transitions delta unavailable (required >= ${MIN_AP_DOWN_TRANSITIONS})."
        elif [[ "$wifi_ap_down_transitions_delta" -lt "$MIN_AP_DOWN_TRANSITIONS" ]]; then
          mark_gate_fail gate_transition_min_events_fail "AP down transitions delta ${wifi_ap_down_transitions_delta} below required ${MIN_AP_DOWN_TRANSITIONS}."
        fi
      fi
      if [[ "$MIN_PROXY_ADV_OFF_TRANSITIONS" -gt 0 ]]; then
        if ! is_uint "$proxy_adv_off_transitions_delta"; then
          mark_gate_fail gate_transition_min_events_fail "Proxy advertising off transitions delta unavailable (required >= ${MIN_PROXY_ADV_OFF_TRANSITIONS})."
        elif [[ "$proxy_adv_off_transitions_delta" -lt "$MIN_PROXY_ADV_OFF_TRANSITIONS" ]]; then
          mark_gate_fail gate_transition_min_events_fail "Proxy advertising off transitions delta ${proxy_adv_off_transitions_delta} below required ${MIN_PROXY_ADV_OFF_TRANSITIONS}."
        fi
      fi

      if is_uint "$wifi_ap_down_events_unstable" && [[ "$wifi_ap_down_events_unstable" -gt 0 ]]; then
        mark_gate_fail gate_transition_recovery_ap_fail "AP down transitions had ${wifi_ap_down_events_unstable} unstable recovery event(s)."
      fi
      if is_uint "$proxy_adv_off_events_unstable" && [[ "$proxy_adv_off_events_unstable" -gt 0 ]]; then
        proxy_unstable_allowed=0
        if is_uint "$proxy_adv_off_events_observed" && is_uint "$transition_flap_off_calls" && [[ "$proxy_adv_off_events_observed" -ge "$transition_flap_off_calls" ]]; then
          extra_proxy_off_events=$((proxy_adv_off_events_observed - transition_flap_off_calls))
          if [[ "$proxy_adv_off_events_unstable" -le "$extra_proxy_off_events" ]]; then
            proxy_unstable_allowed=1
            advisory_warnings+=("[ADVISORY] proxy-off unstable events=${proxy_adv_off_events_unstable} within uncontrolled off-transition budget (observed=${proxy_adv_off_events_observed}, drive_off_calls=${transition_flap_off_calls}).")
          fi
        fi
        if [[ "$proxy_unstable_allowed" -ne 1 ]]; then
          mark_gate_fail gate_transition_recovery_proxy_fail "Proxy advertising off transitions had ${proxy_adv_off_events_unstable} unstable recovery event(s)."
        fi
      fi

      if [[ "$MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN" -gt 0 ]]; then
        if is_uint "$wifi_ap_down_events_observed" && [[ "$wifi_ap_down_events_observed" -gt 0 ]]; then
          if ! is_uint "$time_to_stable_ms_after_ap_down"; then
            mark_gate_fail gate_transition_recovery_ap_fail "AP down recovery time unavailable (max ${MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN}ms)."
          elif [[ "$time_to_stable_ms_after_ap_down" -gt "$MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN" ]]; then
            mark_gate_fail gate_transition_recovery_ap_fail "AP down recovery time ${time_to_stable_ms_after_ap_down}ms above max ${MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN}ms."
          fi
        fi
      fi
      if [[ "$MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF" -gt 0 ]]; then
        if is_uint "$proxy_adv_off_events_observed" && [[ "$proxy_adv_off_events_observed" -gt 0 ]]; then
          if ! is_uint "$time_to_stable_ms_after_proxy_adv_off"; then
            mark_gate_fail gate_transition_recovery_proxy_fail "Proxy advertising off recovery time unavailable (max ${MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF}ms)."
          elif [[ "$time_to_stable_ms_after_proxy_adv_off" -gt "$MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF" ]]; then
            mark_gate_fail gate_transition_recovery_proxy_fail "Proxy advertising off recovery time ${time_to_stable_ms_after_proxy_adv_off}ms above max ${MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF}ms."
          fi
        fi
      fi

      if [[ "$MAX_SAMPLES_TO_STABLE" -gt 0 ]]; then
        if is_uint "$wifi_ap_down_events_observed" && [[ "$wifi_ap_down_events_observed" -gt 0 ]]; then
          if ! is_uint "$samples_to_stable_after_ap_down"; then
            mark_gate_fail gate_transition_samples_fail "AP down samples-to-stable unavailable (max ${MAX_SAMPLES_TO_STABLE})."
          elif [[ "$samples_to_stable_after_ap_down" -gt "$MAX_SAMPLES_TO_STABLE" ]]; then
            mark_gate_fail gate_transition_samples_fail "AP down samples-to-stable ${samples_to_stable_after_ap_down} above max ${MAX_SAMPLES_TO_STABLE}."
          fi
        fi
        if is_uint "$proxy_adv_off_events_observed" && [[ "$proxy_adv_off_events_observed" -gt 0 ]]; then
          if ! is_uint "$samples_to_stable_after_proxy_adv_off"; then
            mark_gate_fail gate_transition_samples_fail "Proxy advertising off samples-to-stable unavailable (max ${MAX_SAMPLES_TO_STABLE})."
          elif [[ "$samples_to_stable_after_proxy_adv_off" -gt "$MAX_SAMPLES_TO_STABLE" ]]; then
            mark_gate_fail gate_transition_samples_fail "Proxy advertising off samples-to-stable ${samples_to_stable_after_proxy_adv_off} above max ${MAX_SAMPLES_TO_STABLE}."
          fi
        fi
      fi
    fi

    # ----- Advisory SLOs (warn-only, do not cause FAIL) -----
    if is_uint "$display_updates_delta" && is_uint "$display_skips_delta"; then
      display_total=$((display_updates_delta + display_skips_delta))
      if [[ "$display_total" -gt 0 ]]; then
        display_skip_pct=$((display_skips_delta * 100 / display_total))
        if [[ "$display_skip_pct" -gt 20 ]]; then
          advisory_warnings+=("displaySkipPct=${display_skip_pct}% exceeds 20% advisory limit.")
        fi
      fi
      if [[ "$soak_elapsed_s" -gt 0 ]]; then
        display_skips_per_min=$((display_skips_delta * 60 / soak_elapsed_s))
        if [[ "$display_skips_per_min" -gt 120 ]]; then
          advisory_warnings+=("displaySkipsPerMin=${display_skips_per_min} exceeds 120 advisory limit.")
        fi
      fi
    fi
    if is_uint "$reconnects_delta" && [[ "$reconnects_delta" -gt 2 ]]; then
      advisory_warnings+=("reconnects delta=${reconnects_delta} exceeds advisory limit of 2.")
    fi
    if is_uint "$disconnects_delta" && [[ "$disconnects_delta" -gt 2 ]]; then
      advisory_warnings+=("disconnects delta=${disconnects_delta} exceeds advisory limit of 2.")
    fi
    fi
  fi
else
  advisory_warnings+=("Runtime SLO gates skipped due to reboot/crash evidence (${reboot_evidence_detail}). Resolve reboot cause first.")
fi

if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]]; then
  if [[ "$display_drive_calls" -eq 0 ]]; then
    mark_gate_fail gate_display_drive_fail "Display drive produced zero calls."
  fi
  if ! is_int "$display_updates_delta"; then
    mark_gate_fail gate_display_drive_fail "Display updates delta ${display_updates_delta:-n/a} below required ${DISPLAY_MIN_UPDATES_DELTA}."
  elif [[ "$display_updates_delta" -lt "$DISPLAY_MIN_UPDATES_DELTA" ]]; then
    mark_gate_fail gate_display_drive_fail "Display updates delta ${display_updates_delta} below required ${DISPLAY_MIN_UPDATES_DELTA}."
  fi
fi

if [[ ${#fail_reasons[@]} -gt 0 ]]; then
  result="FAIL"
elif [[ "$signal_sources" -eq 0 ]]; then
  result="INCONCLUSIVE"
fi

diagnosis_bucket="No issues"
diagnosis_next_action="No action required."
diagnosis_baseline_note=""

if [[ "$BASELINE_GATES_APPLIED" -eq 1 && ( "$BASELINE_PROFILE" != "$SOAK_PROFILE" || "$BASELINE_STRESS_CLASS" != "$RUN_STRESS_CLASS" ) ]]; then
  diagnosis_baseline_note="Baseline gates came from perf CSV and this run used active stress drivers; compare against a stress baseline for release gating."
fi

if [[ "$result" == "FAIL" ]]; then
  if [[ "$reboot_evidence_detected" -eq 1 || "$gate_serial_reset_fail" -eq 1 || "$gate_serial_panic_fail" -eq 1 || "$gate_panic_endpoint_fail" -eq 1 ]]; then
    diagnosis_bucket="Device Reboot/Crash During Soak"
    diagnosis_next_action="Reboot evidence detected (${reboot_evidence_detail}). Treat latency counters as secondary and inspect serial log + panic endpoint reset reason first."
  elif [[ "$gate_metrics_window_fail" -eq 1 && "$serial_log_bytes" -eq 0 ]]; then
    diagnosis_bucket="Connectivity/Telemetry Outage"
    diagnosis_next_action="Verify WiFi/API reachability and USB serial capture first, then rerun before evaluating firmware."
  elif [[ "$gate_serial_monitor_fail" -eq 1 && "$serial_log_bytes" -eq 0 ]]; then
    diagnosis_bucket="USB Serial Capture Failure"
    diagnosis_next_action="Stabilize /dev/cu.usbmodem capture (close other monitors/cables), rerun soak."
  elif [[ "$gate_parse_fail_fail" -eq 1 || "$gate_queue_drop_fail" -eq 1 || "$gate_perf_drop_fail" -eq 1 || "$gate_event_drop_fail" -eq 1 || "$gate_oversize_drop_fail" -eq 1 ]]; then
    diagnosis_bucket="Core Data-Path Integrity Regression"
    diagnosis_next_action="Inspect parser/queue/event counters around failing timestamps and bisect recent BLE/parser changes."
  elif [[ "$gate_queue_high_water_fail" -eq 1 || "$gate_wifi_connect_deferred_fail" -eq 1 || "$gate_dma_free_fail" -eq 1 || "$gate_dma_largest_fail" -eq 1 ]]; then
    diagnosis_bucket="Resource Pressure Regression"
    diagnosis_next_action="Inspect queueHighWater/wifiConnectDeferred and DMA triage stats (below-floor count/streak + largest/free p05/p50) to separate sustained fragmentation from single-sample dips."
  elif [[ "$gate_transition_churn_ap_fail" -eq 1 || "$gate_transition_churn_proxy_fail" -eq 1 || "$gate_transition_min_events_fail" -eq 1 || "$gate_transition_recovery_ap_fail" -eq 1 || "$gate_transition_recovery_proxy_fail" -eq 1 || "$gate_transition_samples_fail" -eq 1 || "$gate_transition_drive_fail" -eq 1 ]]; then
    diagnosis_bucket="Transition Recovery Regression"
    diagnosis_next_action="Inspect transition flap/recovery windows and reason codes; reduce churn or shorten convergence after AP/proxy-down transitions."
  elif [[ "$gate_loop_fail" -eq 1 || "$gate_wifi_fail" -eq 1 || "$gate_flush_fail" -eq 1 || "$gate_ble_drain_fail" -eq 1 || "$gate_sd_max_fail" -eq 1 || "$gate_fs_max_fail" -eq 1 ]]; then
    if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]]; then
      diagnosis_bucket="Stress Latency Regression"
      diagnosis_next_action="Run paired tests: core (no stress drivers) and stress (drivers on). Optimize only the delta introduced by stress paths."
    else
      diagnosis_bucket="Core Latency Regression"
      diagnosis_next_action="Profile loop/wifi/flush peaks at reported timestamps and reduce blocking work on the main loop."
    fi
  elif [[ "$gate_display_drive_fail" -eq 1 ]]; then
    diagnosis_bucket="Stress Endpoint Saturation"
    diagnosis_next_action="Stabilize preview/demo endpoint handling under load (timeouts/retries) before treating as firmware regression."
  else
    diagnosis_bucket="Mixed Failure"
    diagnosis_next_action="Inspect failing checks and peak sample context below, then rerun with one stress driver at a time."
  fi
elif [[ "$result" == "INCONCLUSIVE" ]]; then
  diagnosis_bucket="Inconclusive Telemetry"
  diagnosis_next_action="Run again with metrics enabled and reachable; current run lacked enough signal to score."
fi

{
  echo "# Real Firmware Soak Summary"
  echo ""
  echo "- Result: **$result**"
  echo "- Firmware env: \`$ENV_NAME\`"
  echo "- Profile: \`${SOAK_PROFILE:-none}\`"
  echo "- Run stress class: \`${RUN_STRESS_CLASS}\`"
  echo "- Port: \`$MONITOR_PORT\`"
  echo "- Soak start (UTC): $soak_start_utc"
  echo "- Soak end (UTC): $soak_end_utc"
  echo "- Soak duration (s): $soak_elapsed_s"
  echo "- Serial capture died early: $monitor_died_early"
  echo "- Data signal sources captured: $signal_sources (serial/api-metrics/api-panic)"
  echo "- Serial log bytes: $serial_log_bytes"
  echo ""
  echo "## Serial Health"
  echo ""
  echo "- Reset line count (\`rst:0x\`): $serial_reset_count"
  echo "- WDT/panic signature count: $serial_wdt_or_panic_count"
  echo "- Guru Meditation count: $serial_guru_count"
  echo "- Reboot evidence detected: $([[ "$reboot_evidence_detected" -eq 1 ]] && echo "yes" || echo "no")"
  echo "- Reboot evidence detail: ${reboot_evidence_detail}"
  echo ""
  echo "## Actionable Diagnosis"
  echo ""
  echo "- Primary bucket: ${diagnosis_bucket}"
  echo "- Suggested next action: ${diagnosis_next_action}"
  if [[ -n "$diagnosis_baseline_note" ]]; then
    echo "- Baseline note: ${diagnosis_baseline_note}"
  fi
  echo "- Failing checks:"
  if [[ ${#fail_reasons[@]} -gt 0 ]]; then
    for reason in "${fail_reasons[@]}"; do
      echo "  - ${reason}"
    done
  else
    echo "  - none"
  fi
  echo "- Peak sample context (UTC):"
  echo "  - loop peak: ts=${loop_peak_ts:-n/a} loop=${loop_max_peak:-n/a} wifi=${loop_peak_wifi:-n/a} flush=${loop_peak_flush:-n/a} bleDrain=${loop_peak_ble_drain:-n/a} displayUpdates=${loop_peak_display_updates:-n/a} rxPackets=${loop_peak_rx_packets:-n/a}"
  echo "  - wifi peak: ts=${wifi_peak_ts:-n/a} wifi=${wifi_max_peak:-n/a} loop=${wifi_peak_loop:-n/a} flush=${wifi_peak_flush:-n/a} bleDrain=${wifi_peak_ble_drain:-n/a} displayUpdates=${wifi_peak_display_updates:-n/a} rxPackets=${wifi_peak_rx_packets:-n/a}"
  echo "  - flush peak: ts=${flush_peak_ts:-n/a} flush=${flush_max_peak:-n/a}"
  echo "  - bleDrain peak: ts=${ble_drain_peak_ts:-n/a} bleDrain=${ble_drain_max_peak:-n/a}"
  echo ""
  echo "## Debug API Metrics"
  echo ""
  echo "- Metrics polling configured: $([[ -n "$METRICS_URL" ]] && echo "yes" || echo "no")"
  echo "- Metrics URL: ${METRICS_URL:-disabled}"
  echo "- Metrics poll URL: ${METRICS_POLL_URL:-disabled}"
  echo "- Metrics soak mode: ${METRICS_SOAK_MODE}"
  echo "- Metrics reset URL: ${METRICS_RESET_URL:-disabled}"
  echo "- Metrics pre-reset attempted: $([[ "$metrics_reset_attempted" -eq 1 ]] && echo "yes" || echo "no")"
  echo "- Metrics pre-reset success: $([[ "$metrics_reset_success" -eq 1 ]] && echo "yes" || echo "no")"
  echo "- Metrics pre-reset HTTP code: ${metrics_reset_http_code:-n/a}"
  echo "- Metrics pre-reset reason: ${metrics_reset_reason}"
  echo "- Metrics recovery budget/elapsed: ${METRICS_RECOVERY_BUDGET_SECONDS:-0}s / ${METRICS_RECOVERY_ELAPSED_SECONDS:-0}s"
  echo "- Metrics recovery attempts: ${METRICS_RECOVERY_ATTEMPTS:-0}"
  echo "- Metrics recovery URL: ${METRICS_RECOVERY_URL:-disabled}"
  echo "- Metrics recovery status: ${METRICS_RECOVERY_REASON:-n/a}"
  echo "- Metrics samples (shell): $metrics_samples"
  echo "- Metrics successful (shell): $metrics_ok_samples"
  echo "- Metrics samples parsed: ${metrics_samples_parsed:-0}"
  echo "- Metrics successful parsed: ${metrics_ok_samples_parsed:-0}"
  echo "- Startup settle applied/succeeded: $([[ "$STARTUP_SETTLE_APPLIED" -eq 1 ]] && echo "yes" || echo "no") / $([[ "$STARTUP_SETTLE_SUCCEEDED" -eq 1 ]] && echo "yes" || echo "no")"
  echo "- Startup settle attempts/elapsed: ${STARTUP_SETTLE_ATTEMPTS_USED:-0} / ${STARTUP_SETTLE_ELAPSED_SECONDS:-0}s"
  echo "- Startup settle last sample uptime/loop/disp/wifi: ${STARTUP_SETTLE_LAST_UPTIME_MS:-n/a} / ${STARTUP_SETTLE_LAST_LOOP_MAX_US:-n/a} / ${STARTUP_SETTLE_LAST_DISP_PIPE_MAX_US:-n/a} / ${STARTUP_SETTLE_LAST_WIFI_MAX_US:-n/a}"
  echo "- Metrics required minimum parsed successes: ${MIN_METRICS_OK_SAMPLES}"
  echo "- rxPackets delta: ${rx_packets_delta:-n/a} (min ${MIN_RX_PACKETS_DELTA})"
  echo "- parseSuccesses delta: ${parse_successes_delta:-n/a} (min ${MIN_PARSE_SUCCESSES_DELTA})"
  echo "- parseFailures delta: ${parse_failures_delta:-n/a} (max ${MAX_PARSE_FAILURES_DELTA})"
  echo "- queueDrops delta: ${queue_drops_delta:-n/a} (max ${MAX_QUEUE_DROPS_DELTA})"
  echo "- perfDrop delta: ${perf_drop_delta:-n/a} (max ${MAX_PERF_DROPS_DELTA})"
  echo "- Min heapFree: ${heap_free_min:-n/a}"
  echo "- Min heapMinFree: ${heap_min_free_min:-n/a}"
  echo "- Min heapDma: ${heap_dma_min:-n/a}"
  echo "- Min heapDmaLargest: ${heap_dma_largest_min:-n/a}"
  echo "- Peak latencyMaxUs: ${latency_max_peak:-n/a}"
  echo "- Event publish delta: ${event_publish_delta:-n/a}"
  echo "- Event drop delta: ${event_drop_delta:-n/a} (max ${MAX_EVENT_DROPS_DELTA})"
  echo "- Event size peak: ${event_size_peak:-n/a}"
  echo "- Display updates first/last/delta: ${display_updates_first:-n/a} / ${display_updates_last:-n/a} / ${display_updates_delta:-n/a}"
  echo "- Display skips first/last/delta: ${display_skips_first:-n/a} / ${display_skips_last:-n/a} / ${display_skips_delta:-n/a}"
  echo "- Peak flushMaxUs: ${flush_max_peak:-n/a} (max gate ${MAX_FLUSH_MAX_US})"
  echo "- Peak loopMaxUs: ${loop_max_peak:-n/a} (max gate ${MAX_LOOP_MAX_US})"
  echo "- Latency gate mode: ${LATENCY_GATE_MODE} (robust minSamples=${LATENCY_ROBUST_MIN_SAMPLES} maxExceedPct=${LATENCY_ROBUST_MAX_EXCEED_PCT} wifiSkipFirst=${WIFI_ROBUST_SKIP_FIRST_SAMPLES})"
  echo "- Peak wifiMaxUs (raw): ${wifi_max_peak:-n/a} (max gate ${MAX_WIFI_MAX_US})"
  echo "- Peak wifiMaxUs (excluding first sample): ${wifi_max_peak_excluding_first:-n/a} (ts ${wifi_peak_excluding_first_ts:-n/a})"
  echo "- Peak wifiMaxUs used for gate: ${wifi_peak_gate_value:-n/a} (${wifi_peak_gate_basis})"
  echo "- Robust wifi samples raw/excluding-first: ${wifi_sample_count:-n/a} / ${wifi_sample_count_excluding_first:-n/a}"
  echo "- Robust wifi p95 raw/excluding-first: ${wifi_p95_raw:-n/a} / ${wifi_p95_excluding_first:-n/a}"
  echo "- Robust wifi over-limit raw/excluding-first: ${wifi_over_limit_count_raw:-n/a} / ${wifi_over_limit_count_excluding_first:-n/a} (allowed ${wifi_robust_allowed_over_limit:-n/a}, basis ${wifi_robust_basis:-n/a})"
  echo "- AP transition deltas up/down: ${wifi_ap_up_transitions_delta:-n/a} / ${wifi_ap_down_transitions_delta:-n/a}"
  echo "- AP state sample split up/down: ${wifi_ap_active_samples:-n/a} / ${wifi_ap_inactive_samples:-n/a}"
  echo "- AP-segment wifi peaks up/down: ${wifi_peak_ap_active:-n/a} / ${wifi_peak_ap_inactive:-n/a}"
  echo "- AP last transition reason code/name: ${wifi_ap_last_reason_code:-n/a} / ${wifi_ap_last_reason:-n/a}"
  echo "- Proxy advertising transition deltas on/off: ${proxy_adv_on_transitions_delta:-n/a} / ${proxy_adv_off_transitions_delta:-n/a}"
  echo "- Proxy advertising sample split on/off: ${proxy_adv_on_samples:-n/a} / ${proxy_adv_off_samples:-n/a}"
  echo "- Proxy-segment wifi peaks on/off: ${wifi_peak_proxy_adv_on:-n/a} / ${wifi_peak_proxy_adv_off:-n/a}"
  echo "- Proxy advertising last transition reason code/name: ${proxy_adv_last_reason_code:-n/a} / ${proxy_adv_last_reason:-n/a}"
  echo "- Transition stability consecutive samples required: ${stable_consecutive_samples_required:-n/a}"
  echo "- Transition primary source/event/stable-index: ${transition_primary_source:-n/a} / ${transition_primary_event_index:-n/a} / ${transition_primary_stable_index:-n/a}"
  echo "- Transition primary samples/time-to-stable: ${samples_to_stable:-n/a} / ${time_to_stable_ms:-n/a}ms"
  echo "- AP down transition events observed/stabilized/unstable: ${wifi_ap_down_events_observed:-n/a} / ${wifi_ap_down_events_stabilized:-n/a} / ${wifi_ap_down_events_unstable:-n/a}"
  echo "- AP down samples/time-to-stable: ${samples_to_stable_after_ap_down:-n/a} / ${time_to_stable_ms_after_ap_down:-n/a}ms (max ${MAX_SAMPLES_TO_STABLE}, ${MAX_TIME_TO_STABLE_MS_AFTER_AP_DOWN}ms)"
  echo "- Proxy off transition events observed/stabilized/unstable: ${proxy_adv_off_events_observed:-n/a} / ${proxy_adv_off_events_stabilized:-n/a} / ${proxy_adv_off_events_unstable:-n/a}"
  echo "- Proxy off samples/time-to-stable: ${samples_to_stable_after_proxy_adv_off:-n/a} / ${time_to_stable_ms_after_proxy_adv_off:-n/a}ms (max ${MAX_SAMPLES_TO_STABLE}, ${MAX_TIME_TO_STABLE_MS_AFTER_PROXY_ADV_OFF}ms)"
  echo "- Window pre samples wifiPeak/wifiP95 loopPeak/loopP95 dispPeak/dispP95: ${window_pre_samples:-n/a} ${window_pre_wifi_peak:-n/a}/${window_pre_wifi_p95:-n/a} ${window_pre_loop_peak:-n/a}/${window_pre_loop_p95:-n/a} ${window_pre_disp_pipe_peak:-n/a}/${window_pre_disp_pipe_p95:-n/a}"
  echo "- Window transition samples wifiPeak/wifiP95 loopPeak/loopP95 dispPeak/dispP95: ${window_transition_samples:-n/a} ${window_transition_wifi_peak:-n/a}/${window_transition_wifi_p95:-n/a} ${window_transition_loop_peak:-n/a}/${window_transition_loop_p95:-n/a} ${window_transition_disp_pipe_peak:-n/a}/${window_transition_disp_pipe_p95:-n/a}"
  echo "- Window post-stable samples wifiPeak/wifiP95 loopPeak/loopP95 dispPeak/dispP95: ${window_post_stable_samples:-n/a} ${window_post_stable_wifi_peak:-n/a}/${window_post_stable_wifi_p95:-n/a} ${window_post_stable_loop_peak:-n/a}/${window_post_stable_loop_p95:-n/a} ${window_post_stable_disp_pipe_peak:-n/a}/${window_post_stable_disp_pipe_p95:-n/a}"
  echo "- Connect-burst diagnostics (diagnostic-only): detected=${connect_burst_detected:-n/a} stabilized=${connect_burst_stabilized:-n/a} consecutiveSamples=${connect_burst_stable_consecutive_samples_required:-n/a}"
  echo "- Connect-burst event sample/state/step/proxy: ${connect_burst_event_index:-n/a} / ${connect_burst_event_ble_state:-n/a} / ${connect_burst_event_subscribe_step:-n/a} / ${connect_burst_event_proxy_advertising:-n/a}"
  echo "- Connect-burst stable sample/samples/time: ${connect_burst_stable_index:-n/a} / ${connect_burst_samples_to_stable:-n/a} / ${connect_burst_time_to_stable_ms:-n/a}ms"
  echo "- Connect-burst pre-window bleProcess/dispPipe peaks: ${connect_burst_pre_ble_process_peak:-n/a} / ${connect_burst_pre_disp_pipe_peak:-n/a}"
  echo "- Connect-burst BLE root-cause peaks alert/version/stableCallback/proxyStart: ${connect_burst_ble_followup_request_alert_peak:-n/a} / ${connect_burst_ble_followup_request_version_peak:-n/a} / ${connect_burst_ble_connect_stable_callback_peak:-n/a} / ${connect_burst_ble_proxy_start_peak:-n/a}"
  echo "- Connect-burst display root-cause peaks render/voice/gapRecover: ${connect_burst_disp_render_peak:-n/a} / ${connect_burst_display_voice_peak:-n/a} / ${connect_burst_display_gap_recover_peak:-n/a}"
  echo "- Connect-burst display subphase peaks base/status/freq/bands/icons/cards/flush: ${connect_burst_display_base_frame_peak:-n/a} / ${connect_burst_display_status_strip_peak:-n/a} / ${connect_burst_display_frequency_peak:-n/a} / ${connect_burst_display_bands_bars_peak:-n/a} / ${connect_burst_display_arrows_icons_peak:-n/a} / ${connect_burst_display_cards_peak:-n/a} / ${connect_burst_display_flush_subphase_peak:-n/a}"
  echo "- Display render path deltas full/incremental/cards/restingFull/restingIncremental/persisted/preview/restore: ${display_full_render_count_delta:-n/a} / ${display_incremental_render_count_delta:-n/a} / ${display_cards_only_render_count_delta:-n/a} / ${display_resting_full_render_count_delta:-n/a} / ${display_resting_incremental_render_count_delta:-n/a} / ${display_persisted_render_count_delta:-n/a} / ${display_preview_render_count_delta:-n/a} / ${display_restore_render_count_delta:-n/a}"
  echo "- Display scenario render deltas live/resting/persisted/preview/restore: ${display_live_scenario_render_count_delta:-n/a} / ${display_resting_scenario_render_count_delta:-n/a} / ${display_persisted_scenario_render_count_delta:-n/a} / ${display_preview_scenario_render_count_delta:-n/a} / ${display_restore_scenario_render_count_delta:-n/a}"
  echo "- Display redraw reason deltas first/enterLive/leaveLive/leavePersisted/force: ${display_redraw_reason_first_run_count_delta:-n/a} / ${display_redraw_reason_enter_live_count_delta:-n/a} / ${display_redraw_reason_leave_live_count_delta:-n/a} / ${display_redraw_reason_leave_persisted_count_delta:-n/a} / ${display_redraw_reason_force_redraw_count_delta:-n/a}"
  echo "- Display redraw reason deltas freq/bands/arrow/signal/volume/bogey/rssi/flash: ${display_redraw_reason_frequency_change_count_delta:-n/a} / ${display_redraw_reason_band_set_change_count_delta:-n/a} / ${display_redraw_reason_arrow_change_count_delta:-n/a} / ${display_redraw_reason_signal_bar_change_count_delta:-n/a} / ${display_redraw_reason_volume_change_count_delta:-n/a} / ${display_redraw_reason_bogey_counter_change_count_delta:-n/a} / ${display_redraw_reason_rssi_refresh_count_delta:-n/a} / ${display_redraw_reason_flash_tick_count_delta:-n/a}"
  echo "- Display flush counters/areas full/partial/partialPeak/partialTotal/equivalent/maxArea: ${display_full_flush_count_delta:-n/a} / ${display_partial_flush_count_delta:-n/a} / ${display_partial_flush_area_peak_px:-n/a} / ${display_partial_flush_area_total_px_delta:-n/a} / ${display_flush_equivalent_area_total_px_delta:-n/a} / ${display_flush_max_area_px:-n/a}"
  echo "- Display subphase peaks base/status/freq/bands/icons/cards/flush: ${display_base_frame_peak:-n/a} / ${display_status_strip_peak:-n/a} / ${display_frequency_peak:-n/a} / ${display_bands_bars_peak:-n/a} / ${display_arrows_icons_peak:-n/a} / ${display_cards_peak:-n/a} / ${display_flush_subphase_peak:-n/a}"
  echo "- Display scenario render peaks live/resting/persisted/preview/restore/previewFirst/previewSteady: ${display_live_render_peak:-n/a} / ${display_resting_render_peak:-n/a} / ${display_persisted_render_peak:-n/a} / ${display_preview_render_peak:-n/a} / ${display_restore_render_peak:-n/a} / ${display_preview_first_render_peak:-n/a} / ${display_preview_steady_render_peak:-n/a}"
  echo "- Transition churn gates (steady-state): AP down <= ${MAX_AP_TRANSITION_CHURN_DELTA}, proxy on/off <= ${MAX_PROXY_ADV_TRANSITION_CHURN_DELTA}"
  echo "- Transition minimum events gates (active drive): AP down >= ${MIN_AP_DOWN_TRANSITIONS}, proxy off >= ${MIN_PROXY_ADV_OFF_TRANSITIONS}"
  echo "- Peak bleDrainMaxUs: ${ble_drain_max_peak:-n/a} (max gate ${MAX_BLE_DRAIN_MAX_US})"
  echo "- Peak sdMaxUs: ${sd_max_peak:-n/a} (max gate ${MAX_SD_MAX_US})"
  echo "- Peak fsMaxUs: ${fs_max_peak:-n/a} (max gate ${MAX_FS_MAX_US})"
  echo "- Peak bleProcessMaxUs: ${ble_process_max_peak:-n/a} (max gate ${MAX_BLE_PROCESS_MAX_US})"
  echo "- Peak dispPipeMaxUs: ${disp_pipe_max_peak:-n/a} (max gate ${MAX_DISP_PIPE_MAX_US})"
  echo "- Robust dispPipe samples/p95/over-limit: ${disp_pipe_sample_count:-n/a} / ${disp_pipe_p95:-n/a} / ${disp_pipe_over_limit_count:-n/a} (allowed ${disp_pipe_robust_allowed_over_limit:-n/a})"
  echo "- oversizeDrops delta: ${oversize_drops_delta:-n/a} (max ${MAX_OVERSIZE_DROPS_DELTA})"
  echo "- queueHighWater first/peak: ${queue_high_water_first:-n/a} / ${queue_high_water_peak:-n/a} (max ${MAX_QUEUE_HIGH_WATER})"
  echo "- Inherited counter suspect: ${inherited_counter_suspect:-n/a}"
  echo "- wifiConnectDeferred delta: ${wifi_connect_deferred_delta:-n/a} (max ${MAX_WIFI_CONNECT_DEFERRED}; drive_wifi_off requires 0)"
  echo "- bleMutexTimeout delta: ${ble_mutex_timeout_delta:-n/a} (max ${MAX_BLE_MUTEX_TIMEOUT_DELTA})"
  echo "- Min heapDmaMin (SLO): ${dma_free_min_parsed:-n/a} (floor ${MIN_DMA_FREE}; raw ${dma_free_min_raw_parsed:-n/a}; tailExcluded ${minima_tail_samples_excluded:-0}; samplesUsed ${minima_samples_considered:-n/a})"
  echo "- Min heapDmaLargestMin (SLO): ${dma_largest_min_parsed:-n/a} (floor ${MIN_DMA_LARGEST}; raw ${dma_largest_min_raw_parsed:-n/a}; tailExcluded ${minima_tail_samples_excluded:-0}; samplesUsed ${minima_samples_considered:-n/a})"
  echo "- DMA largest current below-floor samples/total: ${dma_largest_below_floor_samples:-n/a}/${dma_largest_current_sample_count:-n/a} (pct ${dma_largest_below_floor_pct:-n/a}%, longest streak ${dma_largest_below_floor_longest_streak:-n/a})"
  echo "- DMA largest/free pct min/p05/p50: ${dma_largest_to_free_pct_min:-n/a} / ${dma_largest_to_free_pct_p05:-n/a} / ${dma_largest_to_free_pct_p50:-n/a}"
  echo "- DMA fragmentation pct p50/p95/max: ${dma_fragmentation_pct_p50:-n/a} / ${dma_fragmentation_pct_p95:-n/a} / ${dma_fragmentation_pct_max:-n/a}"
  echo "- reconnects delta: ${reconnects_delta:-n/a}"
  echo "- disconnects delta: ${disconnects_delta:-n/a}"
  echo "- Proxy drop peak: ${proxy_drop_peak:-n/a}"
  echo "- Core guard tripped count: ${core_guard_tripped_count:-n/a}"
  echo ""
  echo "## Advisory SLO Warnings"
  echo ""
  if [[ ${#advisory_warnings[@]} -gt 0 ]]; then
    for warn in "${advisory_warnings[@]}"; do
      echo "  - [ADVISORY] ${warn}"
    done
  else
    echo "  - none"
  fi
  echo ""
  echo "## Baseline-Derived Gates"
  echo ""
  if [[ "$BASELINE_GATES_APPLIED" -eq 1 ]]; then
    echo "- Baseline perf CSV: \`${BASELINE_PERF_CSV}\`"
    echo "- Baseline profile (declared): ${BASELINE_PROFILE}"
    echo "- Baseline stress class (declared): ${BASELINE_STRESS_CLASS}"
    echo "- Baseline comparability check: run profile=${SOAK_PROFILE}, run stress class=${RUN_STRESS_CLASS}"
    echo "- Baseline session: ${BASELINE_SELECTED_SESSION}"
    echo "- Baseline rows: ${BASELINE_SELECTED_ROWS}"
    echo "- Baseline duration (ms): ${BASELINE_SELECTED_DURATION_MS}"
    echo "- Baseline rx rate (/s): ${BASELINE_RX_RATE_PER_SEC}"
    echo "- Baseline parse rate (/s): ${BASELINE_PARSE_RATE_PER_SEC}"
    echo "- Baseline peaks (loop/flush/wifi/bleDrain): ${BASELINE_PEAK_LOOP_US} / ${BASELINE_PEAK_FLUSH_US} / ${BASELINE_PEAK_WIFI_US} / ${BASELINE_PEAK_BLE_DRAIN_US}"
    echo "- Derived min rxPackets delta: ${BASELINE_DERIVED_MIN_RX_DELTA}"
    echo "- Derived min parseSuccesses delta: ${BASELINE_DERIVED_MIN_PARSE_DELTA}"
    echo "- Derived max loopMaxUs: ${BASELINE_DERIVED_MAX_LOOP_US}"
    echo "- Derived max flushMaxUs: ${BASELINE_DERIVED_MAX_FLUSH_US}"
    echo "- Derived max wifiMaxUs: ${BASELINE_DERIVED_MAX_WIFI_US} (0 means no derived gate)"
    echo "- Derived max bleDrainMaxUs: ${BASELINE_DERIVED_MAX_BLE_DRAIN_US}"
    echo "- Baseline latency factor: ${BASELINE_LATENCY_FACTOR}"
    echo "- Baseline throughput factor: ${BASELINE_THROUGHPUT_FACTOR}"
  else
    echo "- Baseline perf CSV: disabled"
  fi
  echo ""
  echo "## Display Drive"
  echo ""
  echo "- Display drive enabled: $([[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]] && echo "yes" || echo "no")"
  echo "- Display preview URL: ${DISPLAY_PREVIEW_URL:-disabled}"
  echo "- Display clear URL: ${DISPLAY_CLEAR_URL:-disabled}"
  echo "- Display drive interval (s): ${DISPLAY_DRIVE_INTERVAL_SECONDS}"
  echo "- Display preview hold window (s): ${DISPLAY_PREVIEW_HOLD_SECONDS}"
  echo "- Display drive calls: ${display_drive_calls}"
  echo "- Display drive errors: ${display_drive_errors}"
  echo "- Display drive start misses: ${display_drive_start_misses}"
  echo "- Display drive skips while active: ${display_drive_skipped_while_active}"
  echo "- Display drive restore errors: ${display_drive_restore_errors}"
  echo "- Minimum required displayUpdates delta: ${DISPLAY_MIN_UPDATES_DELTA}"
  echo ""
  echo "## Transition Drive"
  echo ""
  echo "- Transition drive enabled: $([[ "$TRANSITION_DRIVE_ENABLED" -eq 1 ]] && echo "yes" || echo "no")"
  echo "- Transition control URL: ${TRANSITION_CONTROL_URL:-disabled}"
  echo "- Transition drive interval (s): ${TRANSITION_DRIVE_INTERVAL_SECONDS}"
  echo "- Transition flap cycles target/completed: ${TRANSITION_FLAP_CYCLES} / ${transition_flap_cycles_completed}"
  echo "- Transition flap off calls: ${transition_flap_off_calls}"
  echo "- Transition flap on calls: ${transition_flap_on_calls}"
  echo "- Transition restore calls: ${transition_flap_restore_calls}"
  echo "- Transition restore attempts used/max: ${transition_restore_attempts}/${transition_restore_max_attempts}"
  echo "- Transition drive actions: ${transition_drive_calls}"
  echo "- Transition drive errors: ${transition_drive_errors}"
  echo "- Stable consecutive samples required: ${TRANSITION_STABLE_CONSECUTIVE_SAMPLES}"
  echo ""
  echo "## Panic Endpoint"
  echo ""
  echo "- Panic URL: ${PANIC_URL:-disabled}"
  echo "- Panic poll URL: ${PANIC_POLL_URL:-disabled}"
  echo "- Panic samples parsed: ${panic_samples_parsed:-0}"
  echo "- Panic successful parsed: ${panic_ok_samples_parsed:-0}"
  echo "- Panic wasCrash=true count: ${panic_was_crash_true:-0}"
  echo "- Panic hasPanicFile=true count: ${panic_has_panic_file_true:-0}"
  echo "- Panic first wasCrash: ${panic_first_was_crash:-n/a}"
  echo "- Panic last wasCrash: ${panic_last_was_crash:-n/a}"
  echo "- Panic first hasPanicFile: ${panic_first_has_panic_file:-n/a}"
  echo "- Panic last hasPanicFile: ${panic_last_has_panic_file:-n/a}"
  echo "- Panic first reset reason: ${panic_first_reset_reason:-n/a}"
  echo "- Panic latest reset reason: ${panic_last_reset_reason:-n/a}"
  echo "- Panic state changes during soak: ${panic_state_change_count:-0}"
  echo ""
  echo "## Artifacts"
  echo ""
  echo "- Run log: \`$RUN_LOG\`"
  echo "- Serial log: \`$SERIAL_LOG\`"
  echo "- Serial capture stderr: \`$SERIAL_CAPTURE_ERR\`"
  echo "- Metrics JSONL: \`$METRICS_JSONL\`"
  echo "- Panic JSONL: \`$PANIC_JSONL\`"
  if [[ -n "$BASELINE_GATES_KV_FILE" ]]; then
    echo "- Baseline derived gates KV: \`$BASELINE_GATES_KV_FILE\`"
  fi
} > "$SUMMARY_BODY_MD"

runtime_result="$result"
track_name="${SOAK_PROFILE:-none}"
cat > "$TREND_METRICS_KV" <<EOF
metrics_ok_samples=${metrics_ok_samples_parsed}
rx_packets_delta=${rx_packets_delta}
parse_successes_delta=${parse_successes_delta}
parse_failures_delta=${parse_failures_delta}
queue_drops_delta=${queue_drops_delta}
perf_drop_delta=${perf_drop_delta}
event_drop_delta=${event_drop_delta}
oversize_drops_delta=${oversize_drops_delta}
display_updates_delta=${display_updates_delta}
display_skips_delta=${display_skips_delta}
reconnects_delta=${reconnects_delta}
disconnects_delta=${disconnects_delta}
flush_max_peak_us=${flush_max_peak}
loop_max_peak_us=${loop_max_peak}
wifi_max_peak_us=${wifi_peak_gate_value}
ble_drain_max_peak_us=${ble_drain_max_peak}
sd_max_peak_us=${sd_max_peak}
fs_max_peak_us=${fs_max_peak}
queue_high_water_peak=${queue_high_water_peak}
wifi_connect_deferred_delta=${wifi_connect_deferred_delta}
dma_free_min_bytes=${dma_free_min_parsed}
dma_largest_min_bytes=${dma_largest_min_parsed}
ble_process_max_peak_us=${ble_process_max_peak}
disp_pipe_max_peak_us=${disp_pipe_max_peak}
ble_mutex_timeout_delta=${ble_mutex_timeout_delta}
wifi_p95_us=${wifi_robust_p95}
disp_pipe_p95_us=${disp_pipe_p95}
dma_fragmentation_pct_p95=${dma_fragmentation_pct_p95}
samples_to_stable=${samples_to_stable}
time_to_stable_ms=${time_to_stable_ms}
connect_burst_samples_to_stable=${connect_burst_samples_to_stable}
connect_burst_time_to_stable_ms=${connect_burst_time_to_stable_ms}
connect_burst_pre_ble_process_peak_us=${connect_burst_pre_ble_process_peak}
connect_burst_pre_disp_pipe_peak_us=${connect_burst_pre_disp_pipe_peak}
connect_burst_ble_followup_request_alert_peak_us=${connect_burst_ble_followup_request_alert_peak}
connect_burst_ble_followup_request_version_peak_us=${connect_burst_ble_followup_request_version_peak}
connect_burst_ble_connect_stable_callback_peak_us=${connect_burst_ble_connect_stable_callback_peak}
connect_burst_ble_proxy_start_peak_us=${connect_burst_ble_proxy_start_peak}
connect_burst_disp_render_peak_us=${connect_burst_disp_render_peak}
connect_burst_display_voice_peak_us=${connect_burst_display_voice_peak}
connect_burst_display_gap_recover_peak_us=${connect_burst_display_gap_recover_peak}
connect_burst_display_base_frame_peak_us=${connect_burst_display_base_frame_peak:-}
connect_burst_display_status_strip_peak_us=${connect_burst_display_status_strip_peak:-}
connect_burst_display_frequency_peak_us=${connect_burst_display_frequency_peak:-}
connect_burst_display_bands_bars_peak_us=${connect_burst_display_bands_bars_peak:-}
connect_burst_display_arrows_icons_peak_us=${connect_burst_display_arrows_icons_peak:-}
connect_burst_display_cards_peak_us=${connect_burst_display_cards_peak:-}
connect_burst_display_flush_subphase_peak_us=${connect_burst_display_flush_subphase_peak:-}
display_full_flush_count_delta=${display_full_flush_count_delta:-}
display_partial_flush_count_delta=${display_partial_flush_count_delta:-}
display_partial_flush_area_peak_px=${display_partial_flush_area_peak_px:-}
display_flush_max_area_px=${display_flush_max_area_px:-}
display_base_frame_peak_us=${display_base_frame_peak:-}
display_status_strip_peak_us=${display_status_strip_peak:-}
display_frequency_peak_us=${display_frequency_peak:-}
display_bands_bars_peak_us=${display_bands_bars_peak:-}
display_arrows_icons_peak_us=${display_arrows_icons_peak:-}
display_cards_peak_us=${display_cards_peak:-}
display_flush_subphase_peak_us=${display_flush_subphase_peak:-}
display_live_render_peak_us=${display_live_render_peak:-}
display_resting_render_peak_us=${display_resting_render_peak:-}
display_persisted_render_peak_us=${display_persisted_render_peak:-}
display_preview_render_peak_us=${display_preview_render_peak:-}
display_restore_render_peak_us=${display_restore_render_peak:-}
display_preview_first_render_peak_us=${display_preview_first_render_peak:-}
display_preview_steady_render_peak_us=${display_preview_steady_render_peak:-}
EOF

trend_metric_count="$(python3 - "$TREND_METRICS_KV" "$TREND_METRICS_NDJSON" "$RUN_ID" "$GIT_SHA_SHORT" "$track_name" "$RUN_STRESS_CLASS" "$ROOT_DIR" <<'PY'
import json
import sys
from pathlib import Path

kv_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
run_id = sys.argv[3]
git_sha = sys.argv[4]
track_name = sys.argv[5]
stress_class = sys.argv[6]
root_dir = Path(sys.argv[7])
sys.path.insert(0, str(root_dir / "tools"))

from metric_schema import SOAK_TREND_METRIC_KV_ALIASES, SOAK_TREND_METRIC_UNITS  # type: ignore

payload = {}
with kv_path.open("r", encoding="utf-8") as handle:
    for raw in handle:
        line = raw.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        payload[key] = value

count = 0
with out_path.open("w", encoding="utf-8") as out_handle:
    for key, unit in SOAK_TREND_METRIC_UNITS.items():
        source_key = SOAK_TREND_METRIC_KV_ALIASES.get(key, key)
        value = payload.get(source_key, "")
        if value == "" and source_key != key:
            value = payload.get(key, "")
        if value == "":
            continue
        try:
            numeric = float(value)
        except ValueError:
            continue
        record = {
            "schema_version": 1,
            "run_id": run_id,
            "git_sha": git_sha,
            "run_kind": "real_fw_soak",
            "suite_or_profile": track_name,
            "metric": key,
            "sample": "value",
            "value": numeric,
            "unit": unit,
            "tags": {"stress_class": stress_class},
        }
        out_handle.write(json.dumps(record, sort_keys=True))
        out_handle.write("\n")
        count += 1

print(count)
PY
)"

python3 - "$MANIFEST_JSON" "$RUN_ID" "$GIT_SHA_SHORT" "$GIT_REF_NAME" "$BOARD_ID" "$ENV_NAME" "$track_name" "$RUN_STRESS_CLASS" "$runtime_result" "$trend_metric_count" "$METRICS_RECOVERY_BUDGET_SECONDS" "$METRICS_RECOVERY_ELAPSED_SECONDS" "$METRICS_RECOVERY_ATTEMPTS" "$METRICS_RECOVERY_URL" <<'PY'
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

manifest_path = Path(sys.argv[1])
run_id = sys.argv[2]
git_sha = sys.argv[3]
git_ref = sys.argv[4]
board_id = sys.argv[5]
env_name = sys.argv[6]
track_name = sys.argv[7]
stress_class = sys.argv[8]
base_result = sys.argv[9]
metric_count = int(sys.argv[10])
metrics_recovery_budget_seconds = int(sys.argv[11])
metrics_recovery_elapsed_seconds = float(sys.argv[12])
metrics_recovery_attempts = int(sys.argv[13])
metrics_recovery_url = sys.argv[14]

payload = {
    "schema_version": 1,
    "run_id": run_id,
    "timestamp_utc": datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
    "git_sha": git_sha,
    "git_ref": git_ref,
    "run_kind": "real_fw_soak",
    "board_id": board_id,
    "env": env_name,
    "lane": "real-fw-soak",
    "suite_or_profile": track_name,
    "stress_class": stress_class,
    "result": base_result,
    "runtime_result": base_result,
    "base_result": base_result,
    "trend_status": "pending",
    "metrics_file": "metrics.ndjson",
    "scoring_file": "scoring.json",
    "metrics_recovery_budget_seconds": metrics_recovery_budget_seconds,
    "metrics_recovery_elapsed_seconds": metrics_recovery_elapsed_seconds,
    "metrics_recovery_attempts": metrics_recovery_attempts,
    "metrics_recovery_url": metrics_recovery_url,
    "tracks": [track_name] if metric_count > 0 and track_name else [],
}
manifest_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY

trend_scorer_exit=0
trend_scoring_skipped=0
trend_error_detail=""

if [[ "$trend_metric_count" -le 0 ]]; then
  trend_scoring_skipped=1
  python3 - "$TREND_SCORING_JSON" "$runtime_result" <<'PY'
import json
import sys
from pathlib import Path

payload = {
    "schema_version": 1,
    "comparison_kind": "skipped",
    "result": sys.argv[2],
    "summary": {
        "reason": "No trend metrics were emitted for this run.",
    },
}
Path(sys.argv[1]).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY
else
  compare_args=()
  if [[ "${#COMPARE_TO_MANIFESTS[@]}" -gt 0 ]]; then
    for compare_to in "${COMPARE_TO_MANIFESTS[@]}"; do
      compare_args+=(--compare-to "$compare_to")
    done
  fi
  set +e
  python3 "$ROOT_DIR/tools/score_hardware_run.py" \
    "$MANIFEST_JSON" \
    --catalog "$ROOT_DIR/tools/hardware_metric_catalog.json" \
    "${compare_args[@]}" \
    --json > "$TREND_SCORING_JSON" 2> "$TREND_SCORING_STDERR"
  trend_scorer_exit=$?
  set -e

  if [[ "$trend_scorer_exit" -le 2 ]]; then
    set +e
    python3 "$ROOT_DIR/tools/score_hardware_run.py" \
      "$MANIFEST_JSON" \
      --catalog "$ROOT_DIR/tools/hardware_metric_catalog.json" \
      "${compare_args[@]}" > "$TREND_SUMMARY_MD" 2>> "$TREND_SCORING_STDERR"
    set -e
  fi

  if [[ "$trend_scorer_exit" -gt 2 ]]; then
    trend_error_detail="$(tr '\n' ' ' < "$TREND_SCORING_STDERR" | sed 's/[[:space:]]\+/ /g; s/^ //; s/ $//')"
    if [[ -z "$trend_error_detail" ]]; then
      trend_error_detail="Trend scorer exited ${trend_scorer_exit} before producing a usable result."
    fi
  elif [[ ! -s "$TREND_SCORING_JSON" ]]; then
    # Scorer exited <=2 but produced no output — guard against silent
    # empty-file failures that break trend comparison downstream.
    trend_error_detail="Scorer exited ${trend_scorer_exit} but scoring.json is empty (0 bytes)."
  fi
fi

finalize_args=(
  --manifest "$MANIFEST_JSON"
  --runtime-result "$runtime_result"
  --trend-scorer-exit "$trend_scorer_exit"
  --trend-scoring-json "$TREND_SCORING_JSON"
  --summary-body "$SUMMARY_BODY_MD"
  --summary-output "$SUMMARY_MD"
  --trend-summary "$TREND_SUMMARY_MD"
)
if [[ "$trend_scoring_skipped" -eq 1 ]]; then
  finalize_args+=(--trend-skipped)
fi
if [[ "$ALLOW_INCONCLUSIVE" -eq 1 ]]; then
  finalize_args+=(--allow-inconclusive)
fi
if [[ -n "$trend_error_detail" ]]; then
  finalize_args+=(--trend-error-detail "$trend_error_detail")
fi
python3 "$ROOT_DIR/tools/finalize_real_fw_soak_result.py" "${finalize_args[@]}" > "$FINAL_RESULT_JSON"

final_result_fields=()
while IFS= read -r line; do
  final_result_fields+=("$line")
done < <(read_finalize_result_fields "$FINAL_RESULT_JSON")
final_result="${final_result_fields[0]:-ERROR}"
runtime_result="${final_result_fields[1]:-$runtime_result}"
trend_status="${final_result_fields[2]:-error}"
trend_result="${final_result_fields[3]:-}"
final_exit_code="${final_result_fields[4]:-1}"
trend_error_detail="${final_result_fields[5]:-$trend_error_detail}"

echo "==> Real firmware soak complete"
echo "    result: $final_result"
echo "    summary: $SUMMARY_MD"
echo "    serial log: $SERIAL_LOG"
echo "    manifest: $MANIFEST_JSON"
exit "$final_exit_code"
