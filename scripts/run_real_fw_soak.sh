#!/usr/bin/env bash
#
# run_real_fw_soak.sh - Flash production firmware and soak-test the real runtime
# on hardware with serial crash detection and optional debug API polling.
#
# Usage examples:
#   ./scripts/run_real_fw_soak.sh --duration-seconds 600
#   ./scripts/run_real_fw_soak.sh --duration-seconds 1800 --metrics-url http://192.168.35.5/api/debug/metrics
#   ./scripts/run_real_fw_soak.sh --skip-flash --duration-seconds 900 --metrics-url http://192.168.35.5/api/debug/metrics --drive-display-preview
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
UPLOAD_FS=0
SKIP_FLASH=0
METRICS_URL="${REAL_FW_METRICS_URL:-http://192.168.35.5/api/debug/metrics}"
PANIC_URL="${REAL_FW_PANIC_URL:-}"
METRICS_REQUIRED=0
ALLOW_INCONCLUSIVE=0
DISPLAY_DRIVE_ENABLED=0
DISPLAY_DRIVE_INTERVAL_SECONDS=7
DISPLAY_PREVIEW_URL="${REAL_FW_DISPLAY_PREVIEW_URL:-}"
DISPLAY_MIN_UPDATES_DELTA=1
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
    --allow-inconclusive)
      ALLOW_INCONCLUSIVE=1
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
  --env NAME             PlatformIO env to flash (default: waveshare-349)
  --port PATH            Fixed serial port (default: auto-detect)
  --with-fs              Upload LittleFS image before firmware upload
  --skip-flash           Skip flashing and only run soak collection
  --metrics-url URL      Poll debug metrics endpoint (default: 192.168.35.5)
  --panic-url URL        Poll debug panic endpoint (default: derived from metrics URL)
  --no-metrics           Disable debug API polling (serial-only soak)
  --require-metrics      Fail run if no successful metrics samples are captured
  --drive-display-preview
                        Repeatedly call display preview endpoint during soak
  --display-drive-interval-seconds N
                        Interval between display preview triggers (default: 7)
  --display-preview-url URL
                        Display preview endpoint URL (default: derived from metrics URL)
  --min-display-updates-delta N
                        Fail when parsed displayUpdates delta is below N (default: 1)
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

if ! [[ "$DISPLAY_DRIVE_INTERVAL_SECONDS" =~ ^[0-9]+$ ]] || [[ "$DISPLAY_DRIVE_INTERVAL_SECONDS" -lt 1 ]]; then
  echo "Invalid --display-drive-interval-seconds value '$DISPLAY_DRIVE_INTERVAL_SECONDS' (expected positive integer)." >&2
  exit 2
fi

if ! [[ "$DISPLAY_MIN_UPDATES_DELTA" =~ ^[0-9]+$ ]]; then
  echo "Invalid --min-display-updates-delta value '$DISPLAY_MIN_UPDATES_DELTA' (expected non-negative integer)." >&2
  exit 2
fi

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO (pio) is required but not found in PATH." >&2
  exit 1
fi

if [[ -n "$METRICS_URL" && -z "$PANIC_URL" ]]; then
  PANIC_URL="${METRICS_URL%/api/debug/metrics}/api/debug/panic"
fi

if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && -z "$DISPLAY_PREVIEW_URL" && -n "$METRICS_URL" ]]; then
  DISPLAY_PREVIEW_URL="${METRICS_URL%/api/debug/metrics}/api/displaycolors/preview"
fi

if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 && -z "$DISPLAY_PREVIEW_URL" ]]; then
  echo "Display drive is enabled but no preview URL was provided or derivable." >&2
  echo "Set --display-preview-url or provide --metrics-url ending in /api/debug/metrics." >&2
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

if [[ -z "$TEST_PORT" ]]; then
  TEST_PORT="$(detect_usb_port || true)"
fi
if [[ -z "$TEST_PORT" ]]; then
  echo "No USB serial device detected. Connect the board and retry." >&2
  exit 1
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

run_and_log() {
  set +e
  "$@" 2>&1 | tee -a "$RUN_LOG"
  local status=${PIPESTATUS[0]}
  set -e
  return "$status"
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
echo "    metrics url: ${METRICS_URL:-disabled}" | tee -a "$RUN_LOG"
if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]]; then
  echo "    display drive: enabled (${DISPLAY_PREVIEW_URL}) every ${DISPLAY_DRIVE_INTERVAL_SECONDS}s, min displayUpdates delta=${DISPLAY_MIN_UPDATES_DELTA}" | tee -a "$RUN_LOG"
else
  echo "    display drive: disabled" | tee -a "$RUN_LOG"
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

"$SERIAL_PYTHON" - "$MONITOR_PORT" "115200" "$SERIAL_LOG" "$DURATION_SECONDS" > "$SERIAL_CAPTURE_ERR" 2>&1 <<'PY' &
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
    payload="$(curl -fsS --max-time 2 "$METRICS_URL" 2>/dev/null || true)"
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
    panic_payload="$(curl -fsS --max-time 2 "$PANIC_URL" 2>/dev/null || true)"
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
    drive_resp="$(curl -fsS --max-time 2 -X POST "$DISPLAY_PREVIEW_URL" 2>/dev/null || true)"
    if [[ -z "$drive_resp" ]]; then
      display_drive_errors=$((display_drive_errors + 1))
      echo "[WARN] Display drive call failed (no response)." | tee -a "$RUN_LOG"
    elif [[ "$drive_resp" == *'"active":false'* ]]; then
      # /api/displaycolors/preview toggles off when already running. Call again
      # to ensure preview is active for stress coverage.
      drive_resp2="$(curl -fsS --max-time 2 -X POST "$DISPLAY_PREVIEW_URL" 2>/dev/null || true)"
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

  sleep "$POLL_SECONDS"
done

soak_end_utc="$(date -u +"%Y-%m-%dT%H:%M:%SZ")"
soak_end_epoch_actual="$(date +%s)"
soak_elapsed_s=$((soak_end_epoch_actual - soak_start_epoch))

set +e
wait "$MONITOR_PID"
serial_capture_status=$?
set -e
if [[ "$serial_capture_status" -ne 0 ]]; then
  monitor_died_early=1
fi

trap - EXIT

serial_reset_count="$(grep -c 'rst:0x' "$SERIAL_LOG" || true)"
serial_wdt_or_panic_count="$(grep -Eic 'task watchdog|task_wdt|Guru Meditation|panic|abort\(' "$SERIAL_LOG" || true)"
serial_guru_count="$(grep -Eic 'Guru Meditation' "$SERIAL_LOG" || true)"

metrics_kv="$OUT_DIR/metrics_kv.txt"
panic_kv="$OUT_DIR/panic_kv.txt"

python3 - "$METRICS_JSONL" <<'PY' > "$metrics_kv"
import json
import sys

path = sys.argv[1]
samples = 0
ok_samples = 0

heap_free_min = None
heap_min_free_min = None
heap_dma_min = None
heap_dma_largest_min = None
latency_max_peak = None
proxy_drop_peak = None
display_updates_first = None
display_updates_last = None
display_skips_first = None
display_skips_last = None
flush_max_peak = None
loop_max_peak = None
wifi_max_peak = None
ble_drain_max_peak = None

event_publish_first = None
event_publish_last = None
event_drop_first = None
event_drop_last = None
event_size_peak = None
core_guard_tripped_count = 0

def update_min(cur, val):
    if val is None:
        return cur
    if cur is None or val < cur:
        return val
    return cur

def update_max(cur, val):
    if val is None:
        return cur
    if cur is None or val > cur:
        return val
    return cur

def num(v):
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)):
        return v
    return None

try:
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            samples += 1
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if not rec.get("ok"):
                continue
            data = rec.get("data")
            if not isinstance(data, dict):
                continue
            ok_samples += 1

            heap_free_min = update_min(heap_free_min, num(data.get("heapFree")))
            heap_min_free_min = update_min(heap_min_free_min, num(data.get("heapMinFree")))
            heap_dma_min = update_min(heap_dma_min, num(data.get("heapDma")))
            heap_dma_largest_min = update_min(heap_dma_largest_min, num(data.get("heapDmaLargest")))
            latency_max_peak = update_max(latency_max_peak, num(data.get("latencyMaxUs")))
            flush_max_peak = update_max(flush_max_peak, num(data.get("flushMaxUs")))
            loop_max_peak = update_max(loop_max_peak, num(data.get("loopMaxUs")))
            wifi_max_peak = update_max(wifi_max_peak, num(data.get("wifiMaxUs")))
            ble_drain_max_peak = update_max(ble_drain_max_peak, num(data.get("bleDrainMaxUs")))

            display_updates = num(data.get("displayUpdates"))
            if display_updates_first is None and display_updates is not None:
                display_updates_first = display_updates
            if display_updates is not None:
                display_updates_last = display_updates

            display_skips = num(data.get("displaySkips"))
            if display_skips_first is None and display_skips is not None:
                display_skips_first = display_skips
            if display_skips is not None:
                display_skips_last = display_skips

            proxy = data.get("proxy")
            if isinstance(proxy, dict):
                proxy_drop_peak = update_max(proxy_drop_peak, num(proxy.get("dropCount")))

            event_bus = data.get("eventBus")
            if isinstance(event_bus, dict):
                pub = num(event_bus.get("publishCount"))
                drp = num(event_bus.get("dropCount"))
                siz = num(event_bus.get("size"))
                if event_publish_first is None and pub is not None:
                    event_publish_first = pub
                if pub is not None:
                    event_publish_last = pub
                if event_drop_first is None and drp is not None:
                    event_drop_first = drp
                if drp is not None:
                    event_drop_last = drp
                event_size_peak = update_max(event_size_peak, siz)

            lockout = data.get("lockout")
            if isinstance(lockout, dict):
                if lockout.get("coreGuardTripped") is True:
                    core_guard_tripped_count += 1
except FileNotFoundError:
    pass

def emit(key, val):
    if val is None:
        print(f"{key}=")
    else:
        print(f"{key}={val}")

emit("samples", samples)
emit("ok_samples", ok_samples)
emit("heap_free_min", heap_free_min)
emit("heap_min_free_min", heap_min_free_min)
emit("heap_dma_min", heap_dma_min)
emit("heap_dma_largest_min", heap_dma_largest_min)
emit("latency_max_peak", latency_max_peak)
emit("proxy_drop_peak", proxy_drop_peak)
emit("display_updates_first", display_updates_first)
emit("display_updates_last", display_updates_last)
emit("display_skips_first", display_skips_first)
emit("display_skips_last", display_skips_last)
emit("flush_max_peak", flush_max_peak)
emit("loop_max_peak", loop_max_peak)
emit("wifi_max_peak", wifi_max_peak)
emit("ble_drain_max_peak", ble_drain_max_peak)
emit("event_publish_first", event_publish_first)
emit("event_publish_last", event_publish_last)
emit("event_drop_first", event_drop_first)
emit("event_drop_last", event_drop_last)
emit("event_size_peak", event_size_peak)
emit("core_guard_tripped_count", core_guard_tripped_count)

if event_publish_first is None or event_publish_last is None:
    print("event_publish_delta=")
else:
    print(f"event_publish_delta={event_publish_last - event_publish_first}")

if event_drop_first is None or event_drop_last is None:
    print("event_drop_delta=")
else:
    print(f"event_drop_delta={event_drop_last - event_drop_first}")

if display_updates_first is None or display_updates_last is None:
    print("display_updates_delta=")
else:
    print(f"display_updates_delta={display_updates_last - display_updates_first}")

if display_skips_first is None or display_skips_last is None:
    print("display_skips_delta=")
else:
    print(f"display_skips_delta={display_skips_last - display_skips_first}")
PY

python3 - "$PANIC_JSONL" <<'PY' > "$panic_kv"
import json
import sys

path = sys.argv[1]
samples = 0
ok_samples = 0
was_crash_true = 0
has_panic_file_true = 0
last_reset_reason = ""

try:
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.strip()
            if not line:
                continue
            samples += 1
            try:
                rec = json.loads(line)
            except Exception:
                continue
            if not rec.get("ok"):
                continue
            data = rec.get("data")
            if not isinstance(data, dict):
                continue
            ok_samples += 1
            if data.get("wasCrash") is True:
                was_crash_true += 1
            if data.get("hasPanicFile") is True:
                has_panic_file_true += 1
            lr = data.get("lastResetReason")
            if isinstance(lr, str) and lr:
                last_reset_reason = lr
except FileNotFoundError:
    pass

print(f"samples={samples}")
print(f"ok_samples={ok_samples}")
print(f"was_crash_true={was_crash_true}")
print(f"has_panic_file_true={has_panic_file_true}")
print(f"last_reset_reason={last_reset_reason}")
PY

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

result="PASS"
serial_log_bytes="$(wc -c < "$SERIAL_LOG" | tr -d '[:space:]')"
if [[ -z "$serial_log_bytes" ]]; then
  serial_log_bytes=0
fi
signal_sources=0
if [[ "$serial_log_bytes" -gt 0 ]]; then
  signal_sources=$((signal_sources + 1))
fi
if [[ -n "$metrics_ok_samples_parsed" && "$metrics_ok_samples_parsed" -gt 0 ]]; then
  signal_sources=$((signal_sources + 1))
fi
if [[ -n "$panic_ok_samples_parsed" && "$panic_ok_samples_parsed" -gt 0 ]]; then
  signal_sources=$((signal_sources + 1))
fi

if [[ "$monitor_died_early" -eq 1 ]]; then
  result="FAIL"
fi
if [[ "$serial_wdt_or_panic_count" -gt 0 ]]; then
  result="FAIL"
fi
if [[ -n "$panic_was_crash_true" && "$panic_was_crash_true" -gt 0 ]]; then
  result="FAIL"
fi
if [[ "$METRICS_REQUIRED" -eq 1 ]]; then
  if [[ -z "$metrics_ok_samples_parsed" || "$metrics_ok_samples_parsed" -eq 0 ]]; then
    result="FAIL"
  fi
fi
if [[ "$DISPLAY_DRIVE_ENABLED" -eq 1 ]]; then
  if [[ "$display_drive_calls" -eq 0 ]]; then
    result="FAIL"
  fi
  if [[ -z "$display_updates_delta" ]] || ! [[ "$display_updates_delta" =~ ^-?[0-9]+$ ]]; then
    result="FAIL"
  elif [[ "$display_updates_delta" -lt "$DISPLAY_MIN_UPDATES_DELTA" ]]; then
    result="FAIL"
  fi
fi
if [[ "$result" == "PASS" && "$signal_sources" -eq 0 ]]; then
  result="INCONCLUSIVE"
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
  echo "## Debug API Metrics"
  echo ""
  echo "- Metrics polling configured: $([[ -n "$METRICS_URL" ]] && echo "yes" || echo "no")"
  echo "- Metrics samples (shell): $metrics_samples"
  echo "- Metrics successful (shell): $metrics_ok_samples"
  echo "- Metrics samples parsed: ${metrics_samples_parsed:-0}"
  echo "- Metrics successful parsed: ${metrics_ok_samples_parsed:-0}"
  echo "- Min heapFree: ${heap_free_min:-n/a}"
  echo "- Min heapMinFree: ${heap_min_free_min:-n/a}"
  echo "- Min heapDma: ${heap_dma_min:-n/a}"
  echo "- Min heapDmaLargest: ${heap_dma_largest_min:-n/a}"
  echo "- Peak latencyMaxUs: ${latency_max_peak:-n/a}"
  echo "- Event publish delta: ${event_publish_delta:-n/a}"
  echo "- Event drop delta: ${event_drop_delta:-n/a}"
  echo "- Event size peak: ${event_size_peak:-n/a}"
  echo "- Display updates first/last/delta: ${display_updates_first:-n/a} / ${display_updates_last:-n/a} / ${display_updates_delta:-n/a}"
  echo "- Display skips first/last/delta: ${display_skips_first:-n/a} / ${display_skips_last:-n/a} / ${display_skips_delta:-n/a}"
  echo "- Peak flushMaxUs: ${flush_max_peak:-n/a}"
  echo "- Peak loopMaxUs: ${loop_max_peak:-n/a}"
  echo "- Peak wifiMaxUs: ${wifi_max_peak:-n/a}"
  echo "- Peak bleDrainMaxUs: ${ble_drain_max_peak:-n/a}"
  echo "- Proxy drop peak: ${proxy_drop_peak:-n/a}"
  echo "- Lockout core guard tripped count: ${core_guard_tripped_count:-n/a}"
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
