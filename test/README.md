# V1 Simple Unit Tests

Enterprise-grade test suite for critical components.

## Running Tests

```bash
# Run all native tests
pio test -e native

# Run focused functional scenarios (integration + behavior checks)
./scripts/run_functional_tests.sh

# Run the same functional scenarios on hardware, too
./scripts/run_functional_tests.sh --with-device

# Run with verbose output
pio test -e native -v

# Run specific test suites
pio test -e native --filter test_display
pio test -e native --filter test_packet_parser
pio test -e native --filter test_drive_scenario
```

## Device Test Suite

Hardware-specific tests that find memory, coexistence, and concurrency issues
before production. Requires ESP32-S3 connected via USB.

```bash
# All device-only suites (boot → heap → PSRAM → RTOS → NVS → battery → radio)
./scripts/run_device_tests.sh

# Quick sanity (boot + heap only)
./scripts/run_device_tests.sh --quick

# Full run (device suites + shared native suites on hardware)
./scripts/run_device_tests.sh --full

# Repeat device test firmware cycles and collect flake metrics (CSV + summary)
./scripts/run_device_soak.sh --cycles 20 --cooldown-seconds 6

# Flash REAL production firmware (waveshare-349) and soak runtime behavior
./scripts/run_real_fw_soak.sh --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics

# Real display-path lag check (forces preview redraw during soak)
./scripts/run_real_fw_soak.sh --skip-flash --duration-seconds 900 \
  --metrics-url http://192.168.35.5/api/debug/metrics \
  --require-metrics --min-metrics-ok-samples 50 \
  --drive-display-preview --display-drive-interval-seconds 6 \
  --min-display-updates-delta 100

# Individual suite
pio test -e device --filter test_device_heap
```

`run_real_fw_soak.sh` validates the normal app image (not UNIT_TEST firmware).
If you provide `--metrics-url`, enable the setup AP first (default URL assumes
`http://192.168.35.5`). If no telemetry is captured, the run is marked
`INCONCLUSIVE` (exit code `2`) instead of reporting a false pass.
Use `--drive-display-preview` when you specifically need display-path coverage.

### Device Suites

| Suite | Category | What it catches |
|-------|----------|-----------------|
| `test_device_boot` | Core / System | Post-boot baseline, CPU/PSRAM detection, flash/partition |
| `test_device_heap` | Core / Memory | Internal SRAM leaks, fragmentation, OOM resilience |
| `test_device_psram` | Core / Memory | PSRAM detection, 4 MB pattern-verify, write-speed sanity |
| `test_device_freertos` | Core / RTOS | Queue overflow, semaphore, cross-task communication |
| `test_device_event_bus` | Core / Concurrency | SystemEventBus under real portMUX across cores |
| `test_device_nvs` | Dependent / Persistence | NVS write/read round-trip, namespace A/B, XOR obfuscation |
| `test_device_battery` | Dependent / Hardware | ADC sampling, TCA9554 I2C, power latch, button GPIO |
| `test_device_coexistence` | Dependent / Radio | WiFi AP heap cost, DMA gate, BLE+WiFi simultaneous |
| `test_device_heap_stress` | Stress | Fragmentation churn, alloc/free leak checks, near-OOM (manual run) |

See [device/README.md](device/README.md) for detailed documentation.

## Functional Test Gate

Use `./scripts/run_functional_tests.sh` when you want behavior-level coverage
in addition to broad `native` unit coverage. The functional gate runs:

- `test_drive_scenario` (cross-module drive flows)
- `test_lockout_enforcer` (enforcement decisions)
- `test_wifi_boot_policy` (WiFi startup gating)
- `test_wifi_manager` (WiFi state behavior)

Each run writes machine-readable reports to:

- `.artifacts/test_reports/functional_<timestamp>/native.json`
- `.artifacts/test_reports/functional_<timestamp>/native.xml`
- `.artifacts/test_reports/functional_<timestamp>/native.log`

## Test Structure

```
test/
├── device/              # Device test documentation
│   └── README.md
├── fixtures/            # Real-world logs, packet captures, and replay data
├── mocks/               # Mock headers for ESP32/Arduino types
│   ├── Arduino.h        # Basic Arduino types
│   ├── display_driver.h # Display/graphics mocks
│   ├── settings.h       # Settings manager mock
│   ├── battery_manager.h # Battery manager mock
│   ├── ble_client.h     # BLE client mock
│   └── freertos/        # FreeRTOS stubs
├── test_device_boot/    # [DEVICE] System baseline + chip detection
├── test_device_heap/    # [DEVICE] Heap fragmentation + leak detection
├── test_device_psram/   # [DEVICE] PSRAM integrity + write speed
├── test_device_freertos/ # [DEVICE] Queue/semaphore/cross-task
├── test_device_event_bus/ # [DEVICE] Event bus concurrency
├── test_device_nvs/     # [DEVICE] NVS persistence round-trip
├── test_device_battery/ # [DEVICE] ADC + I2C hardware
├── test_device_coexistence/ # [DEVICE] BLE/WiFi radio coexistence
├── test_alert_persistence/
├── test_display/        # Display system torture tests
├── test_drive_scenario/ # Replay-driven integration tests
├── test_packet_parser/  # V1 protocol parsing tests
├── test_wifi_manager/
├── ...                  # Additional test_* suites
└── README.md
```

## Current Baseline

| Metric | Native | Device |
|--------|--------|--------|
| Test suites | 76 | 9 (device-only) |
| Test cases | 939 | ~90 |
| Result | ✅ 939 passed | Compile-verified |
| Command | `pio test -e native` | `./scripts/run_device_tests.sh` |

## Display Torture Test Categories

The `test_display` suite comprehensively tests the display system:

### Band/Direction Decoding (11 tests)
- Band priority (Laser > Ka > K > X)
- Direction bitmap parsing
- Multi-direction support

### Frequency Tolerance (4 tests)
- ±5 MHz tolerance prevents jitter redraws
- Force flag overrides tolerance
- Zero-to-nonzero detection

### Cache Invalidation (4 tests)
- `drawBaseFrame()` sets all force flags
- No redraw when state unchanged
- State changes trigger redraw
- Force flags clear after draw

### Component Caching (12 tests)
- Band indicator caching
- Arrow direction caching
- Signal bars caching
- Secondary card caching
- Mute state transitions

### State Transitions (2 tests)
- Resting to alert
- Alert to muted

### Multi-Alert Scenarios (3 tests)
- Priority selection (alert-row priority bit with first-usable fallback)
- Card count calculation
- Single alert (no cards)

### Display State (2 tests)
- Default values
- Volume support detection

### Boundary Conditions (4 tests)
- Frequency min/max ranges
- Signal strength clamping (0-6)
- Brightness range (0-255)
- Volume range (0-9)

### Stress Tests (6 tests)
- Rapid frequency changes within tolerance
- Rapid frequency changes beyond tolerance
- Rapid direction changes
- Rapid band changes
- Alternating mute state
- Full screen clear cycles

### Bogey Counter (4 tests)
- All digits 0-9
- Special chars (J, L, P, A, #)
- Decimal point detection
- Unknown patterns

### Alert Data (5 tests)
- Equality comparison
- Different band/direction/frequency/strength

### Color Helpers (3 tests)
- Band color mapping
- Muted state overrides
- BAND_NONE handling

### Layout (2 tests)
- Screen dimensions (640×172)
- Primary/secondary zone fit

### Test Mode State Machine (6 tests)
Tests display restore behavior after web UI tests (color preview) end.
**These tests catch the "stuck screen" bug where display didn't return to SCANNING when V1 was disconnected.**

| Test | Scenario | Expected Behavior |
|------|----------|-------------------|
| Color preview ends, V1 disconnected | Test ends while scanning | Show SCANNING (not RESTING!) |
| Color preview ends, V1 connected | Test ends with V1 idle | Show RESTING |
| Color preview ends, V1 has alerts | Test ends with active alert | Show ALERT with data |
| Ended flags clear | After processing | Flags reset (no infinite loop) |
| V1 disconnects during test | State change mid-test | Uses current state at end |
| V1 connects during test | State change mid-test | Uses current state at end |

**Key Invariant:**
```
When test mode ends:
  if (v1Connected) → showResting() or update()
  else → showScanning()  // NEVER showResting() when disconnected!
```

## Writing Tests

Tests use the Unity framework. Example:

```cpp
#include <unity.h>

void test_example() {
    TEST_ASSERT_EQUAL(42, someFunction());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_example);
    return UNITY_END();
}
```

## Known Bug Patterns to Test Against

### Display Flashing Bug Pattern

The most common display bug is calling a display function every frame without change detection:

```cpp
// ❌ BUG PATTERN - causes flashing
void loop() {
    if (!hasSecondaryAlerts) {
        display.clearSecondaryCards();  // Called EVERY FRAME!
    }
}
```

**Prevention**: All display functions must have early-exit when state unchanged:

```cpp
void V1Display::clearSecondaryCards() {
    static bool lastHadCards = false;
    bool hasCards = (count > 0);
    
    if (hasCards == lastHadCards) return;  // Early exit
    
    // ... actual clear logic ...
    lastHadCards = hasCards;
}
```

### Frequency Jitter Pattern

V1 frequency can jitter ±1-5 MHz between packets. Never use exact equality:

```cpp
// ❌ BUG PATTERN - causes constant redraws
if (priority.frequency != lastFrequency) {
    needsRedraw = true;
}

// ✅ CORRECT - use tolerance
if (abs(priority.frequency - lastFrequency) > 5) {
    needsRedraw = true;
}
```

See `test/test_display/test_display.cpp` for comprehensive frequency tolerance tests.

## Test Philosophy

1. **Test behavior, not implementation** - Tests verify what the display *should do*, not *how* it does it
2. **Torture test edge cases** - Every boundary condition, every state transition
3. **Prevent regression** - Each fixed bug gets a test to ensure it never returns
4. **Document invariants** - Tests serve as executable documentation
