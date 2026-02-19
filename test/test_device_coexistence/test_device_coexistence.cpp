/**
 * Device WiFi Coexistence Tests
 *
 * The ESP32-S3 has a single shared radio. WiFi and BLE must time-share, and
 * WiFi requires internal DMA-capable SRAM that competes with BLE stack
 * allocations. These tests validate:
 *   - WiFi AP start/stop heap impact
 *   - DMA memory gate (canStartSetupMode equivalent)
 *   - WiFi TX power configuration
 *   - WiFi repeated start/stop stability
 *
 * SAFETY RULES (learned from production freeze):
 *   - NO NimBLEDevice::deinit() — per CLAUDE.md guardrail:
 *     "Never delete active NimBLE clients at runtime; disconnect and reuse."
 *     BLE tests are limited to read-only queries, not init/deinit cycles.
 *   - tearDown() always forces WiFi OFF, even on assertion failure.
 *   - Generous delays (≥1 s) after WiFi stop for full radio release.
 *   - heap_caps_free() BEFORE TEST_ASSERT on pressure tests.
 *   - No portMAX_DELAY anywhere.
 */

#include <unity.h>
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static uint32_t internalFree() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t internalLargest() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Thresholds from wifi_manager.cpp
static constexpr uint32_t WIFI_MIN_FREE_DMA     = 40 * 1024;   // 40 KB
static constexpr uint32_t WIFI_MIN_LARGEST_BLOCK = 20 * 1024;   // 20 KB

// ---------------------------------------------------------------------------
// Safety: tearDown ALWAYS turns WiFi off, even on assertion failure
// ---------------------------------------------------------------------------

void setUp() {}

void tearDown() {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(1000);
}

// ===========================================================================
// BASELINE HEAP SNAPSHOT
// ===========================================================================

static uint32_t baselineInternalFree    = 0;
static uint32_t baselineInternalLargest = 0;

void test_coex_baseline_heap_snapshot() {
    baselineInternalFree    = internalFree();
    baselineInternalLargest = internalLargest();

    Serial.printf("  [coex] baseline internal free: %lu, largest: %lu\n",
                  (unsigned long)baselineInternalFree,
                  (unsigned long)baselineInternalLargest);

    TEST_ASSERT_GREATER_THAN_UINT32(WIFI_MIN_FREE_DMA, baselineInternalFree);
    TEST_ASSERT_GREATER_THAN_UINT32(WIFI_MIN_LARGEST_BLOCK, baselineInternalLargest);
}

// ===========================================================================
// WIFI AP START / STOP HEAP IMPACT
// ===========================================================================

void test_coex_wifi_ap_start_heap_cost() {
    uint32_t beforeFree = internalFree();

    WiFi.mode(WIFI_AP);
    WiFi.softAP("v1test_coex", "testpass123");
    delay(1000);   // Let WiFi stack fully initialise

    uint32_t duringFree = internalFree();
    uint32_t heapCost   = beforeFree - duringFree;

    Serial.printf("  [coex] WiFi AP heap cost: %lu bytes (free: %lu -> %lu)\n",
                  (unsigned long)heapCost,
                  (unsigned long)beforeFree,
                  (unsigned long)duringFree);

    // WiFi typically costs 40-80 KB of internal SRAM
    TEST_ASSERT_GREATER_THAN_UINT32(20 * 1024, heapCost);
    TEST_ASSERT_LESS_THAN_UINT32(120 * 1024, heapCost);

    // After WiFi start there should still be ≥30 KB free for BLE
    TEST_ASSERT_GREATER_THAN_UINT32(30 * 1024, duringFree);
    // tearDown() cleans up WiFi
}

void test_coex_wifi_stop_heap_recovery() {
    uint32_t beforeWifi = internalFree();

    WiFi.mode(WIFI_AP);
    WiFi.softAP("v1test_recover", "testpass123");
    delay(1000);

    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(1500);   // Full cleanup

    uint32_t afterStop = internalFree();

    Serial.printf("  [coex] WiFi recovery: before=%lu after=%lu\n",
                  (unsigned long)beforeWifi, (unsigned long)afterStop);

    // Should recover within 8 KB of pre-WiFi state
    TEST_ASSERT_UINT32_WITHIN(8 * 1024, beforeWifi, afterStop);
}

// ===========================================================================
// DMA MEMORY GATE
// ===========================================================================

void test_coex_dma_gate_passes_at_boot() {
    uint32_t free    = internalFree();
    uint32_t largest = internalLargest();

    bool canStart = (free >= WIFI_MIN_FREE_DMA) && (largest >= WIFI_MIN_LARGEST_BLOCK);
    TEST_ASSERT_TRUE_MESSAGE(canStart,
        "DMA gate should pass at boot — insufficient internal SRAM");
}

void test_coex_dma_gate_under_pressure() {
    uint32_t free = internalFree();
    size_t pressureSize = free - (WIFI_MIN_FREE_DMA / 2);

    void* pressure = heap_caps_malloc(pressureSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (pressure == nullptr) {
        TEST_IGNORE_MESSAGE("Cannot create pressure allocation");
        return;
    }

    uint32_t pressured_free    = internalFree();
    uint32_t pressured_largest = internalLargest();
    bool canStart = (pressured_free >= WIFI_MIN_FREE_DMA)
                 && (pressured_largest >= WIFI_MIN_LARGEST_BLOCK);

    Serial.printf("  [coex] under pressure: free=%lu largest=%lu canStart=%d\n",
                  (unsigned long)pressured_free,
                  (unsigned long)pressured_largest,
                  canStart);

    // Free BEFORE asserting — ensures cleanup even on assertion failure
    heap_caps_free(pressure);
    delay(50);

    TEST_ASSERT_FALSE_MESSAGE(canStart,
        "DMA gate should block WiFi when internal SRAM is exhausted");
}

// ===========================================================================
// WIFI TX POWER
// ===========================================================================

void test_coex_wifi_tx_power_setting() {
    WiFi.mode(WIFI_AP);
    WiFi.softAP("v1test_txpwr", "testpass123");
    delay(500);

    WiFi.setTxPower(WIFI_POWER_5dBm);
    wifi_power_t power = WiFi.getTxPower();

    Serial.printf("  [coex] TX power set: %d\n", (int)power);

    TEST_ASSERT_LESS_OR_EQUAL((int)WIFI_POWER_5dBm, (int)power);
    // tearDown() cleans up WiFi
}

// ===========================================================================
// WIFI REPEATED START/STOP STABILITY
// ===========================================================================

void test_coex_wifi_repeated_start_stop_no_leak() {
    uint32_t baseline = internalFree();

    for (int i = 0; i < 3; i++) {
        WiFi.mode(WIFI_AP);
        WiFi.softAP("v1cycle", "testpass123");
        delay(500);
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        delay(1000);   // 1 s cool-down per cycle
    }

    uint32_t after = internalFree();

    Serial.printf("  [coex] WiFi 3x cycle: baseline=%lu after=%lu delta=%ld\n",
                  (unsigned long)baseline, (unsigned long)after,
                  (long)after - (long)baseline);

    TEST_ASSERT_UINT32_WITHIN(12 * 1024, baseline, after);
}

// ===========================================================================
// TEST RUNNER
// ===========================================================================

void setup() {
    delay(2000);

    WiFi.mode(WIFI_OFF);
    delay(500);

    UNITY_BEGIN();

    RUN_TEST(test_coex_baseline_heap_snapshot);
    RUN_TEST(test_coex_wifi_ap_start_heap_cost);
    RUN_TEST(test_coex_wifi_stop_heap_recovery);
    RUN_TEST(test_coex_dma_gate_passes_at_boot);
    RUN_TEST(test_coex_dma_gate_under_pressure);
    RUN_TEST(test_coex_wifi_tx_power_setting);
    RUN_TEST(test_coex_wifi_repeated_start_stop_no_leak);

    UNITY_END();
}

void loop() {}
