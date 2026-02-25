#!/usr/bin/env bash
#
# run_real_fw_soak.sh - Flash production firmware and soak-test the real runtime
# on hardware with serial crash detection and optional debug API polling.
#
# Usage examples:
#   ./scripts/run_real_fw_soak.sh --duration-seconds 600
#   ./scripts/run_real_fw_soak.sh --duration-seconds 1800 --metrics-url http://192.168.35.5/api/debug/metrics
#   ./scripts/run_real_fw_soak.sh --skip-flash --duration-seconds 900 --metrics-url http://192.168.35.5/api/debug/metrics --drive-display-preview
#   ./scripts/run_real_fw_soak.sh --skip-flash --duration-seconds 900 --metrics-url http://192.168.35.5/api/debug/metrics --drive-display-preview --drive-camera-demo
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
UPLOAD_FS=0
SKIP_FLASH=0
METRICS_URL="${REAL_FW_METRICS_URL:-http://192.168.35.5/api/debug/metrics}"
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
MAX_CAMERA_MAX_TICK_US=0
MAX_CAMERA_MAX_WINDOW_HZ=""
MAX_BLE_MUTEX_TIMEOUT_DELTA=0
MAX_CAMERA_BUDGET_EXCEEDED_DELTA=0
MAX_CAMERA_LOAD_FAILURES_DELTA=0
MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA=0
SOAK_PROFILE=""
LATENCY_GATE_MODE="${REAL_FW_LATENCY_GATE_MODE:-hybrid}"
LATENCY_ROBUST_MIN_SAMPLES="${REAL_FW_LATENCY_ROBUST_MIN_SAMPLES:-8}"
LATENCY_ROBUST_MAX_EXCEED_PCT="${REAL_FW_LATENCY_ROBUST_MAX_EXCEED_PCT:-5}"
WIFI_ROBUST_SKIP_FIRST_SAMPLES="${REAL_FW_WIFI_ROBUST_SKIP_FIRST_SAMPLES:-2}"
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
CLI_OVERRIDE_MAX_CAMERA_MAX_TICK_US=0
CLI_OVERRIDE_MAX_CAMERA_MAX_WINDOW_HZ=0
CLI_OVERRIDE_MAX_BLE_MUTEX_TIMEOUT_DELTA=0
CLI_OVERRIDE_MAX_CAMERA_BUDGET_EXCEEDED_DELTA=0
CLI_OVERRIDE_MAX_CAMERA_LOAD_FAILURES_DELTA=0
CLI_OVERRIDE_MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA=0
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
DISPLAY_MIN_UPDATES_DELTA=1
CAMERA_DRIVE_ENABLED=0
CAMERA_DRIVE_INTERVAL_SECONDS=11
CAMERA_DEMO_URL="${REAL_FW_CAMERA_DEMO_URL:-}"
CAMERA_DEMO_DURATION_MS=2200
CAMERA_DEMO_MUTED=0
OUT_DIR=""

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
    --max-camera-max-tick-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-camera-max-tick-us" >&2
        exit 2
      fi
      MAX_CAMERA_MAX_TICK_US="$2"
      CLI_OVERRIDE_MAX_CAMERA_MAX_TICK_US=1
      shift
      ;;
    --max-camera-max-window-hz)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-camera-max-window-hz" >&2
        exit 2
      fi
      MAX_CAMERA_MAX_WINDOW_HZ="$2"
      CLI_OVERRIDE_MAX_CAMERA_MAX_WINDOW_HZ=1
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
    --max-camera-budget-exceeded-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-camera-budget-exceeded-delta" >&2
        exit 2
      fi
      MAX_CAMERA_BUDGET_EXCEEDED_DELTA="$2"
      CLI_OVERRIDE_MAX_CAMERA_BUDGET_EXCEEDED_DELTA=1
      shift
      ;;
    --max-camera-load-failures-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-camera-load-failures-delta" >&2
        exit 2
      fi
      MAX_CAMERA_LOAD_FAILURES_DELTA="$2"
      CLI_OVERRIDE_MAX_CAMERA_LOAD_FAILURES_DELTA=1
      shift
      ;;
    --max-camera-index-swap-failures-delta)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-camera-index-swap-failures-delta" >&2
        exit 2
      fi
      MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA="$2"
      CLI_OVERRIDE_MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA=1
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
    --drive-camera-demo)
      CAMERA_DRIVE_ENABLED=1
      ;;
    --camera-drive-interval-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --camera-drive-interval-seconds" >&2
        exit 2
      fi
      CAMERA_DRIVE_INTERVAL_SECONDS="$2"
      shift
      ;;
    --camera-demo-url)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --camera-demo-url" >&2
        exit 2
      fi
      CAMERA_DEMO_URL="$2"
      shift
      ;;
    --camera-demo-duration-ms)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --camera-demo-duration-ms" >&2
        exit 2
      fi
      CAMERA_DEMO_DURATION_MS="$2"
      shift
      ;;
    --camera-demo-muted)
      CAMERA_DEMO_MUTED=1
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
  --env NAME             PlatformIO env to flash (default: waveshare-349)
  --port PATH            Fixed serial port (default: auto-detect)
  --with-fs              Upload LittleFS image before firmware upload
  --skip-flash           Skip flashing and only run soak collection
  --metrics-url URL      Poll debug metrics endpoint (default: 192.168.35.5)
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
                        Declared baseline stress class: core, display, camera,
                        or display_camera (required with --baseline-perf-csv)
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
  --drive-camera-demo   Repeatedly call camera demo endpoint during soak
  --camera-drive-interval-seconds N
                        Interval between camera demo calls (default: 11)
  --camera-demo-url URL Camera demo endpoint URL (default: derived from metrics URL)
  --camera-demo-duration-ms N
                        Camera demo duration per trigger (default: 2200)
  --camera-demo-muted   Request muted camera demo
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
  --max-camera-max-tick-us N
                        Maximum cameraMaxTickUs peak (0 disables gate)
  --max-camera-max-window-hz N
                        Maximum camera Hz (sliding window); 0 or empty disables
                        gate
  --max-ble-mutex-timeout-delta N
                        Maximum bleMutexTimeout delta (default: 0)
  --max-camera-budget-exceeded-delta N
                        Maximum cameraBudgetExceeded delta (default: 0)
  --max-camera-load-failures-delta N
                        Maximum cameraLoadFailures delta (default: 0)
  --max-camera-index-swap-failures-delta N
                        Maximum cameraIndexSwapFailures delta (default: 0)
  --profile PROFILE     Apply PERF_SLOS.md gates: drive_wifi_ap (default when
                        metrics enabled) or drive_wifi_off
  --latency-gate-mode MODE
                        Latency classification mode for wifi/disp gates:
                        strict (peak-only), robust (N-of-M), hybrid (default)
  --latency-robust-min-samples N
                        Minimum samples required to evaluate robust mode
                        (default: 8)
  --latency-robust-max-exceed-pct N
                        Allowed percent of samples above gate in robust mode
                        (default: 5)
  --wifi-robust-skip-first-samples N
                        Robust wifi warmup exclusion (default: 2)
  --dry-run             Print resolved config/gates and exit
  --allow-inconclusive   Exit 0 even when no telemetry signals were captured
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
  local camera_enabled="$2"

  if [[ "$display_enabled" -eq 1 && "$camera_enabled" -eq 1 ]]; then
    echo "display_camera"
  elif [[ "$display_enabled" -eq 1 ]]; then
    echo "display"
  elif [[ "$camera_enabled" -eq 1 ]]; then
    echo "camera"
  else
    echo "core"
  fi
}

RUN_STRESS_CLASS="$(derive_stress_class "$DISPLAY_DRIVE_ENABLED" "$CAMERA_DRIVE_ENABLED")"

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
      [[ "$CLI_OVERRIDE_MAX_CAMERA_MAX_TICK_US" -eq 0 && "$MAX_CAMERA_MAX_TICK_US" -eq 0 ]] && MAX_CAMERA_MAX_TICK_US=800
      [[ "$CLI_OVERRIDE_MAX_CAMERA_MAX_WINDOW_HZ" -eq 0 && -z "$MAX_CAMERA_MAX_WINDOW_HZ" ]] && MAX_CAMERA_MAX_WINDOW_HZ=5.05
      [[ "$CLI_OVERRIDE_MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 && "$MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 ]] && MAX_BLE_MUTEX_TIMEOUT_DELTA=0
      [[ "$CLI_OVERRIDE_MAX_CAMERA_BUDGET_EXCEEDED_DELTA" -eq 0 && "$MAX_CAMERA_BUDGET_EXCEEDED_DELTA" -eq 0 ]] && MAX_CAMERA_BUDGET_EXCEEDED_DELTA=0
      [[ "$CLI_OVERRIDE_MAX_CAMERA_LOAD_FAILURES_DELTA" -eq 0 && "$MAX_CAMERA_LOAD_FAILURES_DELTA" -eq 0 ]] && MAX_CAMERA_LOAD_FAILURES_DELTA=0
      [[ "$CLI_OVERRIDE_MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA" -eq 0 && "$MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA" -eq 0 ]] && MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA=0
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
      [[ "$CLI_OVERRIDE_MAX_CAMERA_MAX_TICK_US" -eq 0 && "$MAX_CAMERA_MAX_TICK_US" -eq 0 ]] && MAX_CAMERA_MAX_TICK_US=800
      [[ "$CLI_OVERRIDE_MAX_CAMERA_MAX_WINDOW_HZ" -eq 0 && -z "$MAX_CAMERA_MAX_WINDOW_HZ" ]] && MAX_CAMERA_MAX_WINDOW_HZ=5.05
      [[ "$CLI_OVERRIDE_MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 && "$MAX_BLE_MUTEX_TIMEOUT_DELTA" -eq 0 ]] && MAX_BLE_MUTEX_TIMEOUT_DELTA=0
      [[ "$CLI_OVERRIDE_MAX_CAMERA_BUDGET_EXCEEDED_DELTA" -eq 0 && "$MAX_CAMERA_BUDGET_EXCEEDED_DELTA" -eq 0 ]] && MAX_CAMERA_BUDGET_EXCEEDED_DELTA=0
      [[ "$CLI_OVERRIDE_MAX_CAMERA_LOAD_FAILURES_DELTA" -eq 0 && "$MAX_CAMERA_LOAD_FAILURES_DELTA" -eq 0 ]] && MAX_CAMERA_LOAD_FAILURES_DELTA=0
      [[ "$CLI_OVERRIDE_MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA" -eq 0 && "$MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA" -eq 0 ]] && MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA=0
      ;;
    *)
      echo "Unknown --profile '$SOAK_PROFILE'. Use 'drive_wifi_off' or 'drive_wifi_ap'." >&2
      exit 2
      ;;
  esac
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

if ! [[ "$DISPLAY_DRIVE_INTERVAL_SECONDS" =~ ^[0-9]+$ ]] || [[ "$DISPLAY_DRIVE_INTERVAL_SECONDS" -lt 1 ]]; then
  echo "Invalid --display-drive-interval-seconds value '$DISPLAY_DRIVE_INTERVAL_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$DISPLAY_MIN_UPDATES_DELTA" =~ ^[0-9]+$ ]]; then
  echo "Invalid --min-display-updates-delta value '$DISPLAY_MIN_UPDATES_DELTA' (expected non-negative integer)." >&2
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
  MAX_CAMERA_MAX_TICK_US \
  MAX_BLE_MUTEX_TIMEOUT_DELTA \
  MAX_CAMERA_BUDGET_EXCEEDED_DELTA \
  MAX_CAMERA_LOAD_FAILURES_DELTA \
  MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA
do
  gate_val="${!gate_var}"
  if ! [[ "$gate_val" =~ ^[0-9]+$ ]]; then
    echo "Invalid $gate_var value '$gate_val' (expected non-negative integer)." >&2
    exit 2
  fi
done

# cameraMaxWindowHz is a float gate (empty = disabled)
if [[ -n "$MAX_CAMERA_MAX_WINDOW_HZ" ]]; then
  if ! [[ "$MAX_CAMERA_MAX_WINDOW_HZ" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    echo "Invalid MAX_CAMERA_MAX_WINDOW_HZ value '$MAX_CAMERA_MAX_WINDOW_HZ' (expected non-negative number or empty)." >&2
    exit 2
  fi
fi

CAMERA_MAX_WINDOW_HZ_GATE_ENABLED=0
CAMERA_MAX_WINDOW_HZ_GATE_LABEL="disabled"
if [[ -n "$MAX_CAMERA_MAX_WINDOW_HZ" ]] && awk -v v="$MAX_CAMERA_MAX_WINDOW_HZ" 'BEGIN { exit (v > 0) ? 0 : 1 }'; then
  CAMERA_MAX_WINDOW_HZ_GATE_ENABLED=1
  CAMERA_MAX_WINDOW_HZ_GATE_LABEL="$MAX_CAMERA_MAX_WINDOW_HZ"
fi

if ! [[ "$CAMERA_DRIVE_INTERVAL_SECONDS" =~ ^[0-9]+$ ]] || [[ "$CAMERA_DRIVE_INTERVAL_SECONDS" -lt 1 ]]; then
  echo "Invalid --camera-drive-interval-seconds value '$CAMERA_DRIVE_INTERVAL_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$CAMERA_DEMO_DURATION_MS" =~ ^[0-9]+$ ]] || [[ "$CAMERA_DEMO_DURATION_MS" -lt 500 ]] || [[ "$CAMERA_DEMO_DURATION_MS" -gt 15000 ]]; then
  echo "Invalid --camera-demo-duration-ms value '$CAMERA_DEMO_DURATION_MS' (expected integer in 500..15000)." >&2
  exit 2
fi

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
    core|display|camera|display_camera) ;;
    *)
      echo "Invalid --baseline-stress-class '$BASELINE_STRESS_CLASS'. Use core|display|camera|display_camera." >&2
      exit 2
      ;;
  esac
  if [[ "$BASELINE_STRESS_CLASS" != "$RUN_STRESS_CLASS" ]]; then
    echo "Baseline stress-class mismatch: baseline='$BASELINE_STRESS_CLASS' run='$RUN_STRESS_CLASS'." >&2
    echo "Use a stress-matched baseline CSV (same display/camera drive class)." >&2
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
    DISPLAY_PREVIEW_URL="${metrics_url_base%/api/debug/metrics}/api/displaycolors/preview"
  fi

  if [[ "$CAMERA_DRIVE_ENABLED" -eq 1 && -z "$CAMERA_DEMO_URL" ]]; then
    CAMERA_DEMO_URL="${metrics_url_base%/api/debug/metrics}/api/cameras/demo"
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

if [[ "$CAMERA_DRIVE_ENABLED" -eq 1 && -z "$CAMERA_DEMO_URL" ]]; then
  echo "Camera drive is enabled but no demo URL was provided or derivable." >&2
  echo "Set --camera-demo-url or provide --metrics-url ending in /api/debug/metrics." >&2
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

if [[ -z "$OUT_DIR" ]]; then
  timestamp="$(date +%Y%m%d_%H%M%S)"
  OUT_DIR="$ROOT_DIR/.artifacts/test_reports/real_fw_soak_$timestamp"
fi
mkdir -p "$OUT_DIR"

RUN_LOG="$OUT_DIR/run.log"
SERIAL_LOG="$OUT_DIR/serial.log"
SERIAL_CAPTURE_ERR="$OUT_DIR/serial_capture.stderr"
METRICS_JSONL="$OUT_DIR/metrics.jsonl"
PANIC_JSONL="$OUT_DIR/panic.jsonl"
SUMMARY_MD="$OUT_DIR/summary.md"
: > "$RUN_LOG"
: > "$SERIAL_LOG"
: > "$SERIAL_CAPTURE_ERR"
: > "$METRICS_JSONL"
: > "$PANIC_JSONL"

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
  echo "    firmware gates: maxBleProcessMax=${MAX_BLE_PROCESS_MAX_US} maxDispPipeMax=${MAX_DISP_PIPE_MAX_US} maxCameraMaxTick=${MAX_CAMERA_MAX_TICK_US} maxCameraMaxWindowHz=${CAMERA_MAX_WINDOW_HZ_GATE_LABEL} (0 disables)"
  echo "    counter gates: maxBleMutexTimeoutDelta=${MAX_BLE_MUTEX_TIMEOUT_DELTA} maxCameraBudgetExceededDelta=${MAX_CAMERA_BUDGET_EXCEEDED_DELTA} maxCameraLoadFailuresDelta=${MAX_CAMERA_LOAD_FAILURES_DELTA} maxCameraIndexSwapFailuresDelta=${MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA}"
  echo "    resource gates: maxQueueHighWater=${MAX_QUEUE_HIGH_WATER} maxWifiConnDeferred=${MAX_WIFI_CONNECT_DEFERRED} minDmaFree=${MIN_DMA_FREE} minDmaLargest=${MIN_DMA_LARGEST} (0 disables except drive_wifi_off requires 0)"
  echo "    display drive: enabled=${DISPLAY_DRIVE_ENABLED} url=${DISPLAY_PREVIEW_URL:-disabled} interval=${DISPLAY_DRIVE_INTERVAL_SECONDS}s minDisplayUpdatesDelta=${DISPLAY_MIN_UPDATES_DELTA}"
  echo "    camera drive: enabled=${CAMERA_DRIVE_ENABLED} url=${CAMERA_DEMO_URL:-disabled} interval=${CAMERA_DRIVE_INTERVAL_SECONDS}s durationMs=${CAMERA_DEMO_DURATION_MS} muted=${CAMERA_DEMO_MUTED}"
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
echo "    firmware gates: maxBleProcessMax=${MAX_BLE_PROCESS_MAX_US} maxDispPipeMax=${MAX_DISP_PIPE_MAX_US} maxCameraMaxTick=${MAX_CAMERA_MAX_TICK_US} maxCameraMaxWindowHz=${CAMERA_MAX_WINDOW_HZ_GATE_LABEL} (0 disables)" | tee -a "$RUN_LOG"
echo "    counter gates: maxBleMutexTimeoutDelta=${MAX_BLE_MUTEX_TIMEOUT_DELTA} maxCameraBudgetExceededDelta=${MAX_CAMERA_BUDGET_EXCEEDED_DELTA} maxCameraLoadFailuresDelta=${MAX_CAMERA_LOAD_FAILURES_DELTA} maxCameraIndexSwapFailuresDelta=${MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA}" | tee -a "$RUN_LOG"
echo "    resource gates: maxQueueHighWater=${MAX_QUEUE_HIGH_WATER} maxWifiConnDeferred=${MAX_WIFI_CONNECT_DEFERRED} minDmaFree=${MIN_DMA_FREE} minDmaLargest=${MIN_DMA_LARGEST} (0 disables except drive_wifi_off requires 0)" | tee -a "$RUN_LOG"
if [[ "$BASELINE_GATES_APPLIED" -eq 1 ]]; then
  echo "    baseline csv: ${BASELINE_PERF_CSV} (profile=${BASELINE_PROFILE}, stressClass=${BASELINE_STRESS_CLASS}, runStressClass=${RUN_STRESS_CLASS}, session=${BASELINE_SELECTED_SESSION}, rows=${BASELINE_SELECTED_ROWS}, durationMs=${BASELINE_SELECTED_DURATION_MS})" | tee -a "$RUN_LOG"
  echo "    baseline factors: latency x${BASELINE_LATENCY_FACTOR}, throughput x${BASELINE_THROUGHPUT_FACTOR}; rates rx=${BASELINE_RX_RATE_PER_SEC}/s parse=${BASELINE_PARSE_RATE_PER_SEC}/s" | tee -a "$RUN_LOG"
fi
if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]]; then
  echo "    display drive: enabled (${DISPLAY_PREVIEW_URL}) every ${DISPLAY_DRIVE_INTERVAL_SECONDS}s, min displayUpdates delta=${DISPLAY_MIN_UPDATES_DELTA}" | tee -a "$RUN_LOG"
else
  echo "    display drive: disabled" | tee -a "$RUN_LOG"
fi
if [[ "$CAMERA_DRIVE_ENABLED" -eq 1 ]]; then
  echo "    camera drive: enabled (${CAMERA_DEMO_URL}) every ${CAMERA_DRIVE_INTERVAL_SECONDS}s durationMs=${CAMERA_DEMO_DURATION_MS} muted=${CAMERA_DEMO_MUTED}" | tee -a "$RUN_LOG"
else
  echo "    camera drive: disabled" | tee -a "$RUN_LOG"
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
    echo "==> Uploading filesystem image..." | tee -a "$RUN_LOG"
    run_and_log pio run -e "$ENV_NAME" -t uploadfs --upload-port "$TEST_PORT"
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
  metrics_reset_attempted=1
  echo "==> Resetting debug metrics counters via ${METRICS_RESET_URL}" | tee -a "$RUN_LOG"
  metrics_reset_http_code="$(curl -sS --max-time "$HTTP_TIMEOUT_SECONDS" -o /dev/null -w "%{http_code}" -X POST "$METRICS_RESET_URL" 2>/dev/null || true)"
  if [[ "$metrics_reset_http_code" == "200" ]]; then
    metrics_reset_success=1
    metrics_reset_reason="ok"
    echo "    metrics reset: OK (HTTP 200)" | tee -a "$RUN_LOG"
  elif [[ -n "$metrics_reset_http_code" ]]; then
    metrics_reset_reason="http_${metrics_reset_http_code}"
    echo "    metrics reset: WARN (HTTP ${metrics_reset_http_code})" | tee -a "$RUN_LOG"
  else
    metrics_reset_reason="no_response"
    echo "    metrics reset: WARN (no response)" | tee -a "$RUN_LOG"
  fi
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
if [[ "$CAMERA_DRIVE_ENABLED" -eq 1 ]]; then
  serial_capture_call_budget=$((serial_capture_call_budget + 1))
fi
SERIAL_CAPTURE_GRACE_SECONDS=$((HTTP_TIMEOUT_SECONDS * serial_capture_call_budget + POLL_SECONDS + 15))
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
next_display_drive_epoch="$soak_start_epoch"
camera_drive_calls=0
camera_drive_errors=0
next_camera_drive_epoch="$soak_start_epoch"

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
    display_drive_calls=$((display_drive_calls + 1))
    drive_resp="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" -X POST "$DISPLAY_PREVIEW_URL" 2>/dev/null || true)"
    if [[ -z "$drive_resp" ]]; then
      display_drive_errors=$((display_drive_errors + 1))
      echo "[WARN] Display drive call failed (no response)." | tee -a "$RUN_LOG"
    elif [[ "$drive_resp" == *'"active":false'* ]]; then
      # /api/displaycolors/preview toggles off when already running. Call again
      # to ensure preview is active for stress coverage.
      drive_resp2="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" -X POST "$DISPLAY_PREVIEW_URL" 2>/dev/null || true)"
      if [[ -z "$drive_resp2" ]]; then
        display_drive_errors=$((display_drive_errors + 1))
        echo "[WARN] Display drive retry failed (no response)." | tee -a "$RUN_LOG"
      elif [[ "$drive_resp2" == *'"active":false'* ]]; then
        display_drive_start_misses=$((display_drive_start_misses + 1))
        echo "[WARN] Display preview did not remain active after retry." | tee -a "$RUN_LOG"
      fi
    fi
    next_display_drive_epoch=$((now_epoch + DISPLAY_DRIVE_INTERVAL_SECONDS))
  fi

  if [[ "$CAMERA_DRIVE_ENABLED" -eq 1 && "$now_epoch" -ge "$next_camera_drive_epoch" ]]; then
    camera_drive_calls=$((camera_drive_calls + 1))
    camera_resp="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" -X POST \
      --data "type=0&durationMs=${CAMERA_DEMO_DURATION_MS}&muted=${CAMERA_DEMO_MUTED}" \
      "$CAMERA_DEMO_URL" 2>/dev/null || true)"
    if [[ -z "$camera_resp" ]]; then
      camera_drive_errors=$((camera_drive_errors + 1))
      echo "[WARN] Camera drive call failed (no response)." | tee -a "$RUN_LOG"
    fi
    next_camera_drive_epoch=$((now_epoch + CAMERA_DRIVE_INTERVAL_SECONDS))
  fi

  sleep "$POLL_SECONDS"
done

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
)
if [[ "$MAX_WIFI_MAX_US" -gt 0 ]]; then
  metrics_parser_args+=(--wifi-threshold "$MAX_WIFI_MAX_US")
fi
if [[ "$MAX_DISP_PIPE_MAX_US" -gt 0 ]]; then
  metrics_parser_args+=(--disp-threshold "$MAX_DISP_PIPE_MAX_US")
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
camera_budget_exceeded_delta=""
camera_load_failures_delta=""
camera_index_swap_failures_delta=""
camera_max_tick_peak=""
camera_max_window_hz_peak=""
camera_max_window_hz_peak_ts=""
gps_obs_drops_delta=""

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
    camera_budget_exceeded_delta) camera_budget_exceeded_delta="$value" ;;
    camera_load_failures_delta) camera_load_failures_delta="$value" ;;
    camera_index_swap_failures_delta) camera_index_swap_failures_delta="$value" ;;
    camera_max_tick_peak) camera_max_tick_peak="$value" ;;
    camera_max_window_hz_peak) camera_max_window_hz_peak="$value" ;;
    camera_max_window_hz_peak_ts) camera_max_window_hz_peak_ts="$value" ;;
    gps_obs_drops_delta) gps_obs_drops_delta="$value" ;;
  esac
done < "$metrics_kv"

panic_samples_parsed=""
panic_ok_samples_parsed=""
panic_was_crash_true=""
panic_has_panic_file_true=""
panic_last_reset_reason=""

while IFS='=' read -r key value; do
  case "$key" in
    samples) panic_samples_parsed="$value" ;;
    ok_samples) panic_ok_samples_parsed="$value" ;;
    was_crash_true) panic_was_crash_true="$value" ;;
    has_panic_file_true) panic_has_panic_file_true="$value" ;;
    last_reset_reason) panic_last_reset_reason="$value" ;;
  esac
done < "$panic_kv"

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
if [[ "$metrics_reset_success" -eq 1 ]] &&
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
if [[ "$metrics_reset_success" -eq 1 ]] &&
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
gate_panic_endpoint_fail=0
gate_display_drive_fail=0
gate_camera_drive_fail=0
gate_oversize_drop_fail=0
gate_sd_max_fail=0
gate_fs_max_fail=0
gate_queue_high_water_fail=0
gate_wifi_connect_deferred_fail=0
gate_dma_free_fail=0
gate_dma_largest_fail=0
gate_ble_process_max_fail=0
gate_disp_pipe_max_fail=0
gate_camera_max_tick_fail=0
gate_camera_max_window_hz_fail=0
gate_ble_mutex_timeout_fail=0
gate_camera_budget_exceeded_fail=0
gate_camera_load_failures_fail=0
gate_camera_index_swap_failures_fail=0

# Advisory SLO tracking (warn-only, do not cause FAIL)
advisory_warnings=()

if [[ "$monitor_died_early" -eq 1 ]]; then
  mark_gate_fail gate_serial_monitor_fail "Serial capture exited during soak."
fi
if [[ "$serial_wdt_or_panic_count" -gt 0 ]]; then
  mark_gate_fail gate_serial_panic_fail "Serial panic/WDT signatures detected (${serial_wdt_or_panic_count})."
fi
if is_uint "$panic_was_crash_true" && [[ "$panic_was_crash_true" -gt 0 ]]; then
  mark_gate_fail gate_panic_endpoint_fail "Panic endpoint reported crashes (${panic_was_crash_true})."
fi
if [[ "$METRICS_REQUIRED" -eq 1 ]]; then
  if ! is_uint "$metrics_ok_samples_parsed"; then
    mark_gate_fail gate_metrics_min_samples_fail "Metrics gate could not be evaluated (no parsed samples)."
  elif [[ "$metrics_ok_samples_parsed" -lt "$MIN_METRICS_OK_SAMPLES" ]]; then
    mark_gate_fail gate_metrics_min_samples_fail "Metrics parsed successes ${metrics_ok_samples_parsed} below required ${MIN_METRICS_OK_SAMPLES}."
  fi
fi

if [[ -n "$METRICS_URL" ]]; then
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
      if ! is_uint "$dma_largest_min_parsed"; then
        mark_gate_fail gate_dma_largest_fail "heapDmaLargestMin ${dma_largest_min_parsed:-n/a} below floor ${MIN_DMA_LARGEST}."
      elif [[ "$dma_largest_min_parsed" -lt "$MIN_DMA_LARGEST" ]]; then
        mark_gate_fail gate_dma_largest_fail "heapDmaLargestMin ${dma_largest_min_parsed} below floor ${MIN_DMA_LARGEST}."
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

    # cameraMaxTickUs (0 disables)
    if [[ "$MAX_CAMERA_MAX_TICK_US" -gt 0 ]]; then
      if ! is_uint "$camera_max_tick_peak"; then
        mark_gate_fail gate_camera_max_tick_fail "cameraMaxTickUs peak ${camera_max_tick_peak:-n/a} above max ${MAX_CAMERA_MAX_TICK_US}."
      elif [[ "$camera_max_tick_peak" -gt "$MAX_CAMERA_MAX_TICK_US" ]]; then
        mark_gate_fail gate_camera_max_tick_fail "cameraMaxTickUs peak ${camera_max_tick_peak} above max ${MAX_CAMERA_MAX_TICK_US}."
      fi
    fi

    # cameraMaxWindowHz (0/empty disables; missing metric is a hard failure)
    if [[ "$CAMERA_MAX_WINDOW_HZ_GATE_ENABLED" -eq 1 ]]; then
      if [[ -z "$camera_max_window_hz_peak" ]]; then
        mark_gate_fail gate_camera_max_window_hz_fail "cameraMaxWindowHz unavailable (required <= ${MAX_CAMERA_MAX_WINDOW_HZ})."
      elif ! [[ "$camera_max_window_hz_peak" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
        mark_gate_fail gate_camera_max_window_hz_fail "cameraMaxWindowHz invalid value '${camera_max_window_hz_peak}' (required <= ${MAX_CAMERA_MAX_WINDOW_HZ})."
      elif awk -v obs="$camera_max_window_hz_peak" -v lim="$MAX_CAMERA_MAX_WINDOW_HZ" \
          'BEGIN { exit (obs > lim) ? 0 : 1 }'; then
        mark_gate_fail gate_camera_max_window_hz_fail "cameraMaxWindowHz ${camera_max_window_hz_peak} above max ${MAX_CAMERA_MAX_WINDOW_HZ}."
      fi
    fi

    # bleMutexTimeout (zero-tolerance counter)
    if ! is_uint "$ble_mutex_timeout_delta"; then
      mark_gate_fail gate_ble_mutex_timeout_fail "bleMutexTimeout delta ${ble_mutex_timeout_delta:-n/a} above max ${MAX_BLE_MUTEX_TIMEOUT_DELTA}."
    elif [[ "$ble_mutex_timeout_delta" -gt "$MAX_BLE_MUTEX_TIMEOUT_DELTA" ]]; then
      mark_gate_fail gate_ble_mutex_timeout_fail "bleMutexTimeout delta ${ble_mutex_timeout_delta} above max ${MAX_BLE_MUTEX_TIMEOUT_DELTA}."
    fi

    # cameraBudgetExceeded (zero-tolerance counter)
    if ! is_uint "$camera_budget_exceeded_delta"; then
      mark_gate_fail gate_camera_budget_exceeded_fail "cameraBudgetExceeded delta ${camera_budget_exceeded_delta:-n/a} above max ${MAX_CAMERA_BUDGET_EXCEEDED_DELTA}."
    elif [[ "$camera_budget_exceeded_delta" -gt "$MAX_CAMERA_BUDGET_EXCEEDED_DELTA" ]]; then
      mark_gate_fail gate_camera_budget_exceeded_fail "cameraBudgetExceeded delta ${camera_budget_exceeded_delta} above max ${MAX_CAMERA_BUDGET_EXCEEDED_DELTA}."
    fi

    # cameraLoadFailures (zero-tolerance counter)
    if ! is_uint "$camera_load_failures_delta"; then
      mark_gate_fail gate_camera_load_failures_fail "cameraLoadFailures delta ${camera_load_failures_delta:-n/a} above max ${MAX_CAMERA_LOAD_FAILURES_DELTA}."
    elif [[ "$camera_load_failures_delta" -gt "$MAX_CAMERA_LOAD_FAILURES_DELTA" ]]; then
      mark_gate_fail gate_camera_load_failures_fail "cameraLoadFailures delta ${camera_load_failures_delta} above max ${MAX_CAMERA_LOAD_FAILURES_DELTA}."
    fi

    # cameraIndexSwapFailures (zero-tolerance counter)
    if ! is_uint "$camera_index_swap_failures_delta"; then
      mark_gate_fail gate_camera_index_swap_failures_fail "cameraIndexSwapFailures delta ${camera_index_swap_failures_delta:-n/a} above max ${MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA}."
    elif [[ "$camera_index_swap_failures_delta" -gt "$MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA" ]]; then
      mark_gate_fail gate_camera_index_swap_failures_fail "cameraIndexSwapFailures delta ${camera_index_swap_failures_delta} above max ${MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA}."
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
    if is_uint "$gps_obs_drops_delta" && [[ "$gps_obs_drops_delta" -gt 0 ]]; then
      advisory_warnings+=("gpsObsDrops delta=${gps_obs_drops_delta} indicates dropped GPS observations.")
    fi
  fi
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
if [[ "$CAMERA_DRIVE_ENABLED" -eq 1 ]]; then
  if [[ "$camera_drive_calls" -eq 0 ]]; then
    mark_gate_fail gate_camera_drive_fail "Camera drive produced zero calls."
  fi
  if [[ "$camera_drive_errors" -gt 0 ]]; then
    mark_gate_fail gate_camera_drive_fail "Camera drive had ${camera_drive_errors} failed call(s)."
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
  if [[ "$gate_metrics_window_fail" -eq 1 && "$serial_log_bytes" -eq 0 ]]; then
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
    diagnosis_next_action="Inspect queueHighWater/wifiConnectDeferred/DMA floor counters around failing samples and tune backpressure or memory usage."
  elif [[ "$gate_loop_fail" -eq 1 || "$gate_wifi_fail" -eq 1 || "$gate_flush_fail" -eq 1 || "$gate_ble_drain_fail" -eq 1 || "$gate_sd_max_fail" -eq 1 || "$gate_fs_max_fail" -eq 1 ]]; then
    if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 || "$CAMERA_DRIVE_ENABLED" -eq 1 ]]; then
      diagnosis_bucket="Stress Latency Regression"
      diagnosis_next_action="Run paired tests: core (no stress drivers) and stress (drivers on). Optimize only the delta introduced by stress paths."
    else
      diagnosis_bucket="Core Latency Regression"
      diagnosis_next_action="Profile loop/wifi/flush peaks at reported timestamps and reduce blocking work on the main loop."
    fi
  elif [[ "$gate_display_drive_fail" -eq 1 || "$gate_camera_drive_fail" -eq 1 ]]; then
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
  echo "- Metrics samples (shell): $metrics_samples"
  echo "- Metrics successful (shell): $metrics_ok_samples"
  echo "- Metrics samples parsed: ${metrics_samples_parsed:-0}"
  echo "- Metrics successful parsed: ${metrics_ok_samples_parsed:-0}"
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
  echo "- Peak bleDrainMaxUs: ${ble_drain_max_peak:-n/a} (max gate ${MAX_BLE_DRAIN_MAX_US})"
  echo "- Peak sdMaxUs: ${sd_max_peak:-n/a} (max gate ${MAX_SD_MAX_US})"
  echo "- Peak fsMaxUs: ${fs_max_peak:-n/a} (max gate ${MAX_FS_MAX_US})"
  echo "- Peak bleProcessMaxUs: ${ble_process_max_peak:-n/a} (max gate ${MAX_BLE_PROCESS_MAX_US})"
  echo "- Peak dispPipeMaxUs: ${disp_pipe_max_peak:-n/a} (max gate ${MAX_DISP_PIPE_MAX_US})"
  echo "- Robust dispPipe samples/p95/over-limit: ${disp_pipe_sample_count:-n/a} / ${disp_pipe_p95:-n/a} / ${disp_pipe_over_limit_count:-n/a} (allowed ${disp_pipe_robust_allowed_over_limit:-n/a})"
  echo "- Peak cameraMaxTickUs: ${camera_max_tick_peak:-n/a} (max gate ${MAX_CAMERA_MAX_TICK_US})"
  echo "- Peak cameraMaxWindowHz (computed, 15s window): ${camera_max_window_hz_peak:-n/a} (ts ${camera_max_window_hz_peak_ts:-n/a}, max gate ${CAMERA_MAX_WINDOW_HZ_GATE_LABEL})"
  echo "- oversizeDrops delta: ${oversize_drops_delta:-n/a} (max ${MAX_OVERSIZE_DROPS_DELTA})"
  echo "- queueHighWater first/peak: ${queue_high_water_first:-n/a} / ${queue_high_water_peak:-n/a} (max ${MAX_QUEUE_HIGH_WATER})"
  echo "- Inherited counter suspect: ${inherited_counter_suspect:-n/a}"
  echo "- wifiConnectDeferred delta: ${wifi_connect_deferred_delta:-n/a} (max ${MAX_WIFI_CONNECT_DEFERRED}; drive_wifi_off requires 0)"
  echo "- bleMutexTimeout delta: ${ble_mutex_timeout_delta:-n/a} (max ${MAX_BLE_MUTEX_TIMEOUT_DELTA})"
  echo "- cameraBudgetExceeded delta: ${camera_budget_exceeded_delta:-n/a} (max ${MAX_CAMERA_BUDGET_EXCEEDED_DELTA})"
  echo "- cameraLoadFailures delta: ${camera_load_failures_delta:-n/a} (max ${MAX_CAMERA_LOAD_FAILURES_DELTA})"
  echo "- cameraIndexSwapFailures delta: ${camera_index_swap_failures_delta:-n/a} (max ${MAX_CAMERA_INDEX_SWAP_FAILURES_DELTA})"
  echo "- gpsObsDrops delta: ${gps_obs_drops_delta:-n/a} (advisory report-only)"
  echo "- Min heapDmaMin (SLO): ${dma_free_min_parsed:-n/a} (floor ${MIN_DMA_FREE})"
  echo "- Min heapDmaLargestMin (SLO): ${dma_largest_min_parsed:-n/a} (floor ${MIN_DMA_LARGEST})"
  echo "- reconnects delta: ${reconnects_delta:-n/a}"
  echo "- disconnects delta: ${disconnects_delta:-n/a}"
  echo "- Proxy drop peak: ${proxy_drop_peak:-n/a}"
  echo "- Lockout core guard tripped count: ${core_guard_tripped_count:-n/a}"
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
  echo "- Display drive interval (s): ${DISPLAY_DRIVE_INTERVAL_SECONDS}"
  echo "- Display drive calls: ${display_drive_calls}"
  echo "- Display drive errors: ${display_drive_errors}"
  echo "- Display drive start misses: ${display_drive_start_misses}"
  echo "- Minimum required displayUpdates delta: ${DISPLAY_MIN_UPDATES_DELTA}"
  echo ""
  echo "## Camera Drive"
  echo ""
  echo "- Camera drive enabled: $([[ "$CAMERA_DRIVE_ENABLED" -eq 1 ]] && echo "yes" || echo "no")"
  echo "- Camera demo URL: ${CAMERA_DEMO_URL:-disabled}"
  echo "- Camera drive interval (s): ${CAMERA_DRIVE_INTERVAL_SECONDS}"
  echo "- Camera demo duration (ms): ${CAMERA_DEMO_DURATION_MS}"
  echo "- Camera demo muted: ${CAMERA_DEMO_MUTED}"
  echo "- Camera drive calls: ${camera_drive_calls}"
  echo "- Camera drive errors: ${camera_drive_errors}"
  echo ""
  echo "## Panic Endpoint"
  echo ""
  echo "- Panic URL: ${PANIC_URL:-disabled}"
  echo "- Panic poll URL: ${PANIC_POLL_URL:-disabled}"
  echo "- Panic samples parsed: ${panic_samples_parsed:-0}"
  echo "- Panic successful parsed: ${panic_ok_samples_parsed:-0}"
  echo "- Panic wasCrash=true count: ${panic_was_crash_true:-0}"
  echo "- Panic hasPanicFile=true count: ${panic_has_panic_file_true:-0}"
  echo "- Panic latest reset reason: ${panic_last_reset_reason:-n/a}"
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
} > "$SUMMARY_MD"

echo "==> Real firmware soak complete"
echo "    result: $result"
echo "    summary: $SUMMARY_MD"
echo "    serial log: $SERIAL_LOG"

if [[ "$result" == "FAIL" ]]; then
  exit 1
fi
if [[ "$result" == "INCONCLUSIVE" && "$ALLOW_INCONCLUSIVE" -ne 1 ]]; then
  exit 2
fi
