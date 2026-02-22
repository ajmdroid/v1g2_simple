#include <unity.h>

#include <cmath>
#include <utility>

#include "../mocks/mock_heap_caps_state.h"
#include "../../src/modules/camera/camera_data_loader.h"
#include "../../src/modules/camera/camera_event_log.cpp"      // Pull implementation for UNIT_TEST.
#include "../../src/modules/camera/camera_index.cpp"          // Pull implementation for UNIT_TEST.
#include "../../src/modules/camera/camera_runtime_module.h"
#include "../../src/modules/gps/gps_observation_log.cpp"      // Pull implementation for UNIT_TEST.
#include "../../src/modules/gps/gps_runtime_module.cpp"       // Pull implementation for UNIT_TEST.

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {
CameraIndexOwnedBuffers gReadyBuffers = {};
bool gReadyBuffersValid = false;
uint32_t gNextVersion = 1;

void resetReadyBuffers() {
    if (gReadyBuffersValid) {
        CameraIndex::freeOwnedBuffers(gReadyBuffers);
        gReadyBuffers = {};
        gReadyBuffersValid = false;
    }
}

void queueSingleCamera(float latitudeDeg,
                       float longitudeDeg,
                       int16_t bearingTenthsDeg,
                       uint8_t toleranceDeg,
                       uint8_t type,
                       float snapLatitudeDeg = NAN,
                       float snapLongitudeDeg = NAN,
                       uint8_t widthM = 25) {
    resetReadyBuffers();

    gReadyBuffers.records = static_cast<CameraRecord*>(
        heap_caps_malloc(sizeof(CameraRecord), MALLOC_CAP_8BIT));
    gReadyBuffers.spans = static_cast<CameraCellSpan*>(
        heap_caps_malloc(sizeof(CameraCellSpan), MALLOC_CAP_8BIT));
    TEST_ASSERT_NOT_NULL(gReadyBuffers.records);
    TEST_ASSERT_NOT_NULL(gReadyBuffers.spans);

    const bool hasSnapPoint = std::isfinite(snapLatitudeDeg) && std::isfinite(snapLongitudeDeg);
    const float anchorLatitudeDeg = hasSnapPoint ? snapLatitudeDeg : latitudeDeg;
    const float anchorLongitudeDeg = hasSnapPoint ? snapLongitudeDeg : longitudeDeg;

    CameraRecord& record = gReadyBuffers.records[0];
    record.latitudeDeg = latitudeDeg;
    record.longitudeDeg = longitudeDeg;
    record.snapLatitudeDeg = anchorLatitudeDeg;
    record.snapLongitudeDeg = anchorLongitudeDeg;
    record.bearingTenthsDeg = bearingTenthsDeg;
    record.widthM = widthM;
    record.toleranceDeg = toleranceDeg;
    record.type = type;
    record.speedLimit = 35;
    record.flags = 0;
    record.reserved = 0;
    record.cellKey = CameraIndex::encodeCellKey(anchorLatitudeDeg, anchorLongitudeDeg);

    gReadyBuffers.recordCount = 1;
    gReadyBuffers.spans[0].cellKey = record.cellKey;
    gReadyBuffers.spans[0].beginIndex = 0;
    gReadyBuffers.spans[0].endIndex = 1;
    gReadyBuffers.spanCount = 1;
    gReadyBuffers.version = gNextVersion++;
    gReadyBuffersValid = true;
}

void setGpsSample(float latitudeDeg, float longitudeDeg, float courseDeg, uint32_t nowMs) {
    mockMillis = nowMs;
    mockMicros = static_cast<unsigned long>(nowMs) * 1000UL;
    gpsRuntimeModule.setScaffoldSample(40.0f,
                                       true,
                                       8,
                                       0.9f,
                                       nowMs,
                                       latitudeDeg,
                                       longitudeDeg,
                                       courseDeg);
}

void processCameraTick(uint32_t nowMs, bool signalPriorityActive = false) {
    mockMillis = nowMs;
    mockMicros = static_cast<unsigned long>(nowMs) * 1000UL;
    cameraRuntimeModule.process(nowMs, false, false, signalPriorityActive);
}
}  // namespace

// Stub camera loader methods so camera runtime tests can inject ready buffers directly.
void CameraDataLoader::begin() {}

void CameraDataLoader::reset() {
    resetReadyBuffers();
}

void CameraDataLoader::requestReload() {}

bool CameraDataLoader::consumeReady(CameraIndex& activeIndex) {
    if (!gReadyBuffersValid) {
        return false;
    }
    CameraIndexOwnedBuffers ready = gReadyBuffers;
    gReadyBuffers = {};
    gReadyBuffersValid = false;
    return activeIndex.adopt(std::move(ready));
}

CameraDataLoaderStatus CameraDataLoader::status() const {
    return {};
}

#include "../../src/modules/camera/camera_runtime_module.cpp"  // Pull implementation for UNIT_TEST.

void setUp() {
    mockMillis = 1;
    mockMicros = 1000;
    mock_reset_heap_caps();
    gpsRuntimeModule = GpsRuntimeModule();
    gpsRuntimeModule.begin(true);
    cameraRuntimeModule.begin(true);
    resetReadyBuffers();
}

void tearDown() {
    mock_reset_heap_caps();
    resetReadyBuffers();
}

void test_forward_only_start_requires_heading_alignment() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 4);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);

    CameraRuntimeStatus aligned = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::ACTIVE),
                            static_cast<uint8_t>(aligned.lifecycleState));
    TEST_ASSERT_TRUE(aligned.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(1, aligned.activeAlert.cameraId);

    CameraEvent recent[CameraEventLog::kCapacity] = {};
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(cameraRuntimeModule.eventLog().copyRecent(recent, 8)));

    cameraRuntimeModule.begin(true);
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 4);
    setGpsSample(0.0f, 0.0f, 270.0f, 2000);
    processCameraTick(2000);

    CameraRuntimeStatus opposite = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::IDLE),
                            static_cast<uint8_t>(opposite.lifecycleState));
    TEST_ASSERT_FALSE(opposite.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cameraRuntimeModule.eventLog().copyRecent(recent, 8)));
}

void test_lifecycle_clears_on_pass_distance_and_lifts_after_exit() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);

    setGpsSample(0.0f, 0.00095f, 90.0f, 1300);  // ~5.5m from camera
    processCameraTick(1300);

    CameraRuntimeStatus afterPass = cameraRuntimeModule.snapshot();
    TEST_ASSERT_FALSE(afterPass.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::SUPPRESSED_UNTIL_EXIT),
                            static_cast<uint8_t>(afterPass.lifecycleState));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraClearReason::PASS_DISTANCE),
                            static_cast<uint8_t>(afterPass.lastClearReason));
    TEST_ASSERT_EQUAL_UINT32(1, afterPass.suppressedCameraId);

    setGpsSample(0.0f, 0.0100f, 90.0f, 1600);
    processCameraTick(1600);
    setGpsSample(0.0f, 0.0110f, 90.0f, 1900);
    processCameraTick(1900);

    CameraRuntimeStatus afterExit = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::IDLE),
                            static_cast<uint8_t>(afterExit.lifecycleState));
    TEST_ASSERT_EQUAL_UINT32(0, afterExit.suppressedCameraId);
}

void test_corridor_width_blocks_off_corridor_candidate() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2, NAN, NAN, 25);
    setGpsSample(0.0005f, 0.0f, 90.0f, 1000);  // ~55m north of corridor centerline.
    processCameraTick(1000);

    CameraRuntimeStatus outsideCorridor = cameraRuntimeModule.snapshot();
    TEST_ASSERT_FALSE(outsideCorridor.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::IDLE),
                            static_cast<uint8_t>(outsideCorridor.lifecycleState));

    setGpsSample(0.0001f, 0.0f, 90.0f, 1300);  // ~11m north, inside width=25m.
    processCameraTick(1300);
    CameraRuntimeStatus insideCorridor = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(insideCorridor.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(1, insideCorridor.activeAlert.cameraId);
}

void test_snap_anchor_distance_allows_match_when_raw_point_is_far() {
    queueSingleCamera(
        0.0f, 0.0200f, 900, 35, 2, 0.0f, 0.0010f, 25);  // Raw point far; snap point is nearby.
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);

    CameraRuntimeStatus status = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(status.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(1, status.activeAlert.cameraId);
}

void test_lifecycle_clears_on_turn_away_after_two_ticks() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 1);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);

    setGpsSample(0.0f, 0.0f, 270.0f, 1300);
    processCameraTick(1300);
    TEST_ASSERT_TRUE(cameraRuntimeModule.snapshot().activeAlert.active);

    setGpsSample(0.0f, 0.0f, 270.0f, 1600);
    processCameraTick(1600);

    CameraRuntimeStatus status = cameraRuntimeModule.snapshot();
    TEST_ASSERT_FALSE(status.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::SUPPRESSED_UNTIL_EXIT),
                            static_cast<uint8_t>(status.lifecycleState));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraClearReason::TURN_AWAY),
                            static_cast<uint8_t>(status.lastClearReason));
}

void test_preempt_suppresses_same_pass_until_exit_then_allows_reentry() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 4);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);

    processCameraTick(1010, true);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::PREEMPTED),
                            static_cast<uint8_t>(cameraRuntimeModule.snapshot().lifecycleState));

    setGpsSample(0.0f, 0.0f, 90.0f, 1300);
    processCameraTick(1300);
    CameraRuntimeStatus suppressed = cameraRuntimeModule.snapshot();
    TEST_ASSERT_FALSE(suppressed.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(1, suppressed.suppressedCameraId);

    setGpsSample(0.0f, 0.0100f, 90.0f, 32000);
    processCameraTick(32000);
    setGpsSample(0.0f, 0.0110f, 90.0f, 32300);
    processCameraTick(32300);
    TEST_ASSERT_EQUAL_UINT32(0, cameraRuntimeModule.snapshot().suppressedCameraId);

    setGpsSample(0.0f, 0.0f, 90.0f, 32600);
    processCameraTick(32600);
    CameraRuntimeStatus reentry = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(reentry.activeAlert.active);

    CameraEvent recent[CameraEventLog::kCapacity] = {};
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(cameraRuntimeModule.eventLog().copyRecent(recent, 8)));
}

void test_signal_priority_blocks_new_camera_start_until_cleared() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);

    processCameraTick(1000, true);
    CameraRuntimeStatus blocked = cameraRuntimeModule.snapshot();
    TEST_ASSERT_FALSE(blocked.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::IDLE),
                            static_cast<uint8_t>(blocked.lifecycleState));

    CameraEvent recent[CameraEventLog::kCapacity] = {};
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(cameraRuntimeModule.eventLog().copyRecent(recent, 8)));

    processCameraTick(1300, false);
    CameraRuntimeStatus started = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(started.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(1, started.activeAlert.cameraId);
}

void test_memory_guard_blocks_then_recovers_candidate_scans() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);

    // Force memory-guard rejection.
    mock_set_heap_caps(20000u, 12000u);
    processCameraTick(1000);
    CameraRuntimeStatus blocked = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(blocked.counters.cameraTickSkipsMemoryGuard > 0);
    TEST_ASSERT_EQUAL_UINT32(0, blocked.counters.cameraCandidatesChecked);
    TEST_ASSERT_FALSE(blocked.activeAlert.active);

    // Restore healthy heap and verify candidate scanning resumes.
    mock_set_heap_caps(64000u, 32000u);
    processCameraTick(1300);
    CameraRuntimeStatus recovered = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(recovered.counters.cameraCandidatesChecked > blocked.counters.cameraCandidatesChecked);
    TEST_ASSERT_TRUE(recovered.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(1, recovered.activeAlert.cameraId);
}

void test_memory_guard_blocks_when_free_below_threshold() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);

    mock_set_heap_caps(
        CameraRuntimeModule::kMemoryGuardMinFreeInternal - 1u,
        CameraRuntimeModule::kMemoryGuardMinLargestBlock + 1024u);
    processCameraTick(1000);

    CameraRuntimeStatus blocked = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT32(1, blocked.counters.cameraTickSkipsMemoryGuard);
    TEST_ASSERT_EQUAL_UINT32(0, blocked.counters.cameraCandidatesChecked);
    TEST_ASSERT_FALSE(blocked.activeAlert.active);
}

void test_memory_guard_blocks_when_largest_block_below_threshold() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);

    mock_set_heap_caps(
        CameraRuntimeModule::kMemoryGuardMinFreeInternal + 1024u,
        CameraRuntimeModule::kMemoryGuardMinLargestBlock - 1u);
    processCameraTick(1000);

    CameraRuntimeStatus blocked = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT32(1, blocked.counters.cameraTickSkipsMemoryGuard);
    TEST_ASSERT_EQUAL_UINT32(0, blocked.counters.cameraCandidatesChecked);
    TEST_ASSERT_FALSE(blocked.activeAlert.active);
}

void test_memory_guard_allows_scan_at_exact_thresholds() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);

    mock_set_heap_caps(
        CameraRuntimeModule::kMemoryGuardMinFreeInternal,
        CameraRuntimeModule::kMemoryGuardMinLargestBlock);
    processCameraTick(1000);

    CameraRuntimeStatus status = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT32(0, status.counters.cameraTickSkipsMemoryGuard);
    TEST_ASSERT_TRUE(status.counters.cameraCandidatesChecked > 0);
    TEST_ASSERT_TRUE(status.activeAlert.active);
}

void test_memory_guard_skip_counter_accumulates_under_sustained_pressure() {
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 2);
    mock_set_heap_caps(20000u, 12000u);

    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);
    setGpsSample(0.0f, 0.0f, 90.0f, 1300);
    processCameraTick(1300);
    setGpsSample(0.0f, 0.0f, 90.0f, 1600);
    processCameraTick(1600);

    CameraRuntimeStatus blocked = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT32(3, blocked.counters.cameraTickSkipsMemoryGuard);
    TEST_ASSERT_EQUAL_UINT32(3, blocked.counters.cameraTicks);
    TEST_ASSERT_EQUAL_UINT32(0, blocked.counters.cameraCandidatesChecked);

    mock_set_heap_caps(64000u, 32000u);
    setGpsSample(0.0f, 0.0f, 90.0f, 1900);
    processCameraTick(1900);

    CameraRuntimeStatus recovered = cameraRuntimeModule.snapshot();
    TEST_ASSERT_EQUAL_UINT32(3, recovered.counters.cameraTickSkipsMemoryGuard);
    TEST_ASSERT_TRUE(recovered.counters.cameraCandidatesChecked > 0);
    TEST_ASSERT_TRUE(recovered.activeAlert.active);
}

void test_alert_distance_tuning_expands_match_gate() {
    // ~556m from origin.
    queueSingleCamera(0.0f, 0.0050f, 900, 35, 4);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);

    CameraRuntimeStatus defaultDistance = cameraRuntimeModule.snapshot();
    TEST_ASSERT_FALSE(defaultDistance.activeAlert.active);

    cameraRuntimeModule.begin(true);
    cameraRuntimeModule.setAlertTuning(2000, 5);  // ~610m trigger radius
    queueSingleCamera(0.0f, 0.0050f, 900, 35, 4);
    setGpsSample(0.0f, 0.0f, 90.0f, 2000);
    processCameraTick(2000);

    CameraRuntimeStatus expandedDistance = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(expandedDistance.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT32(1, expandedDistance.activeAlert.cameraId);
}

void test_active_alert_times_out_when_pass_distance_not_met() {
    cameraRuntimeModule.setAlertTuning(1640, 3);
    queueSingleCamera(0.0f, 0.0010f, 900, 35, 4);
    setGpsSample(0.0f, 0.0f, 90.0f, 1000);
    processCameraTick(1000);

    CameraRuntimeStatus started = cameraRuntimeModule.snapshot();
    TEST_ASSERT_TRUE(started.activeAlert.active);

    // Stay on course but never pass the camera; timeout should clear lifecycle.
    setGpsSample(0.0f, 0.0f, 90.0f, 4100);
    processCameraTick(4100);

    CameraRuntimeStatus timedOut = cameraRuntimeModule.snapshot();
    TEST_ASSERT_FALSE(timedOut.activeAlert.active);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraLifecycleState::SUPPRESSED_UNTIL_EXIT),
                            static_cast<uint8_t>(timedOut.lifecycleState));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraClearReason::TIMEOUT),
                            static_cast<uint8_t>(timedOut.lastClearReason));
    TEST_ASSERT_EQUAL_UINT32(1, timedOut.suppressedCameraId);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_forward_only_start_requires_heading_alignment);
    RUN_TEST(test_lifecycle_clears_on_pass_distance_and_lifts_after_exit);
    RUN_TEST(test_corridor_width_blocks_off_corridor_candidate);
    RUN_TEST(test_snap_anchor_distance_allows_match_when_raw_point_is_far);
    RUN_TEST(test_lifecycle_clears_on_turn_away_after_two_ticks);
    RUN_TEST(test_preempt_suppresses_same_pass_until_exit_then_allows_reentry);
    RUN_TEST(test_signal_priority_blocks_new_camera_start_until_cleared);
    RUN_TEST(test_memory_guard_blocks_then_recovers_candidate_scans);
    RUN_TEST(test_memory_guard_blocks_when_free_below_threshold);
    RUN_TEST(test_memory_guard_blocks_when_largest_block_below_threshold);
    RUN_TEST(test_memory_guard_allows_scan_at_exact_thresholds);
    RUN_TEST(test_memory_guard_skip_counter_accumulates_under_sustained_pressure);
    RUN_TEST(test_alert_distance_tuning_expands_match_gate);
    RUN_TEST(test_active_alert_times_out_when_pass_distance_not_met);
    return UNITY_END();
}
