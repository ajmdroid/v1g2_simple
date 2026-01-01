#!/bin/bash
# A/B Performance Test Script for V1G2 Simple
# 
# This script automates running the test matrix for latency spike isolation.
# 
# USAGE:
#   ./tools/perf_test.sh <test_name>
#
# Test names:
#   baseline    - All systems enabled (default)
#   no_wifi     - Disable WiFi/WebServer
#   no_touch    - Disable touch handler polling
#   no_battery  - Disable battery manager
#   early_drain - Move queue drain to top of loop
#   no_proxy    - Disable BLE proxy forwarding
#   no_throttle - Disable display throttle

set -e

TEST_NAME="${1:-baseline}"
BUILD_FLAGS=""

case "$TEST_NAME" in
    baseline)
        echo "Test: BASELINE (all systems enabled)"
        BUILD_FLAGS=""
        ;;
    no_wifi)
        echo "Test: DISABLE WIFI"
        BUILD_FLAGS="-DPERF_TEST_DISABLE_WIFI"
        ;;
    no_touch)
        echo "Test: DISABLE TOUCH"
        BUILD_FLAGS="-DPERF_TEST_DISABLE_TOUCH"
        ;;
    no_battery)
        echo "Test: DISABLE BATTERY"
        BUILD_FLAGS="-DPERF_TEST_DISABLE_BATTERY"
        ;;
    early_drain)
        echo "Test: EARLY QUEUE DRAIN"
        BUILD_FLAGS="-DPERF_TEST_EARLY_DRAIN"
        ;;
    no_proxy)
        echo "Test: DISABLE PROXY"
        BUILD_FLAGS="-DPERF_TEST_DISABLE_PROXY"
        ;;
    no_throttle)
        echo "Test: DISABLE THROTTLE"
        BUILD_FLAGS="-DPERF_TEST_DISABLE_THROTTLE"
        ;;
    *)
        echo "Unknown test: $TEST_NAME"
        echo "Valid tests: baseline, no_wifi, no_touch, no_battery, early_drain, no_proxy, no_throttle"
        exit 1
        ;;
esac

echo ""
echo "Building with flags: $BUILD_FLAGS"
echo ""

# Add build flags to platformio.ini temporarily
if [ -n "$BUILD_FLAGS" ]; then
    # Backup and modify
    cp platformio.ini platformio.ini.bak
    sed -i '' "s/build_flags = /build_flags = $BUILD_FLAGS /" platformio.ini
fi

# Build
pio run

# Restore platformio.ini
if [ -f platformio.ini.bak ]; then
    mv platformio.ini.bak platformio.ini
fi

echo ""
echo "Build complete. To flash and monitor:"
echo "  pio run -t upload && pio device monitor -b 115200"
echo ""
echo "Run test scenario for 2 minutes, then look for:"
echo "  ========== PERF TEST RESULTS =========="
echo ""
