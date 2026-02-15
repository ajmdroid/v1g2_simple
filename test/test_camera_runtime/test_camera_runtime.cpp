#include <unity.h>

#include <utility>

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
                       uint8_t type) {
    resetReadyBuffers();

    gReadyBuffers.records = static_cast<CameraRecord*>(
        heap_caps_malloc(sizeof(CameraRecord), MALLOC_CAP_8BIT));
    gReadyBuffers.spans = static_cast<CameraCellSpan*>(
        heap_caps_malloc(sizeof(CameraCellSpan), MALLOC_CAP_8BIT));
    TEST_ASSERT_NOT_NULL(gReadyBuffers.records);
    TEST_ASSERT_NOT_NULL(gReadyBuffers.spans);

    CameraRecord& record = gReadyBuffers.records[0];
    record.latitudeDeg = latitudeDeg;
    record.longitudeDeg = longitudeDeg;
    record.snapLatitudeDeg = latitudeDeg;
    record.snapLongitudeDeg = longitudeDeg;
    record.bearingTenthsDeg = bearingTenthsDeg;
    record.widthM = 25;
    record.toleranceDeg = toleranceDeg;
    record.type = type;
    record.speedLimit = 35;
    record.flags = 0;
    record.reserved = 0;
    record.cellKey = CameraIndex::encodeCellKey(latitudeDeg, longitudeDeg);

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

void processCameraTick(uint32_t nowMs) {
    mockMillis = nowMs;
    mockMicros = static_cast<unsigned long>(nowMs) * 1000UL;
    cameraRuntimeModule.process(nowMs, false, false);
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
    gpsRuntimeModule = GpsRuntimeModule();
    gpsRuntimeModule.begin(true);
    cameraRuntimeModule.begin(true);
    resetReadyBuffers();
}

void tearDown() {
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

    cameraRuntimeModule.notifySignalPreempted(1010);
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_forward_only_start_requires_heading_alignment);
    RUN_TEST(test_lifecycle_clears_on_pass_distance_and_lifts_after_exit);
    RUN_TEST(test_lifecycle_clears_on_turn_away_after_two_ticks);
    RUN_TEST(test_preempt_suppresses_same_pass_until_exit_then_allows_reentry);
    return UNITY_END();
}
