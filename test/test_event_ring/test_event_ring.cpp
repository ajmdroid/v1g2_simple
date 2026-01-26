/**
 * Event Ring Buffer Unit Tests
 * 
 * Tests ring buffer overflow, index wraparound, and event type names.
 * These tests catch bugs where:
 * - Events get lost or duplicated on overflow
 * - Wrong events returned due to index calculation errors
 * - Debug display shows wrong event types
 */

#include <unity.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

// ============================================================================
// MOCK DEFINITIONS
// ============================================================================

// Event types from event_ring.h
enum EventType : uint8_t {
    EVENT_NONE = 0,
    EVENT_BOOT,
    EVENT_V1_CONNECT,
    EVENT_V1_DISCONNECT,
    EVENT_ALERT_START,
    EVENT_ALERT_END,
    EVENT_MUTE,
    EVENT_UNMUTE,
    EVENT_OBD_CONNECT,
    EVENT_OBD_DISCONNECT,
    EVENT_GPS_FIX,
    EVENT_GPS_LOST,
    EVENT_LOCKOUT_ENTER,
    EVENT_LOCKOUT_EXIT,
    EVENT_WIFI_CONNECT,
    EVENT_WIFI_DISCONNECT,
    EVENT_SETTINGS_CHANGE,
    EVENT_ERROR,
    EVENT_TYPE_COUNT  // Sentinel for iteration
};

// Simplified event struct
struct Event {
    uint32_t timestamp;
    EventType type;
    uint32_t data;
};

// Ring buffer constants (power of 2 for fast modulo)
static constexpr size_t RING_SIZE = 256;
static constexpr size_t RING_MASK = RING_SIZE - 1;

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * Ring buffer index calculation
 * Uses power-of-2 mask for fast modulo
 */
size_t ringIndex(size_t head, size_t offset) {
    return (head - offset) & RING_MASK;
}

/**
 * Calculate actual count (capped at ring size)
 */
size_t effectiveCount(size_t totalCount) {
    return (totalCount > RING_SIZE) ? RING_SIZE : totalCount;
}

/**
 * Check if overflow has occurred
 */
bool hasOverflowed(size_t totalCount) {
    return totalCount > RING_SIZE;
}

/**
 * Get event type name (from event_ring.cpp)
 */
const char* eventTypeName(EventType type) {
    switch (type) {
        case EVENT_NONE:            return "NONE";
        case EVENT_BOOT:            return "BOOT";
        case EVENT_V1_CONNECT:      return "V1_CONNECT";
        case EVENT_V1_DISCONNECT:   return "V1_DISCONNECT";
        case EVENT_ALERT_START:     return "ALERT_START";
        case EVENT_ALERT_END:       return "ALERT_END";
        case EVENT_MUTE:            return "MUTE";
        case EVENT_UNMUTE:          return "UNMUTE";
        case EVENT_OBD_CONNECT:     return "OBD_CONNECT";
        case EVENT_OBD_DISCONNECT:  return "OBD_DISCONNECT";
        case EVENT_GPS_FIX:         return "GPS_FIX";
        case EVENT_GPS_LOST:        return "GPS_LOST";
        case EVENT_LOCKOUT_ENTER:   return "LOCKOUT_ENTER";
        case EVENT_LOCKOUT_EXIT:    return "LOCKOUT_EXIT";
        case EVENT_WIFI_CONNECT:    return "WIFI_CONNECT";
        case EVENT_WIFI_DISCONNECT: return "WIFI_DISCONNECT";
        case EVENT_SETTINGS_CHANGE: return "SETTINGS_CHANGE";
        case EVENT_ERROR:           return "ERROR";
        default:                    return "UNKNOWN";
    }
}

/**
 * Simulated ring buffer for testing
 */
class TestEventRing {
public:
    Event events[RING_SIZE];
    size_t head = 0;
    size_t count = 0;
    
    void push(EventType type, uint32_t timestamp, uint32_t data = 0) {
        events[head] = {timestamp, type, data};
        head = (head + 1) & RING_MASK;
        count++;
    }
    
    const Event* get(size_t idx) const {
        if (idx >= effectiveCount(count)) return nullptr;
        size_t ringIdx = ringIndex(head, idx + 1);
        return &events[ringIdx];
    }
    
    size_t getCount() const {
        return effectiveCount(count);
    }
    
    bool hasOverflow() const {
        return hasOverflowed(count);
    }
    
    void reset() {
        head = 0;
        count = 0;
    }
};

// ============================================================================
// TESTS: Ring Index Calculation
// ============================================================================

void test_ringIndex_simple() {
    // Head at 10, offset 1 = index 9
    TEST_ASSERT_EQUAL(9, ringIndex(10, 1));
}

void test_ringIndex_wraparound() {
    // Head at 2, offset 5 = should wrap to 253
    // (2 - 5) & 255 = -3 & 255 = 253
    TEST_ASSERT_EQUAL(253, ringIndex(2, 5));
}

void test_ringIndex_at_zero() {
    // Head at 0, offset 1 = should wrap to 255
    TEST_ASSERT_EQUAL(255, ringIndex(0, 1));
}

void test_ringIndex_full_wrap() {
    // Head at 128, offset 128 = index 0
    TEST_ASSERT_EQUAL(0, ringIndex(128, 128));
}

// ============================================================================
// TESTS: Overflow Detection
// ============================================================================

void test_effectiveCount_under_limit() {
    TEST_ASSERT_EQUAL(0, effectiveCount(0));
    TEST_ASSERT_EQUAL(100, effectiveCount(100));
    TEST_ASSERT_EQUAL(256, effectiveCount(256));
}

void test_effectiveCount_at_overflow() {
    TEST_ASSERT_EQUAL(256, effectiveCount(257));
    TEST_ASSERT_EQUAL(256, effectiveCount(1000));
    TEST_ASSERT_EQUAL(256, effectiveCount(100000));
}

void test_hasOverflowed_false_under_limit() {
    TEST_ASSERT_FALSE(hasOverflowed(0));
    TEST_ASSERT_FALSE(hasOverflowed(100));
    TEST_ASSERT_FALSE(hasOverflowed(256));
}

void test_hasOverflowed_true_over_limit() {
    TEST_ASSERT_TRUE(hasOverflowed(257));
    TEST_ASSERT_TRUE(hasOverflowed(1000));
}

// ============================================================================
// TESTS: Event Type Names
// ============================================================================

void test_eventTypeName_covers_all_types() {
    // Verify all event types have names (not "UNKNOWN")
    for (int i = 0; i < EVENT_TYPE_COUNT; i++) {
        const char* name = eventTypeName((EventType)i);
        TEST_ASSERT_NOT_NULL(name);
        if (i != EVENT_NONE) {
            TEST_ASSERT_TRUE_MESSAGE(
                strcmp(name, "UNKNOWN") != 0,
                "Event type should have a name"
            );
        }
    }
}

void test_eventTypeName_unknown_for_invalid() {
    const char* name = eventTypeName((EventType)99);
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", name);
}

// ============================================================================
// TESTS: Ring Buffer Operations
// ============================================================================

void test_ring_push_and_get() {
    TestEventRing ring;
    
    ring.push(EVENT_BOOT, 1000);
    ring.push(EVENT_V1_CONNECT, 2000);
    ring.push(EVENT_ALERT_START, 3000);
    
    TEST_ASSERT_EQUAL(3, ring.getCount());
    
    // Most recent first (index 0)
    const Event* e0 = ring.get(0);
    TEST_ASSERT_NOT_NULL(e0);
    TEST_ASSERT_EQUAL(EVENT_ALERT_START, e0->type);
    TEST_ASSERT_EQUAL(3000, e0->timestamp);
    
    // Oldest last
    const Event* e2 = ring.get(2);
    TEST_ASSERT_NOT_NULL(e2);
    TEST_ASSERT_EQUAL(EVENT_BOOT, e2->type);
}

void test_ring_overflow_flag() {
    TestEventRing ring;
    
    // Fill exactly
    for (size_t i = 0; i < RING_SIZE; i++) {
        ring.push(EVENT_ALERT_START, i);
    }
    TEST_ASSERT_FALSE(ring.hasOverflow());
    TEST_ASSERT_EQUAL(RING_SIZE, ring.getCount());
    
    // One more triggers overflow
    ring.push(EVENT_ALERT_END, RING_SIZE);
    TEST_ASSERT_TRUE(ring.hasOverflow());
    TEST_ASSERT_EQUAL(RING_SIZE, ring.getCount());  // Still capped at 256
}

void test_ring_overflow_overwrites_oldest() {
    TestEventRing ring;
    
    // Push RING_SIZE events with timestamp = index
    for (size_t i = 0; i < RING_SIZE; i++) {
        ring.push(EVENT_ALERT_START, i);
    }
    
    // Oldest event has timestamp 0
    const Event* oldest = ring.get(RING_SIZE - 1);
    TEST_ASSERT_NOT_NULL(oldest);
    TEST_ASSERT_EQUAL(0, oldest->timestamp);
    
    // Push one more (timestamp = 256)
    ring.push(EVENT_ALERT_END, RING_SIZE);
    
    // Now oldest has timestamp 1 (0 was overwritten)
    oldest = ring.get(RING_SIZE - 1);
    TEST_ASSERT_NOT_NULL(oldest);
    TEST_ASSERT_EQUAL(1, oldest->timestamp);
    
    // Newest has timestamp 256
    const Event* newest = ring.get(0);
    TEST_ASSERT_NOT_NULL(newest);
    TEST_ASSERT_EQUAL(RING_SIZE, newest->timestamp);
}

void test_ring_empty() {
    TestEventRing ring;
    
    TEST_ASSERT_EQUAL(0, ring.getCount());
    TEST_ASSERT_FALSE(ring.hasOverflow());
    TEST_ASSERT_NULL(ring.get(0));
}

void test_ring_out_of_bounds_returns_null() {
    TestEventRing ring;
    ring.push(EVENT_BOOT, 1000);
    
    TEST_ASSERT_NOT_NULL(ring.get(0));  // Valid
    TEST_ASSERT_NULL(ring.get(1));      // Out of bounds
    TEST_ASSERT_NULL(ring.get(100));    // Way out of bounds
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Ring index calculation tests (4 tests)
    RUN_TEST(test_ringIndex_simple);
    RUN_TEST(test_ringIndex_wraparound);
    RUN_TEST(test_ringIndex_at_zero);
    RUN_TEST(test_ringIndex_full_wrap);
    
    // Overflow detection tests (4 tests)
    RUN_TEST(test_effectiveCount_under_limit);
    RUN_TEST(test_effectiveCount_at_overflow);
    RUN_TEST(test_hasOverflowed_false_under_limit);
    RUN_TEST(test_hasOverflowed_true_over_limit);
    
    // Event type name tests (2 tests)
    RUN_TEST(test_eventTypeName_covers_all_types);
    RUN_TEST(test_eventTypeName_unknown_for_invalid);
    
    // Ring buffer operation tests (5 tests)
    RUN_TEST(test_ring_push_and_get);
    RUN_TEST(test_ring_overflow_flag);
    RUN_TEST(test_ring_overflow_overwrites_oldest);
    RUN_TEST(test_ring_empty);
    RUN_TEST(test_ring_out_of_bounds_returns_null);
    
    return UNITY_END();
}
