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
```

## Test Structure

```
test/
├── mocks/            # Mock headers for ESP32/Arduino types
│   ├── Arduino.h     # Basic Arduino types
│   └── freertos/     # FreeRTOS stubs
├── test_haversine/   # GPS distance calculation tests
├── test_packet_parser/  # V1 protocol parsing tests
└── README.md
```

## Coverage Targets

| Module | Tests | Status |
|--------|-------|--------|
| haversine distance | 10 | ✅ PASS |
| packet parser | 30 | ✅ PASS |
| frequency tolerance | 4 | ✅ PASS |

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

See `test/test_packet_parser/test_packet_parser.cpp` for frequency tolerance tests.
