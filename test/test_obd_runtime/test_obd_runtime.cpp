#include <unity.h>

#include "../../src/modules/obd/obd_runtime_module.h"
#include "../../src/modules/obd/obd_elm327_parser.cpp"
#include "../../src/modules/obd/obd_runtime_module.cpp"

static void resetRuntime() {
    obdRuntimeModule = ObdRuntimeModule();
}

static void feedBleResponse(const char* response) {
    obdRuntimeModule.onBleData(reinterpret_cast<const uint8_t*>(response), strlen(response));
}

void setUp() {
    resetRuntime();
}

void tearDown() {}

// ── begin() state transitions ─────────────────────────────────────

void test_begin_disabled_stays_idle() {
    obdRuntimeModule.begin(false, "", 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_FALSE(obdRuntimeModule.isEnabled());
}

void test_begin_enabled_no_saved_addr_goes_idle() {
    obdRuntimeModule.begin(true, "", 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isEnabled());
}

void test_begin_enabled_no_saved_addr_null_goes_idle() {
    obdRuntimeModule.begin(true, nullptr, 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_begin_enabled_with_saved_addr_goes_wait_boot() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(0);
    TEST_ASSERT_TRUE(status.savedAddressValid);
}

// ── Boot defer tests ──────────────────────────────────────────────

void test_wait_boot_stays_until_boot_ready() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    // Boot not ready — should stay in WAIT_BOOT
    obdRuntimeModule.update(1000, false, false, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());

    obdRuntimeModule.update(4000, false, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
}

void test_wait_boot_transitions_when_v1_connected() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    // Boot ready, V1 connected → should transition to CONNECTING
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_wait_boot_transitions_after_dwell_without_v1() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    // Boot ready at 5000ms
    obdRuntimeModule.update(5000, true, false, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());

    // Still waiting (dwell not expired)
    obdRuntimeModule.update(14000, true, false, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());

    // Dwell expired (10s after boot ready)
    obdRuntimeModule.update(15001, true, false, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_idle_no_scan_at_boot_without_saved_addr() {
    obdRuntimeModule.begin(true, "", 0, -80);

    // Even with boot ready and everything clear — no scan, stays IDLE
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    obdRuntimeModule.update(60000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_disabled_module_never_transitions() {
    obdRuntimeModule.begin(false, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

// ── Web UI scan trigger ───────────────────────────────────────────

void test_start_scan_from_idle() {
    obdRuntimeModule.begin(true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1000);
    TEST_ASSERT_TRUE(status.scanInProgress);
}

void test_start_scan_waits_for_ble_scan_idle() {
    obdRuntimeModule.begin(true, "", 0, -80);

    obdRuntimeModule.startScan();
    // V1 is scanning (bleScanIdle=false) — OBD should wait
    obdRuntimeModule.update(1000, true, true, false);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    // Now BLE scan is idle
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_start_scan_disabled_does_nothing() {
    obdRuntimeModule.begin(false, "", 0, -80);
    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_scan_timeout_returns_to_idle() {
    obdRuntimeModule.begin(true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    // After scan duration (5000ms) with no device found
    obdRuntimeModule.update(6001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_scan_finds_device_transitions_to_connecting() {
    obdRuntimeModule.begin(true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    // Device found with good RSSI
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);

    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Address should be saved
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(2000);
    TEST_ASSERT_TRUE(status.savedAddressValid);
}

void test_scan_request_retries_when_start_scan_fails_once() {
    obdRuntimeModule.begin(true, "", 0, -80);
    obdRuntimeModule.setTestStartScanResult(false);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getStartScanCallCountForTest());

    obdRuntimeModule.setTestStartScanResult(true);
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getStartScanCallCountForTest());
}

// ── RSSI gate ─────────────────────────────────────────────────────

void test_rssi_gate_rejects_weak_signal() {
    obdRuntimeModule.begin(true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);

    // Device found with weak RSSI (below threshold)
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -90);

    obdRuntimeModule.update(2000, true, true, true);
    // Should still be scanning — device was rejected
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_rssi_gate_accepts_strong_signal() {
    obdRuntimeModule.begin(true, "", 0, -60);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);

    // Device found with strong RSSI (above threshold of -60)
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);

    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_rssi_gate_rejects_at_boundary() {
    obdRuntimeModule.begin(true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);

    // Device at exactly minimum — should be rejected (< not <=)
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -81);

    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_device_found_outside_scanning_ignored() {
    obdRuntimeModule.begin(true, "", 0, -80);
    // In IDLE, not scanning
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

// ── Connect timeout & retry ───────────────────────────────────────

void test_connect_timeout_increments_attempts() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    // Get past boot wait
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Connect times out after 5s
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(10001);
    TEST_ASSERT_EQUAL_UINT8(1, status.connectAttempts);
}

void test_three_connect_failures_clears_saved_address() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    // Get past boot wait
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 1
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // Wait for fast reconnect backoff
    obdRuntimeModule.update(15002, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 2
    obdRuntimeModule.update(20003, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // Wait for fast reconnect backoff
    obdRuntimeModule.update(25004, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 3 — should clear saved address and go to IDLE
    obdRuntimeModule.update(30005, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(30005);
    TEST_ASSERT_FALSE(status.savedAddressValid);
    TEST_ASSERT_EQUAL_UINT8(0, status.connectAttempts);
}

void test_connect_entry_action_runs_on_next_tick() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getConnectCallCountForTest());

    obdRuntimeModule.update(5001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getConnectCallCountForTest());
}

void test_discover_entry_action_runs_on_next_tick() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SECURING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getConnectCallCountForTest());

    obdRuntimeModule.setTestDiscoverResult(false);
    // POST_CONNECT_SETTLE_MS (500ms) must elapse before security / GATT work begins.
    obdRuntimeModule.update(5501, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());
    obdRuntimeModule.update(5502, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDiscoverCallCountForTest());
}

void test_connect_enters_securing_before_discovery() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::SECURING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getDiscoverCallCountForTest());
}

void test_discovery_waits_for_security_ready() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);

    obdRuntimeModule.update(5000, true, true, true);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SECURING, obdRuntimeModule.getState());

    obdRuntimeModule.update(5501, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SECURING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getBeginSecurityCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getDiscoverCallCountForTest());

    obdRuntimeModule.setTestSecurityReady(true);
    obdRuntimeModule.setTestSecurityEncrypted(true);
    obdRuntimeModule.setTestSecurityBonded(true);
    obdRuntimeModule.update(5502, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());
}

void test_security_timeout_auto_heals_bond_once() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);

    obdRuntimeModule.update(4000, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getRefreshBondBackupCallCountForTest());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(4000);
    TEST_ASSERT_EQUAL_UINT32(1, status.securityRepairs);
}

void test_security_timeout_does_not_repair_same_bond_twice() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);

    obdRuntimeModule.update(4000, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());

    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(4001, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
}

void test_first_at_init_write_failure_auto_heals_bond() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::AT_INIT, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(true);
    obdRuntimeModule.setTestSecurityEncrypted(true);
    obdRuntimeModule.setTestSecurityBonded(true);
    obdRuntimeModule.setTestWriteResult(false);
    obdRuntimeModule.setTestLastBleError(1);

    obdRuntimeModule.update(100, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
}

// ── Speed data ────────────────────────────────────────────────────

void test_inject_speed_is_fresh() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(37.3f, 1000);

    float speed = 0.0f;
    uint32_t ts = 0;
    TEST_ASSERT_TRUE(obdRuntimeModule.getFreshSpeed(2000, speed, ts));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 37.3f, speed);
    TEST_ASSERT_EQUAL_UINT32(1000, ts);
}

void test_speed_goes_stale() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(37.3f, 1000);

    float speed = 0.0f;
    uint32_t ts = 0;
    // 3001ms after sample — stale
    TEST_ASSERT_FALSE(obdRuntimeModule.getFreshSpeed(4002, speed, ts));
}

void test_speed_boundary_fresh() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(50.0f, 1000);

    float speed = 0.0f;
    uint32_t ts = 0;
    // Exactly at max age boundary — should still be fresh
    TEST_ASSERT_TRUE(obdRuntimeModule.getFreshSpeed(4000, speed, ts));
}

void test_snapshot_speed_age() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(60.0f, 1000);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(2500);
    TEST_ASSERT_TRUE(status.speedValid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, status.speedMph);
    TEST_ASSERT_EQUAL_UINT32(1500, status.speedAgeMs);
}

void test_no_speed_when_disabled() {
    obdRuntimeModule.begin(false, "", 0, -80);
    float speed = 0.0f;
    uint32_t ts = 0;
    TEST_ASSERT_FALSE(obdRuntimeModule.getFreshSpeed(1000, speed, ts));

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1000);
    TEST_ASSERT_FALSE(status.speedValid);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, status.speedAgeMs);
}

// ── forgetDevice() ────────────────────────────────────────────────

void test_forget_device_clears_address_and_goes_idle() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    obdRuntimeModule.forgetDevice();
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(5000);
    TEST_ASSERT_FALSE(status.savedAddressValid);
}

void test_forget_device_disconnects_active_client() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 1000);
    obdRuntimeModule.setTestBleConnected(true);

    obdRuntimeModule.forgetDevice();

    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());
}

// ── setEnabled() ──────────────────────────────────────────────────

void test_disable_during_operation_goes_idle() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    obdRuntimeModule.setEnabled(false);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_FALSE(obdRuntimeModule.isEnabled());
}

void test_enable_same_state_is_noop() {
    obdRuntimeModule.begin(true, "", 0, -80);
    obdRuntimeModule.setEnabled(true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_reenable_with_saved_address_restores_wait_boot() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.setEnabled(false);
    obdRuntimeModule.setEnabled(true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
}

void test_set_min_rssi_applies_immediately() {
    obdRuntimeModule.begin(true, "", 0, -80);
    obdRuntimeModule.setMinRssi(-60);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -70);
    obdRuntimeModule.update(2000, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

// ── Error backoff ─────────────────────────────────────────────────

void test_poll_timeout_counts_as_error() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);

    obdRuntimeModule.update(100, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());
    TEST_ASSERT_EQUAL_STRING("010D\r", obdRuntimeModule.getLastCommandForTest());

    obdRuntimeModule.update(600, true, true, true);
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(600);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
    TEST_ASSERT_EQUAL_UINT32(1, status.consecutiveErrors);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
}

void test_cached_profile_polls_before_background_vin_lookup() {
    obdRuntimeModule.begin(true,
                           "A4:C1:38:00:11:22",
                           0,
                           -80,
                           "1FTW1ET7DFA",
                           static_cast<uint8_t>(ObdEotProfileId::FORD_22F45C));
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);
    obdRuntimeModule.injectSpeedForTest(45.0f, 5900);
    obdRuntimeModule.setConsecutiveSpeedSamplesForTest(3);

    obdRuntimeModule.update(6000, true, true, true);
    TEST_ASSERT_EQUAL_STRING("010D\r", obdRuntimeModule.getLastCommandForTest());

    feedBleResponse("41 0D 28\r\n>");
    obdRuntimeModule.update(6050, true, true, true);
    obdRuntimeModule.update(6100, true, true, true);

    TEST_ASSERT_EQUAL(ObdCommandKind::EOT_POLL, obdRuntimeModule.getActiveCommandKindForTest());
    TEST_ASSERT_EQUAL_STRING("22F45C\r", obdRuntimeModule.getLastCommandForTest());
    TEST_ASSERT_EQUAL(ObdEotProfileId::FORD_22F45C,
                      obdRuntimeModule.getActiveEotProfileForTest());
}

void test_vin_response_sets_family_and_starts_standard_eot_probe() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);
    obdRuntimeModule.injectSpeedForTest(45.0f, 5900);
    obdRuntimeModule.setConsecutiveSpeedSamplesForTest(3);

    obdRuntimeModule.update(6000, true, true, true);
    feedBleResponse("41 0D 28\r\n>");
    obdRuntimeModule.update(6050, true, true, true);
    obdRuntimeModule.update(6100, true, true, true);
    TEST_ASSERT_EQUAL_STRING("0902\r", obdRuntimeModule.getLastCommandForTest());

    feedBleResponse(
        "0902\r\n"
        "0: 49 02 01 31 46 54\r\n"
        "1: 57 31 45 54 37 44\r\n"
        "2: 46 41 31 32 33 34\r\n"
        "3: 35 36\r\n"
        ">");
    obdRuntimeModule.update(6150, true, true, true);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(6150);
    TEST_ASSERT_TRUE(status.vinDetected);
    TEST_ASSERT_EQUAL(ObdVehicleFamily::FORD, status.vehicleFamily);

    obdRuntimeModule.update(6200, true, true, true);
    TEST_ASSERT_EQUAL(ObdCommandKind::EOT_PROBE, obdRuntimeModule.getActiveCommandKindForTest());
    TEST_ASSERT_EQUAL_STRING("015C\r", obdRuntimeModule.getLastCommandForTest());
}

void test_error_backoff_returns_to_polling() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 1000);
    obdRuntimeModule.setConsecutiveErrorsForTest(5);

    obdRuntimeModule.update(6001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
}

void test_error_backoff_disconnects_after_ten_errors() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 1000);
    obdRuntimeModule.setConsecutiveErrorsForTest(10);

    obdRuntimeModule.update(6001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
}

// ── Disconnected reconnect backoff ────────────────────────────────

void test_disconnected_reconnects_after_backoff() {
    obdRuntimeModule.begin(true, "A4:C1:38:00:11:22", 0, -80);

    // Boot ready, V1 connected → CONNECTING
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Connect timeout → DISCONNECTED
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // Before backoff — still DISCONNECTED
    obdRuntimeModule.update(14000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // After fast reconnect backoff — back to CONNECTING
    obdRuntimeModule.update(15002, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_disconnected_no_saved_addr_goes_idle() {
    obdRuntimeModule.begin(true, "", 0, -80);

    // Trigger scan, find device, connect, then fail 3 times
    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 1
    obdRuntimeModule.update(7001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // Backoff, reconnect, fail 2
    obdRuntimeModule.update(12002, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    obdRuntimeModule.update(17003, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // Backoff, reconnect, fail 3 → clears address → IDLE
    obdRuntimeModule.update(22004, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    obdRuntimeModule.update(27005, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(27005);
    TEST_ASSERT_FALSE(status.savedAddressValid);
}

int main() {
    UNITY_BEGIN();

    // begin() state transitions
    RUN_TEST(test_begin_disabled_stays_idle);
    RUN_TEST(test_begin_enabled_no_saved_addr_goes_idle);
    RUN_TEST(test_begin_enabled_no_saved_addr_null_goes_idle);
    RUN_TEST(test_begin_enabled_with_saved_addr_goes_wait_boot);

    // Boot defer
    RUN_TEST(test_wait_boot_stays_until_boot_ready);
    RUN_TEST(test_wait_boot_transitions_when_v1_connected);
    RUN_TEST(test_wait_boot_transitions_after_dwell_without_v1);
    RUN_TEST(test_idle_no_scan_at_boot_without_saved_addr);
    RUN_TEST(test_disabled_module_never_transitions);

    // Web UI scan
    RUN_TEST(test_start_scan_from_idle);
    RUN_TEST(test_start_scan_waits_for_ble_scan_idle);
    RUN_TEST(test_start_scan_disabled_does_nothing);
    RUN_TEST(test_scan_timeout_returns_to_idle);
    RUN_TEST(test_scan_finds_device_transitions_to_connecting);
    RUN_TEST(test_scan_request_retries_when_start_scan_fails_once);

    // RSSI gate
    RUN_TEST(test_rssi_gate_rejects_weak_signal);
    RUN_TEST(test_rssi_gate_accepts_strong_signal);
    RUN_TEST(test_rssi_gate_rejects_at_boundary);
    RUN_TEST(test_device_found_outside_scanning_ignored);

    // Connect timeout & retry
    RUN_TEST(test_connect_timeout_increments_attempts);
    RUN_TEST(test_three_connect_failures_clears_saved_address);
    RUN_TEST(test_connect_entry_action_runs_on_next_tick);
    RUN_TEST(test_discover_entry_action_runs_on_next_tick);
    RUN_TEST(test_connect_enters_securing_before_discovery);
    RUN_TEST(test_discovery_waits_for_security_ready);
    RUN_TEST(test_security_timeout_auto_heals_bond_once);
    RUN_TEST(test_security_timeout_does_not_repair_same_bond_twice);
    RUN_TEST(test_first_at_init_write_failure_auto_heals_bond);

    // Speed data
    RUN_TEST(test_inject_speed_is_fresh);
    RUN_TEST(test_speed_goes_stale);
    RUN_TEST(test_speed_boundary_fresh);
    RUN_TEST(test_snapshot_speed_age);
    RUN_TEST(test_no_speed_when_disabled);

    // forgetDevice
    RUN_TEST(test_forget_device_clears_address_and_goes_idle);
    RUN_TEST(test_forget_device_disconnects_active_client);

    // setEnabled
    RUN_TEST(test_disable_during_operation_goes_idle);
    RUN_TEST(test_enable_same_state_is_noop);
    RUN_TEST(test_reenable_with_saved_address_restores_wait_boot);
    RUN_TEST(test_set_min_rssi_applies_immediately);

    // Error handling
    RUN_TEST(test_poll_timeout_counts_as_error);
    RUN_TEST(test_cached_profile_polls_before_background_vin_lookup);
    RUN_TEST(test_vin_response_sets_family_and_starts_standard_eot_probe);
    RUN_TEST(test_error_backoff_returns_to_polling);
    RUN_TEST(test_error_backoff_disconnects_after_ten_errors);

    // Disconnected reconnect
    RUN_TEST(test_disconnected_reconnects_after_backoff);
    RUN_TEST(test_disconnected_no_saved_addr_goes_idle);

    return UNITY_END();
}
