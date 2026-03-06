#include <unity.h>

#include <cmath>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/lockout/road_map_reader.h"
#include "../../src/modules/lockout/road_map_reader.cpp"
#include "../../src/modules/camera_alert/camera_alert_module.h"
#include "../../src/modules/camera_alert/camera_alert_module.cpp"

namespace {

constexpr int32_t BASE_LAT_E5 = 3974000;
constexpr int32_t BASE_LON_E5 = -10499000;
constexpr float TEST_PI_F = 3.14159265f;

int32_t metresNorthToE5(float metresNorth) {
    return static_cast<int32_t>(lroundf(metresNorth / 1.11f));
}

float cosLatForTest(int32_t latE5) {
    return cosf(static_cast<float>(latE5) / 100000.0f * (TEST_PI_F / 180.0f));
}

int32_t metresEastToE5(int32_t refLatE5, float metresEast) {
    return static_cast<int32_t>(lroundf(metresEast / (1.11f * cosLatForTest(refLatE5))));
}

}  // namespace

void test_heading_delta_wraps_across_zero() {
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, headingDeltaDeg(359.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.0f, headingDeltaDeg(45.0f, 315.0f));
}

void test_bearing_matches_cardinal_directions() {
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 0.0f,
                             bearingDeg(BASE_LAT_E5, BASE_LON_E5,
                                        BASE_LAT_E5 + metresNorthToE5(100.0f), BASE_LON_E5));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 90.0f,
                             bearingDeg(BASE_LAT_E5, BASE_LON_E5,
                                        BASE_LAT_E5,
                                        BASE_LON_E5 + metresEastToE5(BASE_LAT_E5, 100.0f)));
}

void test_forward_corridor_accepts_camera_ahead() {
    const int32_t camLat = BASE_LAT_E5 + metresNorthToE5(150.0f);
    const int32_t camLon = BASE_LON_E5 + metresEastToE5(camLat, 10.0f);
    TEST_ASSERT_TRUE(cameraInForwardCorridor(BASE_LAT_E5, BASE_LON_E5, camLat, camLon, 0.0f));
}

void test_forward_corridor_rejects_camera_behind_or_too_wide() {
    const int32_t behindLat = BASE_LAT_E5 - metresNorthToE5(50.0f);
    TEST_ASSERT_FALSE(cameraInForwardCorridor(BASE_LAT_E5, BASE_LON_E5, behindLat, BASE_LON_E5, 0.0f));

    const int32_t wideLat = BASE_LAT_E5 + metresNorthToE5(150.0f);
    const int32_t wideLon = BASE_LON_E5 + metresEastToE5(wideLat, 70.0f);
    TEST_ASSERT_FALSE(cameraInForwardCorridor(BASE_LAT_E5, BASE_LON_E5, wideLat, wideLon, 0.0f));
}

void test_camera_bearing_gate_respects_enforcement_heading() {
    TEST_ASSERT_TRUE(cameraBearingMatches(0.0f, 0));
    TEST_ASSERT_TRUE(cameraBearingMatches(45.0f, 90));
    TEST_ASSERT_FALSE(cameraBearingMatches(0.0f, 225));
    TEST_ASSERT_TRUE(cameraBearingMatches(180.0f, 0xFFFF));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_heading_delta_wraps_across_zero);
    RUN_TEST(test_bearing_matches_cardinal_directions);
    RUN_TEST(test_forward_corridor_accepts_camera_ahead);
    RUN_TEST(test_forward_corridor_rejects_camera_behind_or_too_wide);
    RUN_TEST(test_camera_bearing_gate_respects_enforcement_heading);
    return UNITY_END();
}
