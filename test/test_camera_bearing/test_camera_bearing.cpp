#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"
#include "../mocks/modules/gps/gps_runtime_module.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SettingsManager settingsManager;
#endif

#include "../../src/modules/camera_alert/camera_alert_module.cpp"

namespace {

CameraResult gCameraResult;
uint32_t gCameraCount = 1;

CameraResult nearestCameraStub(void*, int32_t, int32_t, uint16_t) {
    return gCameraResult;
}

uint32_t cameraCountStub(void*) {
    return gCameraCount;
}

GpsRuntimeStatus makeGps(float courseDeg,
                         bool courseValid = true,
                         uint32_t courseAgeMs = 100,
                         float latDeg = 40.0f,
                         float lonDeg = -75.0f) {
    GpsRuntimeStatus gps;
    gps.enabled = true;
    gps.hasFix = true;
    gps.stableHasFix = true;
    gps.locationValid = true;
    gps.latitudeDeg = latDeg;
    gps.longitudeDeg = lonDeg;
    gps.courseValid = courseValid;
    gps.courseDeg = courseDeg;
    gps.courseAgeMs = courseAgeMs;
    return gps;
}

CameraAlertResult processOnce(CameraAlertModule& module,
                              const GpsRuntimeStatus& gps,
                              bool v1Suppressed = false) {
    CameraAlertContext ctx;
    ctx.settings = &settingsManager.settings;
    ctx.gpsStatus = &gps;
    ctx.v1SignalPriorityActive = v1Suppressed;
    return module.process(1000, ctx);
}

}  // namespace

void setUp() {
    settingsManager = SettingsManager{};
    settingsManager.settings.cameraAlertsEnabled = true;
    settingsManager.settings.cameraAlertRangeM = 805;
    settingsManager.settings.cameraTypeSpeed = true;
    settingsManager.settings.cameraTypeRedLight = true;
    settingsManager.settings.cameraTypeBusLane = true;
    settingsManager.settings.cameraTypeAlpr = true;
    gCameraCount = 1;
    gCameraResult = CameraResult{};
    gCameraResult.valid = true;
    gCameraResult.flags = 1;  // speed
    gCameraResult.distanceCm = 5000;
}

void tearDown() {}

void test_camera_directly_ahead() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 90;
    gCameraResult.latE5 = 4000000;
    gCameraResult.lonE5 = -7490000;  // east of vehicle

    const auto result = processOnce(module, makeGps(90.0f));
    TEST_ASSERT_TRUE(result.displayActive);
}

void test_camera_behind() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 90;
    gCameraResult.latE5 = 4000000;
    gCameraResult.lonE5 = -7510000;  // west of vehicle

    const auto result = processOnce(module, makeGps(90.0f));
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_camera_90deg_left() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 0;
    gCameraResult.latE5 = 4100000;  // north of vehicle
    gCameraResult.lonE5 = -7500000;

    const auto result = processOnce(module, makeGps(90.0f));
    TEST_ASSERT_TRUE(result.displayActive);
}

void test_camera_91deg_past() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 0;
    gCameraResult.latE5 = 4100000;
    gCameraResult.lonE5 = -7500000;

    const auto result = processOnce(module, makeGps(91.1f));
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_bearing_unknown() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 0xFFFF;
    gCameraResult.latE5 = 4000000;
    gCameraResult.lonE5 = -7490000;

    const auto result = processOnce(module, makeGps(90.0f));
    TEST_ASSERT_TRUE(result.displayActive);
}

void test_course_invalid() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 90;
    gCameraResult.latE5 = 4000000;
    gCameraResult.lonE5 = -7510000;  // behind, but course invalid => conservative allow

    const auto result = processOnce(module, makeGps(90.0f, false));
    TEST_ASSERT_TRUE(result.displayActive);
}

void test_wraparound_359_to_1() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 359;
    gCameraResult.latE5 = 4100000;
    gCameraResult.lonE5 = -7500000;

    const auto result = processOnce(module, makeGps(1.0f));
    TEST_ASSERT_TRUE(result.displayActive);
}

void test_wraparound_1_to_359() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.bearing = 1;
    gCameraResult.latE5 = 4100000;
    gCameraResult.lonE5 = -7500000;

    const auto result = processOnce(module, makeGps(359.0f));
    TEST_ASSERT_TRUE(result.displayActive);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_camera_directly_ahead);
    RUN_TEST(test_camera_behind);
    RUN_TEST(test_camera_90deg_left);
    RUN_TEST(test_camera_91deg_past);
    RUN_TEST(test_bearing_unknown);
    RUN_TEST(test_course_invalid);
    RUN_TEST(test_wraparound_359_to_1);
    RUN_TEST(test_wraparound_1_to_359);
    return UNITY_END();
}
