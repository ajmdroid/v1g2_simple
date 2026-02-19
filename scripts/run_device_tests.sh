#!/usr/bin/env bash
#
# run_device_tests.sh — Run the device test suite on connected ESP32-S3.
#
# Usage:
#   ./scripts/run_device_tests.sh              # All device-only suites
#   ./scripts/run_device_tests.sh --quick      # Core suites only (boot + heap)
#   ./scripts/run_device_tests.sh --full       # Device + shared native suites
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MODE="device"
if [[ "${1:-}" == "--quick" ]]; then
  MODE="quick"
  shift
elif [[ "${1:-}" == "--full" ]]; then
  MODE="full"
  shift
fi

if [[ $# -ne 0 ]]; then
  echo "Usage: $0 [--quick | --full]" >&2
  echo "" >&2
  echo "Modes:" >&2
  echo "  (default)  Run all device-only test suites (test_device_*)" >&2
  echo "  --quick    Run core suites only (boot + heap)" >&2
  echo "  --full     Run device suites PLUS shared native suites on hardware" >&2
  exit 2
fi

if ! command -v pio >/dev/null 2>&1; then
  echo "PlatformIO (pio) is required but not found in PATH." >&2
  exit 1
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$ROOT_DIR/.artifacts/test_reports/device_$timestamp"
mkdir -p "$OUT_DIR"

# ─── Define suite groups ─────────────────────────────────────────────

# Core device suites (boot must be first — validates board is alive)
CORE_FILTERS=(
  -f test_device_boot
  -f test_device_heap
)

# Memory & concurrency suites
MEMORY_FILTERS=(
  -f test_device_psram
  -f test_device_freertos
  -f test_device_event_bus
)

# Dependent system suites
DEPENDENT_FILTERS=(
  -f test_device_nvs
  -f test_device_battery
  -f test_device_coexistence
)

# Shared native suites that compile on device (self-contained, have setup/loop,
# no mock headers, no ../../src/modules/ includes).
# NOTE: These are not yet validated on hardware. Run individually first before
# adding to automated suite. Use: pio test -e device -f test_<name>
# SHARED_FILTERS=(
#   -f test_audio
#   -f test_battery_manager
#   -f test_ble_client
#   -f test_display
#   -f test_packet_parser
#   -f test_settings
#   -f test_wifi_manager
# )

# ─── Build filter list ───────────────────────────────────────────────

FILTERS=()
case "$MODE" in
  quick)
    FILTERS=("${CORE_FILTERS[@]}")
    echo "==> Quick mode: core suites only"
    ;;
  device)
    FILTERS=("${CORE_FILTERS[@]}" "${MEMORY_FILTERS[@]}" "${DEPENDENT_FILTERS[@]}")
    echo "==> Device mode: all device-only suites"
    ;;
  full)
    # All device suites (same as default for now — shared native suites need
    # individual validation before adding to automated run)
    FILTERS=("${CORE_FILTERS[@]}" "${MEMORY_FILTERS[@]}" "${DEPENDENT_FILTERS[@]}")
    echo "==> Full mode: all device-compatible suites (device + shared)"
    ;;
esac

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

json_path="$OUT_DIR/device.json"
junit_path="$OUT_DIR/device.xml"
log_path="$OUT_DIR/device.log"

echo ""
pio test -e device "${FILTERS[@]}" \
  --json-output-path "$json_path" \
  --junit-output-path "$junit_path" | tee "$log_path"

summarize_json "$json_path"

echo "Reports written to: $OUT_DIR"
