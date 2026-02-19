#!/usr/bin/env bash
#
# run_device_tests.sh — Run the device test suite on connected ESP32-S3.
#
# Usage:
#   ./scripts/run_device_tests.sh              # All device-only suites (safe set)
#   ./scripts/run_device_tests.sh --quick      # Core suites only (boot + heap safe)
#   ./scripts/run_device_tests.sh --full       # Device + shared native suites (safe set)
#   ./scripts/run_device_tests.sh --stress     # Include stress suites
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MODE="device"
RUN_STRESS=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick)
      MODE="quick"
      ;;
    --full)
      MODE="full"
      ;;
    --stress)
      RUN_STRESS=1
      ;;
    -h|--help)
      echo "Usage: $0 [--quick | --full] [--stress]"
      echo ""
      echo "Modes:"
      echo "  (default)  Run all device-only test suites (safe set)"
      echo "  --quick    Run core suites only (boot + heap safe)"
      echo "  --full     Run device suites PLUS shared native suites (safe set)"
      echo "Options:"
      echo "  --stress   Include stress suites (can destabilize fragile hardware)"
      exit 0
      ;;
    *)
      echo "Usage: $0 [--quick | --full] [--stress]" >&2
      echo "" >&2
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
  shift
done

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO (pio) is required but not found in PATH." >&2
  exit 1
fi

PIO_TEST_VERBOSITY="${PIO_TEST_VERBOSITY:--vvv}"

# If DEVICE_PORT is set, keep using that exact path.
TEST_PORT="${DEVICE_PORT:-}"
PORT_LOCKED=0
if [[ -n "$TEST_PORT" ]]; then
  PORT_LOCKED=1
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

resolve_test_port() {
  if [[ "$PORT_LOCKED" -eq 1 ]]; then
    echo "$TEST_PORT"
    return 0
  fi

  detect_usb_port || return 1
}

ensure_port_unlocked() {
  local port="$1"

  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi

  local pids
  pids="$(lsof -t "$port" 2>/dev/null | tr '\n' ' ' | xargs || true)"
  if [[ -n "$pids" ]]; then
    echo "Serial port '$port' is currently in use by another process." >&2
    echo "Close the serial monitor (or kill the process) and retry." >&2
    echo "Port owner(s):" >&2
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
  echo "No USB serial device detected for hardware tests." >&2
  echo "Connect the ESP32-S3 in bootloader mode and retry." >&2
  echo "Or set DEVICE_PORT explicitly, for example:" >&2
  echo "  DEVICE_PORT=/dev/cu.usbmodemXXXX ./scripts/run_device_tests.sh --quick" >&2
  exit 1
fi

if [[ "$PORT_LOCKED" -eq 1 ]]; then
  echo "==> Using fixed device port: $TEST_PORT"
else
  echo "==> Auto-detected initial device port: $TEST_PORT"
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$ROOT_DIR/.artifacts/test_reports/device_$timestamp"
mkdir -p "$OUT_DIR"
MAIN_LOG="$OUT_DIR/device.log"
: > "$MAIN_LOG"

# ─── Define suite groups ─────────────────────────────────────────────

# Core device suites (boot must be first — validates board is alive)
CORE_SUITES=(
  test_device_boot
  test_device_heap
)

# Memory & concurrency suites
MEMORY_SUITES=(
  test_device_psram
  test_device_freertos
  test_device_event_bus
)

# Dependent system suites
DEPENDENT_SUITES=(
  test_device_nvs
  test_device_battery
  test_device_coexistence
)

# Stress suites (run only with --stress)
STRESS_SUITES=(
  test_device_heap_stress
)

# Shared native suites that compile on device (self-contained, have setup/loop,
# no mock headers, no ../../src/modules/ includes).
# NOTE: These are not yet validated on hardware. Run individually first before
# adding to automated suite. Use: pio test -e device -f test_<name>
# SHARED_SUITES=(
#   test_audio
#   test_battery_manager
#   test_ble_client
#   test_display
#   test_packet_parser
#   test_settings
#   test_wifi_manager
# )

# ─── Build suite list ────────────────────────────────────────────────

SUITES=()
case "$MODE" in
  quick)
    SUITES=("${CORE_SUITES[@]}")
    echo "==> Quick mode: core suites only (safe)"
    ;;
  device)
    SUITES=("${CORE_SUITES[@]}" "${MEMORY_SUITES[@]}" "${DEPENDENT_SUITES[@]}")
    echo "==> Device mode: all device-only suites (safe)"
    ;;
  full)
    # All device suites (same as default for now — shared native suites need
    # individual validation before adding to automated run)
    SUITES=("${CORE_SUITES[@]}" "${MEMORY_SUITES[@]}" "${DEPENDENT_SUITES[@]}")
    echo "==> Full mode: all device-compatible suites (safe)"
    ;;
esac

if [[ "$RUN_STRESS" -eq 1 ]]; then
  SUITES=("${SUITES[@]}" "${STRESS_SUITES[@]}")
  echo "==> Stress mode enabled: adding stress suites"
fi

echo "==> Planned suite order:"
for suite in "${SUITES[@]}"; do
  echo "    - $suite"
done

# ─── Summarize JSON results ──────────────────────────────────────────

summarize_json() {
  local json_path="$1"
  python3 - "device" "$json_path" <<'PY'
import json
import sys

env_name = sys.argv[1]
json_path = sys.argv[2]

with open(json_path, "r", encoding="utf-8") as f:
    data = json.load(f)

suites = [
    s for s in data.get("test_suites", [])
    if s.get("env_name") == env_name and s.get("status") != "SKIPPED"
]

suite_count = len(suites)
test_count = sum(int(s.get("testcase_nums", 0)) for s in suites)
failure_count = sum(int(s.get("failure_nums", 0)) for s in suites)
error_count = sum(int(s.get("error_nums", 0)) for s in suites)
duration_s = sum(float(s.get("duration", 0.0)) for s in suites)

status = "PASS" if (failure_count + error_count) == 0 else "FAIL"

print(f"\n{'='*60}")
print(f"  Device Test Summary: {status}")
print(f"  Suites: {suite_count}  Tests: {test_count}"
      f"  Failures: {failure_count}  Errors: {error_count}")
print(f"  Duration: {duration_s:.3f}s")
print(f"{'='*60}\n")

if failure_count > 0 or error_count > 0:
    for s in suites:
        if int(s.get("failure_nums", 0)) > 0 or int(s.get("error_nums", 0)) > 0:
            print(f"  FAILED: {s.get('test_suite_name', 'unknown')}")
    print()
    sys.exit(1)
PY
}

# ─── Run tests ────────────────────────────────────────────────────────

run_suite() {
  local suite="$1"
  local index="$2"
  local total="$3"
  local run_port

  # After a previous test, give USB CDC time to stabilize before re-detecting
  if (( index > 1 )); then
    echo "    Waiting for USB CDC port to stabilize..."
    sleep 3
  fi

  # Re-detect port each time (USB CDC may change name after reset)
  run_port="$(resolve_test_port || true)"
  if [[ -z "$run_port" ]]; then
    # Port not found immediately — wait up to 15s for it to reappear
    echo "    Port not found, waiting up to 15s for USB CDC re-enumeration..."
    local waited=0
    while (( waited < 15 )); do
      sleep 1
      waited=$((waited + 1))
      run_port="$(detect_usb_port || true)"
      if [[ -n "$run_port" ]]; then
        break
      fi
    done
    if [[ -z "$run_port" ]]; then
      echo "Unable to locate a USB serial device before suite '$suite'." >&2
      echo "Device may need manual reset (BOOT+RESET buttons)." >&2
      return 1
    fi
  fi

  if ! wait_for_port "$run_port" 20; then
    echo "Timed out waiting for port '$run_port' before suite '$suite'." >&2
    return 1
  fi

  if ! ensure_port_unlocked "$run_port"; then
    return 1
  fi

  echo ""
  echo "==> [$index/$total] Running $suite on $run_port"

  local suite_json="$OUT_DIR/${suite}.json"
  local suite_xml="$OUT_DIR/${suite}.xml"
  local suite_log="$OUT_DIR/${suite}.log"

  set +e
  pio test -e device -f "$suite" \
    --upload-port "$run_port" \
    --test-port "$run_port" \
    --json-output-path "$suite_json" \
    --junit-output-path "$suite_xml" "$PIO_TEST_VERBOSITY" | tee "$suite_log"
  local cmd_status=${PIPESTATUS[0]}
  set -e

  cat "$suite_log" >> "$MAIN_LOG"

  if [[ $cmd_status -ne 0 ]]; then
    echo "" >&2
    echo "Suite '$suite' failed (exit $cmd_status)." >&2
    echo "Last 40 log lines:" >&2
    tail -n 40 "$suite_log" >&2 || true
    return "$cmd_status"
  fi

  summarize_json "$suite_json"
}

failed_suite=""
total_suites="${#SUITES[@]}"
suite_index=1

for suite in "${SUITES[@]}"; do
  if ! run_suite "$suite" "$suite_index" "$total_suites"; then
    failed_suite="$suite"
    break
  fi
  suite_index=$((suite_index + 1))
done

if [[ -n "$failed_suite" ]]; then
  echo "" >&2
  echo "Device test run stopped at: $failed_suite" >&2
  echo "Reports written to: $OUT_DIR"
  exit 1
fi

echo ""
echo "All requested device suites passed."
echo "Reports written to: $OUT_DIR"
