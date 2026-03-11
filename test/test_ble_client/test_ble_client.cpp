/**
 * BLE client header-level tests.
 *
 * Backoff coverage in this file binds directly to the shipped internal BLE
 * policy symbols so the suite fails if production config changes.
 */

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../mocks/Arduino.h"
#include "../mocks/freertos/FreeRTOS.h"
#include "../mocks/freertos/task.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/ble_client.h"

#include "../../include/ble_internals.h"

namespace {

uint8_t calcV1Checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

unsigned long productionBackoffMs(uint8_t consecutiveFailures) {
    return computeV1BleBackoffMs(consecutiveFailures);
}

bool hitsHardResetThreshold(uint8_t consecutiveFailures) {
    return hitsV1BleHardResetThreshold(consecutiveFailures);
}

struct BootGateStateMachine {
    bool bootReadyFlag = false;
    BLEState state = BLEState::DISCONNECTED;

    void setBootReady(bool ready) { bootReadyFlag = ready; }
    bool isBootReady() const { return bootReadyFlag; }

    void process() {
        if (!bootReadyFlag) {
            return;
        }
        if (state == BLEState::DISCONNECTED) {
            state = BLEState::SCANNING;
        }
    }
};

}  // namespace

void test_ble_state_strings_match_production() {
    TEST_ASSERT_EQUAL_STRING("DISCONNECTED", bleStateToString(BLEState::DISCONNECTED));
    TEST_ASSERT_EQUAL_STRING("SCANNING", bleStateToString(BLEState::SCANNING));
    TEST_ASSERT_EQUAL_STRING("SCAN_STOPPING", bleStateToString(BLEState::SCAN_STOPPING));
    TEST_ASSERT_EQUAL_STRING("CONNECTING", bleStateToString(BLEState::CONNECTING));
    TEST_ASSERT_EQUAL_STRING("CONNECTING_WAIT", bleStateToString(BLEState::CONNECTING_WAIT));
    TEST_ASSERT_EQUAL_STRING("DISCOVERING", bleStateToString(BLEState::DISCOVERING));
    TEST_ASSERT_EQUAL_STRING("SUBSCRIBING", bleStateToString(BLEState::SUBSCRIBING));
    TEST_ASSERT_EQUAL_STRING("SUBSCRIBE_YIELD", bleStateToString(BLEState::SUBSCRIBE_YIELD));
    TEST_ASSERT_EQUAL_STRING("CONNECTED", bleStateToString(BLEState::CONNECTED));
    TEST_ASSERT_EQUAL_STRING("BACKOFF", bleStateToString(BLEState::BACKOFF));
}

void test_ble_state_unknown_string() {
    TEST_ASSERT_EQUAL_STRING("UNKNOWN", bleStateToString(static_cast<BLEState>(99)));
}

void test_state_enum_values() {
    TEST_ASSERT_EQUAL_INT(0, static_cast<int>(BLEState::DISCONNECTED));
    TEST_ASSERT_EQUAL_INT(1, static_cast<int>(BLEState::SCANNING));
    TEST_ASSERT_EQUAL_INT(2, static_cast<int>(BLEState::SCAN_STOPPING));
    TEST_ASSERT_EQUAL_INT(3, static_cast<int>(BLEState::CONNECTING));
    TEST_ASSERT_EQUAL_INT(4, static_cast<int>(BLEState::CONNECTING_WAIT));
    TEST_ASSERT_EQUAL_INT(5, static_cast<int>(BLEState::DISCOVERING));
    TEST_ASSERT_EQUAL_INT(6, static_cast<int>(BLEState::SUBSCRIBING));
    TEST_ASSERT_EQUAL_INT(7, static_cast<int>(BLEState::SUBSCRIBE_YIELD));
    TEST_ASSERT_EQUAL_INT(8, static_cast<int>(BLEState::CONNECTED));
    TEST_ASSERT_EQUAL_INT(9, static_cast<int>(BLEState::BACKOFF));
}

void test_production_backoff_constants_match_expected_profile() {
    TEST_ASSERT_EQUAL_UINT8(5, V1_BLE_MAX_BACKOFF_FAILURES);
    TEST_ASSERT_EQUAL_UINT32(200, V1_BLE_BACKOFF_BASE_MS);
    TEST_ASSERT_EQUAL_UINT32(1500, V1_BLE_BACKOFF_MAX_MS);
}

void test_backoff_zero_failures_returns_zero() {
    TEST_ASSERT_EQUAL_UINT32(0, productionBackoffMs(0));
}

void test_backoff_doubles_from_production_base() {
    TEST_ASSERT_EQUAL_UINT32(V1_BLE_BACKOFF_BASE_MS, productionBackoffMs(1));
    TEST_ASSERT_EQUAL_UINT32(V1_BLE_BACKOFF_BASE_MS * 2u, productionBackoffMs(2));
    TEST_ASSERT_EQUAL_UINT32(V1_BLE_BACKOFF_BASE_MS * 4u, productionBackoffMs(3));
    TEST_ASSERT_EQUAL_UINT32(V1_BLE_BACKOFF_MAX_MS, productionBackoffMs(4));
}

void test_backoff_caps_at_production_max() {
    TEST_ASSERT_EQUAL_UINT32(V1_BLE_BACKOFF_MAX_MS,
                             productionBackoffMs(V1_BLE_MAX_BACKOFF_FAILURES));
    TEST_ASSERT_EQUAL_UINT32(V1_BLE_BACKOFF_MAX_MS, productionBackoffMs(10));
    TEST_ASSERT_EQUAL_UINT32(V1_BLE_BACKOFF_MAX_MS, productionBackoffMs(100));
}

void test_hard_reset_threshold_uses_production_limit() {
    TEST_ASSERT_FALSE(hitsHardResetThreshold(V1_BLE_MAX_BACKOFF_FAILURES - 1));
    TEST_ASSERT_TRUE(hitsHardResetThreshold(V1_BLE_MAX_BACKOFF_FAILURES));
    TEST_ASSERT_TRUE(hitsHardResetThreshold(V1_BLE_MAX_BACKOFF_FAILURES + 1));
}

void test_checksum_empty_data() {
    uint8_t data[] = {};
    TEST_ASSERT_EQUAL_UINT8(0, calcV1Checksum(data, 0));
}

void test_checksum_multiple_bytes() {
    uint8_t data[] = {0x01, 0x02, 0x03, 0x04};
    TEST_ASSERT_EQUAL_UINT8(0x0A, calcV1Checksum(data, 4));
}

void test_checksum_overflow_wraps() {
    uint8_t data[] = {0xFF, 0x02};
    TEST_ASSERT_EQUAL_UINT8(0x01, calcV1Checksum(data, 2));
}

void test_checksum_real_v1_packet() {
    uint8_t packet[] = {0xAA, 0x55, 0x01, 0x03, 0x31};
    TEST_ASSERT_EQUAL_UINT8(0x34, calcV1Checksum(packet, 5));
}

void test_boot_gate_blocks_state_machine() {
    BootGateStateMachine sm;
    sm.state = BLEState::DISCONNECTED;
    sm.setBootReady(false);

    sm.process();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BLEState::DISCONNECTED), static_cast<int>(sm.state));

    sm.setBootReady(true);
    sm.process();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(BLEState::SCANNING), static_cast<int>(sm.state));
}

void test_boot_gate_default_false_until_set() {
    BootGateStateMachine sm;
    TEST_ASSERT_FALSE(sm.isBootReady());

    sm.setBootReady(true);
    TEST_ASSERT_TRUE(sm.isBootReady());
}

void setUp(void) {}
void tearDown(void) {}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ble_state_strings_match_production);
    RUN_TEST(test_ble_state_unknown_string);
    RUN_TEST(test_state_enum_values);
    RUN_TEST(test_production_backoff_constants_match_expected_profile);
    RUN_TEST(test_backoff_zero_failures_returns_zero);
    RUN_TEST(test_backoff_doubles_from_production_base);
    RUN_TEST(test_backoff_caps_at_production_max);
    RUN_TEST(test_hard_reset_threshold_uses_production_limit);
    RUN_TEST(test_checksum_empty_data);
    RUN_TEST(test_checksum_multiple_bytes);
    RUN_TEST(test_checksum_overflow_wraps);
    RUN_TEST(test_checksum_real_v1_packet);
    RUN_TEST(test_boot_gate_blocks_state_machine);
    RUN_TEST(test_boot_gate_default_false_until_set);
    return UNITY_END();
}
