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
| display system | 74 | ✅ PASS |
| integration/ownership | 20 | ✅ PASS |
| lockout manager | 32 | ✅ PASS |
| auto-lockout manager | 27 | ✅ PASS |
| settings manager | 15 | ✅ PASS |
| event ring | 15 | ✅ PASS |
| **Total** | **223** | **✅ ALL PASS** |

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

### Test Mode State Machine (9 tests)
Tests display restore behavior after web UI tests (color preview, camera test) end.
**These tests catch the "stuck screen" bug where display didn't return to SCANNING when V1 was disconnected.**

| Test | Scenario | Expected Behavior |
|------|----------|-------------------|
| Color preview ends, V1 disconnected | Test ends while scanning | Show SCANNING (not RESTING!) |
| Color preview ends, V1 connected | Test ends with V1 idle | Show RESTING |
| Color preview ends, V1 has alerts | Test ends with active alert | Show ALERT with data |
| Camera test ends, V1 disconnected | Test ends while scanning | Show SCANNING |
| Camera test ends, V1 connected | Test ends with V1 | Show RESTING or ALERT |
| Ended flags clear | After processing | Flags reset (no infinite loop) |
| V1 disconnects during test | State change mid-test | Uses current state at end |
| V1 connects during test | State change mid-test | Uses current state at end |
| Sequential test modes | Multiple tests | Each restores correctly |

**Key Invariant:**
```
When test mode ends:
  if (v1Connected) → showResting() or update()
  else → showScanning()  // NEVER showResting() when disconnected!
```

## Display Ownership Integration Tests (20 tests) ⭐ NEW

Located in `test/test_integration/test_display_ownership.cpp`.

**Purpose:** Catch bugs where multiple code paths try to manage the same display state, causing flashing or conflicts. This is the exact class of bug that caused camera test flashing when V1 was connected.

### What It Tests

1. **Path Decision Logic** (6 tests) - Verifies correct code path is chosen:
   - No cameras → no display path active
   - Camera test + V1 disconnected → `updateCameraAlerts` owns main area
   - Camera test + V1 connected → `updateCameraCardState` owns cards
   - Real cameras follow same rules

2. **Ownership Conflict Detection** (6 tests) - Catches dual-writer bugs:
   - Only ONE caller should write to camera card state per frame
   - Tests fail if multiple callers write to same state
   - Covers V1 connect/disconnect transitions

3. **Performance Guards** (2 tests):
   - Single flush per frame (multiple flushes = flashing)
   - Force redraw flag not set unconditionally

4. **Color Preview Ownership** (6 tests):
   - Color preview owns main display when active (V1 connected or disconnected)
   - Live data owns main display when preview inactive
   - Ownership transfers cleanly when preview ends
   - Path decision logic for all preview + V1 combinations

### The Pattern Being Enforced

```
Each display element should have ONE owner per frame:

CAMERA CARDS:
- V1 connected: updateCameraCardState() owns camera cards
- V1 disconnected: updateCameraAlerts() owns camera cards
- NEVER both in the same frame!

MAIN DISPLAY:
- Color preview active: preview path owns main display
- Color preview inactive: live data path owns main display
- NEVER both in the same frame!
```

### How to Add New Ownership Tests

When adding a new test mode or display feature:
1. Add the path decision to `getCameraDisplayPath()`
2. Add simulation function like `simulateUpdateCameraAlerts()`
3. Add conflict detection test
4. Run tests - if they fail with "conflict", you have a dual-writer bug

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
