#!/bin/bash
#
# V1 Gen2 Monitored Test Runner
# 
# Runs firmware build, captures serial output, parses metrics, and validates thresholds.
#
# Usage:
#   ./run_monitored_tests.sh                  # Build + capture 30s + validate
#   ./run_monitored_tests.sh --upload         # Build + upload + capture + validate
#   ./run_monitored_tests.sh --duration 60    # Capture for 60 seconds
#   ./run_monitored_tests.sh --scenario proxy # Run proxy load test
#
# Exit codes:
#   0 = All tests passed
#   1 = Metrics threshold exceeded
#   2 = Build failed
#   3 = Upload failed
#   4 = Serial capture failed

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
OUTPUT_DIR="$PROJECT_DIR/test_results"
DURATION=30
UPLOAD=false
SCENARIO="stream"
PORT=""
BAUD=115200
VERBOSE=false

# Thresholds (can be overridden via environment)
MAX_LATENCY_US=${MAX_LATENCY_US:-100000}    # 100ms
MAX_DROP_RATE=${MAX_DROP_RATE:-0.01}        # 1%

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    cat << EOF
V1 Gen2 Monitored Test Runner

Usage: $(basename "$0") [OPTIONS]

Options:
  --upload, -u          Upload firmware before testing
  --duration, -d SEC    Capture duration in seconds (default: 30)
  --scenario, -s NAME   Test scenario: stream, proxy, reconnect (default: stream)
  --port, -p PORT       Serial port (auto-detect if not specified)
  --baud, -b RATE       Baud rate (default: 115200)
  --verbose, -v         Show serial output during capture
  --help, -h            Show this help

Scenarios:
  stream     - Normal streaming test (connect and monitor)
  proxy      - Proxy load test (requires JBV1 connection)
  reconnect  - Reconnection stress test

Environment variables:
  MAX_LATENCY_US  - Maximum allowed latency in microseconds (default: 100000)
  MAX_DROP_RATE   - Maximum allowed packet drop rate (default: 0.01)

Examples:
  $(basename "$0") --upload --duration 60
  $(basename "$0") --scenario proxy -d 120
  MAX_LATENCY_US=50000 $(basename "$0")
EOF
}

log() {
    echo -e "${GREEN}[TEST]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --upload|-u)
            UPLOAD=true
            shift
            ;;
        --duration|-d)
            DURATION="$2"
            shift 2
            ;;
        --scenario|-s)
            SCENARIO="$2"
            shift 2
            ;;
        --port|-p)
            PORT="$2"
            shift 2
            ;;
        --baud|-b)
            BAUD="$2"
            shift 2
            ;;
        --verbose|-v)
            VERBOSE=true
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            error "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Create output directory
mkdir -p "$OUTPUT_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="$OUTPUT_DIR/test_${SCENARIO}_${TIMESTAMP}.log"
METRICS_FILE="$OUTPUT_DIR/metrics_${SCENARIO}_${TIMESTAMP}.json"

log "Starting monitored test: scenario=$SCENARIO, duration=${DURATION}s"
log "Output: $LOG_FILE"

# Auto-detect serial port if not specified
if [ -z "$PORT" ]; then
    if [ "$(uname)" == "Darwin" ]; then
        # macOS
        PORT=$(ls /dev/cu.usbserial* /dev/cu.usbmodem* 2>/dev/null | head -1)
    else
        # Linux
        PORT=$(ls /dev/ttyUSB* /dev/ttyACM* 2>/dev/null | head -1)
    fi
    
    if [ -z "$PORT" ]; then
        error "No serial port detected. Connect device or specify --port"
        exit 4
    fi
    log "Auto-detected port: $PORT"
fi

# Build firmware
log "Building firmware..."
cd "$PROJECT_DIR"
if ! pio run 2>&1 | tee "$OUTPUT_DIR/build_${TIMESTAMP}.log"; then
    error "Build failed! See $OUTPUT_DIR/build_${TIMESTAMP}.log"
    exit 2
fi
log "Build successful"

# Upload if requested
if [ "$UPLOAD" = true ]; then
    log "Uploading firmware..."
    if ! pio run -t upload 2>&1 | tee -a "$OUTPUT_DIR/build_${TIMESTAMP}.log"; then
        error "Upload failed!"
        exit 3
    fi
    log "Upload successful"
    log "Waiting for device to boot..."
    sleep 3
fi

# Capture serial output
log "Capturing serial output for ${DURATION}s..."
log "Port: $PORT, Baud: $BAUD"

# Use timeout with stty for cross-platform serial capture
capture_serial() {
    if command -v timeout &> /dev/null; then
        TIMEOUT_CMD="timeout"
    elif command -v gtimeout &> /dev/null; then
        TIMEOUT_CMD="gtimeout"  # macOS with coreutils
    else
        # Fallback: use background process with sleep
        (
            stty -f "$PORT" $BAUD raw -echo 2>/dev/null || stty -F "$PORT" $BAUD raw -echo
            cat "$PORT" &
            CAT_PID=$!
            sleep "$DURATION"
            kill $CAT_PID 2>/dev/null
        ) > "$LOG_FILE"
        return
    fi
    
    stty -f "$PORT" $BAUD raw -echo 2>/dev/null || stty -F "$PORT" $BAUD raw -echo
    $TIMEOUT_CMD "${DURATION}s" cat "$PORT" > "$LOG_FILE" 2>/dev/null || true
}

if [ "$VERBOSE" = true ]; then
    capture_serial | tee "$LOG_FILE"
else
    capture_serial
fi

# Check if we got any output
if [ ! -s "$LOG_FILE" ]; then
    warn "No serial output captured. Device may not be running."
fi

log "Serial capture complete: $(wc -l < "$LOG_FILE") lines"

# Parse metrics
log "Parsing metrics..."
METRICS_COUNT=$(grep -c '\[METRICS\]' "$LOG_FILE" 2>/dev/null || echo "0")
log "Found $METRICS_COUNT metrics samples"

if [ "$METRICS_COUNT" -eq 0 ]; then
    warn "No metrics found in log. Enable debug mode: curl -X POST http://<device>/api/debug/enable?enable=true"
fi

# Run metrics parser
cd "$SCRIPT_DIR"
if python3 parse_metrics.py "$LOG_FILE" --json --max-latency "$MAX_LATENCY_US" --max-drops "$MAX_DROP_RATE" > "$METRICS_FILE" 2>&1; then
    PASSED=true
else
    PASSED=false
fi

# Print summary
echo ""
echo "=========================================="
echo "Test Results: $SCENARIO"
echo "=========================================="
python3 parse_metrics.py "$LOG_FILE" --max-latency "$MAX_LATENCY_US" --max-drops "$MAX_DROP_RATE"

# Final status
echo ""
if [ "$PASSED" = true ]; then
    log "✓ All thresholds passed!"
    exit 0
else
    error "✗ Threshold exceeded!"
    exit 1
fi
