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
BASELINE_PERF_CSV=""
BASELINE_PERF_SESSION="last-connected"
BASELINE_LATENCY_FACTOR="1.20"
BASELINE_THROUGHPUT_FACTOR="0.50"
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
      shift
      ;;
    --max-loop-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-loop-max-us" >&2
        exit 2
      fi
      MAX_LOOP_MAX_US="$2"
      shift
      ;;
    --max-wifi-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-wifi-max-us" >&2
        exit 2
      fi
      MAX_WIFI_MAX_US="$2"
      shift
      ;;
    --max-ble-drain-max-us)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --max-ble-drain-max-us" >&2
        exit 2
      fi
      MAX_BLE_DRAIN_MAX_US="$2"
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
  --baseline-latency-factor F
                        Multiply baseline peaks for max gates (default: 1.20)
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
  MAX_BLE_DRAIN_MAX_US
do
  gate_val="${!gate_var}"
  if ! [[ "$gate_val" =~ ^[0-9]+$ ]]; then
    echo "Invalid $gate_var value '$gate_val' (expected non-negative integer)." >&2
    exit 2
  fi
done

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

if [[ -n "$BASELINE_PERF_CSV" ]]; then
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

if [[ -n "$METRICS_URL" && -z "$PANIC_URL" ]]; then
  PANIC_URL="${METRICS_URL%/api/debug/metrics}/api/debug/panic"
fi

if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && -z "$DISPLAY_PREVIEW_URL" && -n "$METRICS_URL" ]]; then
  DISPLAY_PREVIEW_URL="${METRICS_URL%/api/debug/metrics}/api/displaycolors/preview"
fi

if [[ "$CAMERA_DRIVE_ENABLED" -eq 1 && -z "$CAMERA_DEMO_URL" && -n "$METRICS_URL" ]]; then
  CAMERA_DEMO_URL="${METRICS_URL%/api/debug/metrics}/api/cameras/demo"
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
  echo "    duration: ${DURATION_SECONDS}s"
  echo "    poll: ${POLL_SECONDS}s"
  echo "    serial baud: ${SERIAL_BAUD}"
  echo "    http timeout: ${HTTP_TIMEOUT_SECONDS}s"
  echo "    metrics url: ${METRICS_URL:-disabled}"
  echo "    panic url: ${PANIC_URL:-disabled}"
  echo "    metrics required: ${METRICS_REQUIRED}"
  echo "    min metrics successes: ${MIN_METRICS_OK_SAMPLES}"
  echo "    runtime gates: minRxDelta=${MIN_RX_PACKETS_DELTA} minParseSuccessDelta=${MIN_PARSE_SUCCESSES_DELTA} maxParseFailDelta=${MAX_PARSE_FAILURES_DELTA} maxQueueDropDelta=${MAX_QUEUE_DROPS_DELTA} maxPerfDropDelta=${MAX_PERF_DROPS_DELTA} maxEventDropDelta=${MAX_EVENT_DROPS_DELTA}"
  echo "    latency gates: maxFlush=${MAX_FLUSH_MAX_US} maxLoop=${MAX_LOOP_MAX_US} maxWifi=${MAX_WIFI_MAX_US} maxBleDrain=${MAX_BLE_DRAIN_MAX_US} (0 disables)"
  echo "    display drive: enabled=${DISPLAY_DRIVE_ENABLED} url=${DISPLAY_PREVIEW_URL:-disabled} interval=${DISPLAY_DRIVE_INTERVAL_SECONDS}s minDisplayUpdatesDelta=${DISPLAY_MIN_UPDATES_DELTA}"
  echo "    camera drive: enabled=${CAMERA_DRIVE_ENABLED} url=${CAMERA_DEMO_URL:-disabled} interval=${CAMERA_DRIVE_INTERVAL_SECONDS}s durationMs=${CAMERA_DEMO_DURATION_MS} muted=${CAMERA_DEMO_MUTED}"
  echo "    out dir: $OUT_DIR"
  if [[ "$BASELINE_GATES_APPLIED" -eq 1 ]]; then
    echo "    baseline csv: ${BASELINE_PERF_CSV} (session=${BASELINE_SELECTED_SESSION}, rows=${BASELINE_SELECTED_ROWS}, durationMs=${BASELINE_SELECTED_DURATION_MS})"
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
echo "    duration: ${DURATION_SECONDS}s" | tee -a "$RUN_LOG"
echo "    poll: ${POLL_SECONDS}s" | tee -a "$RUN_LOG"
echo "    serial baud: ${SERIAL_BAUD}" | tee -a "$RUN_LOG"
echo "    http timeout: ${HTTP_TIMEOUT_SECONDS}s" | tee -a "$RUN_LOG"
echo "    metrics url: ${METRICS_URL:-disabled}" | tee -a "$RUN_LOG"
if [[ "$METRICS_REQUIRED" -eq 1 ]]; then
  echo "    metrics gate: require >= ${MIN_METRICS_OK_SAMPLES} parsed successes" | tee -a "$RUN_LOG"
fi
echo "    runtime gates: minRxDelta=${MIN_RX_PACKETS_DELTA} minParseSuccessDelta=${MIN_PARSE_SUCCESSES_DELTA} maxParseFailDelta=${MAX_PARSE_FAILURES_DELTA} maxQueueDropDelta=${MAX_QUEUE_DROPS_DELTA} maxPerfDropDelta=${MAX_PERF_DROPS_DELTA} maxEventDropDelta=${MAX_EVENT_DROPS_DELTA}" | tee -a "$RUN_LOG"
echo "    latency gates: maxFlush=${MAX_FLUSH_MAX_US} maxLoop=${MAX_LOOP_MAX_US} maxWifi=${MAX_WIFI_MAX_US} maxBleDrain=${MAX_BLE_DRAIN_MAX_US} (0 disables)" | tee -a "$RUN_LOG"
if [[ "$BASELINE_GATES_APPLIED" -eq 1 ]]; then
  echo "    baseline csv: ${BASELINE_PERF_CSV} (session=${BASELINE_SELECTED_SESSION}, rows=${BASELINE_SELECTED_ROWS}, durationMs=${BASELINE_SELECTED_DURATION_MS})" | tee -a "$RUN_LOG"
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
if [[ -n "$PANIC_URL" ]]; then
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

end_time = time.time() + duration
ser = serial.Serial(port=port, baudrate=baud, timeout=0.25)
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

  if [[ -n "$METRICS_URL" ]]; then
    metrics_samples=$((metrics_samples + 1))
    payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$METRICS_URL" 2>/dev/null || true)"
    if [[ -n "$payload" ]]; then
      metrics_ok_samples=$((metrics_ok_samples + 1))
      payload_oneline="$(printf "%s" "$payload" | tr -d '\r\n')"
      printf '{"ts":"%s","ok":true,"data":%s}\n' "$now_utc" "$payload_oneline" >> "$METRICS_JSONL"
    else
      printf '{"ts":"%s","ok":false}\n' "$now_utc" >> "$METRICS_JSONL"
    fi
  fi

  if [[ -n "$PANIC_URL" ]]; then
    panic_samples=$((panic_samples + 1))
    panic_payload="$(curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$PANIC_URL" 2>/dev/null || true)"
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
if ! "$PARSER_PYTHON" "$ROOT_DIR/tools/soak_parse_metrics.py" "$METRICS_JSONL" > "$metrics_kv"; then
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
ble_drain_max_peak=""
loop_peak_ts=""
loop_peak_wifi=""
loop_peak_flush=""
loop_peak_ble_drain=""
loop_peak_display_updates=""
loop_peak_rx_packets=""
wifi_peak_ts=""
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
    ble_drain_max_peak) ble_drain_max_peak="$value" ;;
    loop_peak_ts) loop_peak_ts="$value" ;;
    loop_peak_wifi) loop_peak_wifi="$value" ;;
    loop_peak_flush) loop_peak_flush="$value" ;;
    loop_peak_ble_drain) loop_peak_ble_drain="$value" ;;
    loop_peak_display_updates) loop_peak_display_updates="$value" ;;
    loop_peak_rx_packets) loop_peak_rx_packets="$value" ;;
    wifi_peak_ts) wifi_peak_ts="$value" ;;
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
      if ! is_uint "$wifi_max_peak"; then
        mark_gate_fail gate_wifi_fail "wifiMaxUs peak ${wifi_max_peak:-n/a} above max ${MAX_WIFI_MAX_US}."
      elif [[ "$wifi_max_peak" -gt "$MAX_WIFI_MAX_US" ]]; then
        mark_gate_fail gate_wifi_fail "wifiMaxUs peak ${wifi_max_peak} above max ${MAX_WIFI_MAX_US}."
      fi
    fi
    if [[ "$MAX_BLE_DRAIN_MAX_US" -gt 0 ]]; then
      if ! is_uint "$ble_drain_max_peak"; then
        mark_gate_fail gate_ble_drain_fail "bleDrainMaxUs peak ${ble_drain_max_peak:-n/a} above max ${MAX_BLE_DRAIN_MAX_US}."
      elif [[ "$ble_drain_max_peak" -gt "$MAX_BLE_DRAIN_MAX_US" ]]; then
        mark_gate_fail gate_ble_drain_fail "bleDrainMaxUs peak ${ble_drain_max_peak} above max ${MAX_BLE_DRAIN_MAX_US}."
      fi
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

if [[ "$BASELINE_GATES_APPLIED" -eq 1 && ( "$DISPLAY_DRIVE_ENABLED" -eq 1 || "$CAMERA_DRIVE_ENABLED" -eq 1 ) ]]; then
  diagnosis_baseline_note="Baseline gates came from perf CSV and this run used active stress drivers; compare against a stress baseline for release gating."
fi

if [[ "$result" == "FAIL" ]]; then
  if [[ "$gate_metrics_window_fail" -eq 1 && "$serial_log_bytes" -eq 0 ]]; then
    diagnosis_bucket="Connectivity/Telemetry Outage"
    diagnosis_next_action="Verify WiFi/API reachability and USB serial capture first, then rerun before evaluating firmware."
  elif [[ "$gate_serial_monitor_fail" -eq 1 && "$serial_log_bytes" -eq 0 ]]; then
    diagnosis_bucket="USB Serial Capture Failure"
    diagnosis_next_action="Stabilize /dev/cu.usbmodem capture (close other monitors/cables), rerun soak."
  elif [[ "$gate_parse_fail_fail" -eq 1 || "$gate_queue_drop_fail" -eq 1 || "$gate_perf_drop_fail" -eq 1 || "$gate_event_drop_fail" -eq 1 ]]; then
    diagnosis_bucket="Core Data-Path Integrity Regression"
    diagnosis_next_action="Inspect parser/queue/event counters around failing timestamps and bisect recent BLE/parser changes."
  elif [[ "$gate_loop_fail" -eq 1 || "$gate_wifi_fail" -eq 1 || "$gate_flush_fail" -eq 1 || "$gate_ble_drain_fail" -eq 1 ]]; then
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
  echo "- Peak wifiMaxUs: ${wifi_max_peak:-n/a} (max gate ${MAX_WIFI_MAX_US})"
  echo "- Peak bleDrainMaxUs: ${ble_drain_max_peak:-n/a} (max gate ${MAX_BLE_DRAIN_MAX_US})"
  echo "- Proxy drop peak: ${proxy_drop_peak:-n/a}"
  echo "- Lockout core guard tripped count: ${core_guard_tripped_count:-n/a}"
  echo ""
  echo "## Baseline-Derived Gates"
  echo ""
  if [[ "$BASELINE_GATES_APPLIED" -eq 1 ]]; then
    echo "- Baseline perf CSV: \`${BASELINE_PERF_CSV}\`"
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
