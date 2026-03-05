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
bool gHonorSearchRadius = true;

CameraResult nearestCameraStub(void*, int32_t, int32_t, uint16_t searchRadiusE5) {
    CameraResult out = gCameraResult;
    if (!out.valid || !gHonorSearchRadius) {
        return out;
    }
    const uint32_t radiusCm = static_cast<uint32_t>(searchRadiusE5) * 111U;
    if (out.distanceCm > radiusCm) {
        out.valid = false;
    }
    return out;
}

uint32_t cameraCountStub(void*) {
    return gCameraCount;
}

GpsRuntimeStatus makeGps(float courseDeg = 90.0f,
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

CameraAlertResult process(CameraAlertModule& module,
                          uint32_t nowMs,
                          const GpsRuntimeStatus& gps,
                          bool liveSuppress = false,
                          bool persistedSuppress = false) {
    CameraAlertContext ctx;
    ctx.settings = &settingsManager.settings;
    ctx.gpsStatus = &gps;
    ctx.v1SignalPriorityActive = liveSuppress;
    ctx.v1PersistedPriorityActive = persistedSuppress;
    return module.process(nowMs, ctx);
}

void configureAheadSpeedCamera(uint16_t distanceCm = 24384) {
    gCameraResult = CameraResult{};
    gCameraResult.valid = true;
    gCameraResult.flags = 1;  // speed
    gCameraResult.bearing = 90;
    gCameraResult.distanceCm = distanceCm;
    gCameraResult.latE5 = 4000000;
    gCameraResult.lonE5 = -7490000;  // east (ahead for course=90)
}

}  // namespace

void setUp() {
    settingsManager = SettingsManager{};
    settingsManager.settings.cameraAlertsEnabled = true;
    settingsManager.settings.cameraAlertRangeM = 805;
    settingsManager.settings.cameraTypeSpeed = true;
    settingsManager.settings.cameraTypeRedLight = true;
    settingsManager.settings.cameraTypeAlpr = true;
    settingsManager.settings.cameraTypeBusLane = false;
    settingsManager.settings.cameraVoiceEnabled = true;
    settingsManager.settings.cameraVoiceClose = true;
    gCameraCount = 1;
    gHonorSearchRadius = true;
    configureAheadSpeedCamera();
}

void tearDown() {}

void test_no_camera_no_alert() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    gCameraResult.valid = false;

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_FALSE(result.displayActive);
    CameraVoiceEvent voice;
    TEST_ASSERT_FALSE(module.consumePendingVoice(voice));
}

void test_camera_detected_within_range() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(24384);  // 800 ft

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_TRUE(result.displayActive);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(CameraType::SPEED),
                            static_cast<uint8_t>(result.payload.type));
}

void test_camera_detected_outside_range() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    settingsManager.settings.cameraAlertRangeM = 100;
    configureAheadSpeedCamera(15000);  // 150 m (outside configured range)

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_type_filter_speed() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    settingsManager.settings.cameraTypeSpeed = false;
    configureAheadSpeedCamera();

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_type_filter_alpr() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    settingsManager.settings.cameraTypeAlpr = false;
    configureAheadSpeedCamera();
    gCameraResult.flags = 4;

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_type_filter_red_light() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    settingsManager.settings.cameraTypeRedLight = false;
    configureAheadSpeedCamera();
    gCameraResult.flags = 2;

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_type_filter_bus_lane() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    settingsManager.settings.cameraTypeBusLane = false;
    configureAheadSpeedCamera();
    gCameraResult.flags = 3;

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_v1_alert_suppresses_camera() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera();

    const auto result = process(module, 1000, makeGps(), true, false);
    TEST_ASSERT_FALSE(result.displayActive);
    TEST_ASSERT_TRUE(result.suppressedByV1);
}

void test_v1_persisted_suppresses_camera() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera();

    const auto result = process(module, 1000, makeGps(), false, true);
    TEST_ASSERT_FALSE(result.displayActive);
    TEST_ASSERT_TRUE(result.suppressedByV1);
}

void test_camera_resumes_after_v1_clears() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera();

    process(module, 1000, makeGps(), true, false);
    const auto resumed = process(module, 1600, makeGps(), false, false);
    TEST_ASSERT_TRUE(resumed.displayActive);
}

void test_far_voice_fires_at_1000ft() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(30480);

    process(module, 1000, makeGps());
    CameraVoiceEvent voice;
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
    TEST_ASSERT_FALSE(voice.isNearStage);
}

void test_far_voice_no_repeat() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(30480);

    process(module, 1000, makeGps());
    CameraVoiceEvent voice;
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
    module.markVoiceAnnounced(voice);

    process(module, 2000, makeGps());
    TEST_ASSERT_FALSE(module.consumePendingVoice(voice));
}

void test_near_voice_fires_at_500ft() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    settingsManager.settings.cameraVoiceEnabled = false;
    settingsManager.settings.cameraVoiceClose = true;
    configureAheadSpeedCamera(15240);

    process(module, 1000, makeGps());
    CameraVoiceEvent voice;
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
    TEST_ASSERT_TRUE(voice.isNearStage);
}

void test_encounter_expires_after_10s() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(30480);

    process(module, 1000, makeGps());
    gCameraResult.valid = false;
    process(module, 11550, makeGps());

    configureAheadSpeedCamera(30480);
    process(module, 12100, makeGps());
    CameraVoiceEvent voice;
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
}

void test_new_camera_resets_encounter() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(30480);

    process(module, 1000, makeGps());
    CameraVoiceEvent voice;
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
    module.markVoiceAnnounced(voice);

    gCameraResult.latE5 = 4010000;
    gCameraResult.lonE5 = -7489000;
    process(module, 1600, makeGps());
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
}

void test_course_stale_shows_alert() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera();
    gCameraResult.lonE5 = -7510000;  // physically behind if course were fresh

    const auto result = process(module, 1000, makeGps(90.0f, true, 5000));
    TEST_ASSERT_TRUE(result.displayActive);
}

void test_same_latlon_different_flags_is_new_encounter() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(30480);

    process(module, 1000, makeGps());
    CameraVoiceEvent voice;
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
    module.markVoiceAnnounced(voice);

    gCameraResult.flags = 2;  // same coordinates, different type
    process(module, 1600, makeGps());
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
}

void test_audio_busy_retries_stage_without_losing_announcement() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(30480);

    process(module, 1000, makeGps());
    CameraVoiceEvent voice;
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
    // Simulate busy: do not mark announced.

    process(module, 1600, makeGps());
    TEST_ASSERT_TRUE(module.consumePendingVoice(voice));
    module.markVoiceAnnounced(voice);
    process(module, 2200, makeGps());
    TEST_ASSERT_FALSE(module.consumePendingVoice(voice));
}

void test_unknown_flag_ignored() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera();
    gCameraResult.flags = 99;

    const auto result = process(module, 1000, makeGps());
    TEST_ASSERT_FALSE(result.displayActive);
    CameraVoiceEvent voice;
    TEST_ASSERT_FALSE(module.consumePendingVoice(voice));
}

void test_stale_course_uses_motion_heading_to_clear_after_turn() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(12000);

    // Initial lock with fresh heading east.
    TEST_ASSERT_TRUE(process(module, 1000, makeGps(90.0f, true, 100, 40.0000f, -75.0000f)).displayActive);

    // Course becomes stale, but movement indicates westbound turn (away from camera).
    const auto result = process(module, 1600, makeGps(90.0f, true, 5000, 40.0000f, -75.0002f));
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_stale_course_diverging_distance_clears_after_grace() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(12000);

    // Start stale with no meaningful movement-derived heading.
    TEST_ASSERT_TRUE(process(module, 1000, makeGps(90.0f, true, 5000, 40.0000f, -75.0000f)).displayActive);

    // Distance steadily increases while stale and no heading available.
    gCameraResult.distanceCm = 13000;
    TEST_ASSERT_TRUE(process(module, 11500, makeGps(90.0f, true, 5000, 40.0000f, -75.0000f)).displayActive);
    gCameraResult.distanceCm = 19000;
    TEST_ASSERT_TRUE(process(module, 12100, makeGps(90.0f, true, 5000, 40.0000f, -75.0000f)).displayActive);
    gCameraResult.distanceCm = 19500;
    TEST_ASSERT_TRUE(process(module, 12700, makeGps(90.0f, true, 5000, 40.0000f, -75.0000f)).displayActive);
    gCameraResult.distanceCm = 20000;
    const auto result = process(module, 13300, makeGps(90.0f, true, 5000, 40.0000f, -75.0000f));
    TEST_ASSERT_FALSE(result.displayActive);
}

void test_stale_course_hard_timeout_clears_without_divergence() {
    CameraAlertModule module;
    module.begin(CameraAlertProviders{nearestCameraStub, cameraCountStub, nullptr});
    configureAheadSpeedCamera(12000);

    TEST_ASSERT_TRUE(process(module, 1000, makeGps(90.0f, true, 5000, 40.0000f, -75.0000f)).displayActive);
    const auto result = process(module, 32000, makeGps(90.0f, true, 5000, 40.0000f, -75.0000f));
    TEST_ASSERT_FALSE(result.displayActive);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_camera_no_alert);
    RUN_TEST(test_camera_detected_within_range);
    RUN_TEST(test_camera_detected_outside_range);
    RUN_TEST(test_type_filter_speed);
    RUN_TEST(test_type_filter_alpr);
    RUN_TEST(test_type_filter_red_light);
    RUN_TEST(test_type_filter_bus_lane);
    RUN_TEST(test_v1_alert_suppresses_camera);
    RUN_TEST(test_v1_persisted_suppresses_camera);
    RUN_TEST(test_camera_resumes_after_v1_clears);
    RUN_TEST(test_far_voice_fires_at_1000ft);
    RUN_TEST(test_far_voice_no_repeat);
    RUN_TEST(test_near_voice_fires_at_500ft);
    RUN_TEST(test_encounter_expires_after_10s);
    RUN_TEST(test_new_camera_resets_encounter);
    RUN_TEST(test_course_stale_shows_alert);
    RUN_TEST(test_same_latlon_different_flags_is_new_encounter);
    RUN_TEST(test_audio_busy_retries_stage_without_losing_announcement);
    RUN_TEST(test_unknown_flag_ignored);
    RUN_TEST(test_stale_course_uses_motion_heading_to_clear_after_turn);
    RUN_TEST(test_stale_course_diverging_distance_clears_after_grace);
    RUN_TEST(test_stale_course_hard_timeout_clears_without_divergence);
    return UNITY_END();
}
