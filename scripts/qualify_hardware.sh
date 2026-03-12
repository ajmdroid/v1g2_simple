#!/usr/bin/env bash
#
# qualify_hardware.sh - Authoritative single-board hardware qualification gate.
#
# Fixed sequence:
#   1) Build firmware + filesystem images
#   2) Flash real firmware + filesystem to the connected board
#   3) Verify metrics endpoint is reachable
#   4) Run the full device test suite
#   5) Run a 300s real-firmware telemetry soak (drive_wifi_ap)
#   6) Run a 300s display-preview telemetry soak
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

if [[ "${WORKSPACE_CLEANED:-0}" != "1" ]]; then
  echo "==> cleanup_workspace"
  python3 scripts/clean_workspace.py --safe --apply
  echo "[pass] cleanup_workspace"
  export WORKSPACE_CLEANED=1
fi

ENV_NAME="waveshare-349"
METRICS_URL="${REAL_FW_METRICS_URL:-http://192.168.35.5/api/debug/metrics}"
HTTP_TIMEOUT_SECONDS="${REAL_FW_HTTP_TIMEOUT_SECONDS:-5}"
DEVICE_PORT_OVERRIDE="${DEVICE_PORT:-}"
BOARD_ID=""

timestamp="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$ROOT_DIR/.artifacts/test_reports/qualification_${timestamp}"
mkdir -p "$OUT_DIR"

usage() {
  cat <<'EOF'
Usage: ./scripts/qualify_hardware.sh [options]

Options:
  --board-id ID           Resolve port/metrics from test/device/board_inventory.json
  --metrics-url URL       Metrics endpoint to require (default: env or 192.168.35.5)
  --port PATH             Fixed serial port (default: DEVICE_PORT or auto-detect)
  --env NAME              PlatformIO environment (default: waveshare-349)
  --out-dir PATH          Qualification artifact directory
  --http-timeout-seconds N
                          curl timeout for metrics precheck (default: 5)
  -h, --help              Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --board-id)
      BOARD_ID="$2"
      shift
      ;;
    --metrics-url)
      METRICS_URL="$2"
      shift
      ;;
    --port)
      DEVICE_PORT_OVERRIDE="$2"
      shift
      ;;
    --env)
      ENV_NAME="$2"
      shift
      ;;
    --out-dir)
      OUT_DIR="$2"
      shift
      ;;
    --http-timeout-seconds)
      HTTP_TIMEOUT_SECONDS="$2"
      shift
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

mkdir -p "$OUT_DIR"
SUMMARY_LOG="$OUT_DIR/qualification.log"
: > "$SUMMARY_LOG"

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

ensure_port_unlocked() {
  local port="$1"
  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi

  local pids
  pids="$(lsof -t "$port" 2>/dev/null | tr '\n' ' ' | xargs || true)"
  if [[ -n "$pids" ]]; then
    echo "Serial port '$port' is in use by another process." >&2
    ps -o pid=,command= -p $pids >&2 || true
    return 1
  fi

  return 0
}

run_step() {
  local step_name="$1"
  shift
  local log_path="$OUT_DIR/${step_name}.log"

  echo "==> ${step_name}" | tee -a "$SUMMARY_LOG"
  if "$@" >"$log_path" 2>&1; then
    echo "[pass] ${step_name}" | tee -a "$SUMMARY_LOG"
  else
    echo "[fail] ${step_name} (see ${log_path})" | tee -a "$SUMMARY_LOG"
    tail -n 40 "$log_path" >&2 || true
    exit 1
  fi
}

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO (pio) is required but not found in PATH." >&2
  exit 1
fi

# Resolve board-id from inventory if specified
if [[ -n "$BOARD_ID" ]]; then
  INVENTORY="$ROOT_DIR/test/device/board_inventory.json"
  if [[ ! -f "$INVENTORY" ]]; then
    echo "Board inventory not found: $INVENTORY" >&2
    exit 2
  fi
  BOARD_INFO="$(python3 -c "
import json, sys
with open('$INVENTORY') as f:
    inv = json.load(f)
for b in inv['boards']:
    if b['board_id'] == '$BOARD_ID':
        print(b['device_path'])
        print(b['metrics_url'])
        sys.exit(0)
print('', file=sys.stderr)
sys.exit(1)
" 2>/dev/null)" || {
    echo "Board '$BOARD_ID' not found in inventory." >&2
    exit 2
  }
  BOARD_PORT="$(echo "$BOARD_INFO" | head -n1)"
  BOARD_METRICS="$(echo "$BOARD_INFO" | tail -n1)"
  if [[ -z "$DEVICE_PORT_OVERRIDE" ]]; then
    DEVICE_PORT_OVERRIDE="$BOARD_PORT"
  fi
  if [[ "$METRICS_URL" == "http://192.168.35.5/api/debug/metrics" ]]; then
    METRICS_URL="$BOARD_METRICS"
  fi
  echo "board_id=${BOARD_ID}" | tee -a "$SUMMARY_LOG"
fi

DEVICE_PORT_RESOLVED="$DEVICE_PORT_OVERRIDE"
if [[ -z "$DEVICE_PORT_RESOLVED" ]]; then
  DEVICE_PORT_RESOLVED="$(detect_usb_port || true)"
fi
if [[ -z "$DEVICE_PORT_RESOLVED" ]]; then
  echo "No USB serial device detected for qualification." >&2
  echo "Connect the ESP32-S3 and retry, or set DEVICE_PORT/--port explicitly." >&2
  exit 1
fi

ensure_port_unlocked "$DEVICE_PORT_RESOLVED"
echo "device_port=${DEVICE_PORT_RESOLVED}" | tee -a "$SUMMARY_LOG"
echo "metrics_url=${METRICS_URL}" | tee -a "$SUMMARY_LOG"

run_step build_firmware \
  pio run -e "$ENV_NAME" -j 1

run_step build_filesystem \
  pio run -e "$ENV_NAME" -t buildfs -j 1

run_step flash_filesystem \
  pio run -e "$ENV_NAME" -t uploadfs --upload-port "$DEVICE_PORT_RESOLVED" -j 1

run_step flash_firmware \
  pio run -e "$ENV_NAME" -t upload --upload-port "$DEVICE_PORT_RESOLVED" -j 1

run_step metrics_precheck \
  curl -fsS --max-time "$HTTP_TIMEOUT_SECONDS" "$METRICS_URL"

if ! grep -q '{' "$OUT_DIR/metrics_precheck.log"; then
  echo "Metrics precheck did not return JSON." >&2
  echo "Enable the setup AP and verify the metrics endpoint before qualification." >&2
  exit 1
fi

run_step device_tests \
  env DEVICE_PORT="$DEVICE_PORT_RESOLVED" DEVICE_BOARD_ID="${BOARD_ID:-unknown}" ./scripts/run_device_tests.sh --full

run_step soak_core \
  env DEVICE_PORT="$DEVICE_PORT_RESOLVED" DEVICE_BOARD_ID="${BOARD_ID:-unknown}" ./scripts/run_real_fw_soak.sh \
    --duration-seconds 300 \
    --with-fs \
    --metrics-url "$METRICS_URL" \
    --require-metrics \
    --profile drive_wifi_ap \
    --out-dir "$OUT_DIR/soak_core"

run_step soak_display_preview \
  env DEVICE_PORT="$DEVICE_PORT_RESOLVED" DEVICE_BOARD_ID="${BOARD_ID:-unknown}" ./scripts/run_real_fw_soak.sh \
    --skip-flash \
    --duration-seconds 300 \
    --metrics-url "$METRICS_URL" \
    --require-metrics \
    --profile drive_wifi_ap \
    --drive-display-preview \
    --out-dir "$OUT_DIR/soak_display_preview"

cat <<EOF | tee -a "$SUMMARY_LOG"
qualification_result=PASS
artifacts_dir=$OUT_DIR
EOF

echo "Hardware qualification passed. Artifacts: $OUT_DIR"
