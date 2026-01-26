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
├── unit/             # Unit test files
│   ├── test_haversine.cpp
│   ├── test_packet_parser.cpp
│   └── test_lockout_matching.cpp
└── README.md
```

## Coverage Targets

| Module | Target | Priority |
|--------|--------|----------|
| haversine distance | 100% | HIGH |
| packet parser | 80% | HIGH |
| lockout matching | 90% | HIGH |
| frequency tolerance | 100% | MEDIUM |

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
