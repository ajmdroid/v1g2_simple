# V1 Simple Unit Tests

Enterprise-grade test suite for critical components.

## Running Tests

```bash
# Run all native tests
pio test -e native

# Run with verbose output
pio test -e native -v

# Run specific test file
pio test -e native --filter test_haversine
pio test -e native --filter test_display
```

## Test Structure

```
test/
├── mocks/               # Mock headers for ESP32/Arduino types
│   ├── Arduino.h        # Basic Arduino types
│   ├── display_driver.h # Display/graphics mocks
│   ├── settings.h       # Settings manager mock
│   ├── external_deps.h  # BLE/GPS/Battery manager mocks
│   └── freertos/        # FreeRTOS stubs
├── test_haversine/      # GPS distance calculation tests
├── test_packet_parser/  # V1 protocol parsing tests
├── test_display/        # Display system torture tests
└── README.md
```

## Coverage Targets

| Module | Tests | Status |
|--------|-------|--------|
| haversine distance | 10 | ✅ PASS |
| packet parser | 30 | ✅ PASS |
| display system | 65 | ✅ PASS |
| **Total** | **105** | **✅ ALL PASS** |

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
- Priority selection (V1's isPriority flag)
- Card count calculation
- Single alert (no cards)

### Camera Integration (3 tests)
- Camera in main area (no V1 alerts)
- Camera as card (V1 has alerts)
- Distance sorting

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
    if (!hasCameras) {
        display.clearCameraAlerts();  // Called EVERY FRAME!
    }
}
```

**Prevention**: All display functions must have early-exit when state unchanged:

```cpp
void V1Display::clearCameraAlerts() {
    static bool lastHadCameras = false;
    bool hasCameras = (count > 0);
    
    if (hasCameras == lastHadCameras) return;  // Early exit
    
    // ... actual clear logic ...
    lastHadCameras = hasCameras;
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
