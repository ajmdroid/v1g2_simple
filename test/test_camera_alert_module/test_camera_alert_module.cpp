#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

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

struct TestCameraSpec {
    int32_t latE5;
    int32_t lonE5;
    uint16_t bearing;
    uint8_t flags;
    uint8_t speedMph;
};

float cosLatForTest(int32_t latE5) {
    return cosf(static_cast<float>(latE5) / 100000.0f * (TEST_PI_F / 180.0f));
}

int32_t metresNorthToE5(float metresNorth) {
    return static_cast<int32_t>(lroundf(metresNorth / 1.11f));
}

int32_t metresEastToE5(int32_t refLatE5, float metresEast) {
    return static_cast<int32_t>(lroundf(metresEast / (1.11f * cosLatForTest(refLatE5))));
}

int32_t offsetLatE5(int32_t latE5, float metresNorth) {
    return latE5 + metresNorthToE5(metresNorth);
}

int32_t offsetLonE5(int32_t latE5, int32_t lonE5, float metresEast) {
    return lonE5 + metresEastToE5(latE5, metresEast);
}

std::vector<uint8_t> buildCameraMap(const std::vector<TestCameraSpec>& cameras) {
    constexpr uint32_t kHeaderSize = sizeof(RoadMapHeader);
    constexpr uint32_t kGridSize = sizeof(RoadMapGridEntry);

    const uint32_t gridIndexOffset = kHeaderSize;
    const uint32_t segDataOffset = gridIndexOffset + kGridSize;
    const uint32_t cameraIndexOffset = segDataOffset;
    const uint32_t cameraDataOffset = cameraIndexOffset + ((cameras.empty()) ? 0u : kGridSize);
    const uint32_t fileSize = cameraDataOffset +
                              static_cast<uint32_t>(cameras.size()) * sizeof(CameraRecord);

    RoadMapHeader header{};
    memcpy(header.magic, "RMAP", 4);
    header.version = 2;
    header.roadClassCount = 1;
    header.minLatE5 = BASE_LAT_E5 - 10000;
    header.maxLatE5 = BASE_LAT_E5 + 10000;
    header.minLonE5 = BASE_LON_E5 - 10000;
    header.maxLonE5 = BASE_LON_E5 + 10000;
    header.gridRows = 1;
    header.gridCols = 1;
    header.cellSizeE5 = 50000;
    header.totalSegments = 0;
    header.totalPoints = 0;
    header.toleranceCm = 10000;
    header.gridIndexOffset = gridIndexOffset;
    header.segDataOffset = segDataOffset;
    header.fileSize = fileSize;
    header.cameraIndexOffset = cameras.empty() ? 0 : cameraIndexOffset;
    header.cameraCount = static_cast<uint32_t>(cameras.size());

    RoadMapGridEntry roadGrid{};
    RoadMapGridEntry cameraGrid{};
    cameraGrid.segCount = static_cast<uint16_t>(cameras.size());

    std::vector<uint8_t> buffer(fileSize, 0);
    memcpy(buffer.data(), &header, sizeof(header));
    memcpy(buffer.data() + gridIndexOffset, &roadGrid, sizeof(roadGrid));
    if (!cameras.empty()) {
        memcpy(buffer.data() + cameraIndexOffset, &cameraGrid, sizeof(cameraGrid));
        uint8_t* writePtr = buffer.data() + cameraDataOffset;
        for (const TestCameraSpec& camera : cameras) {
            CameraRecord record{};
            record.latE5 = camera.latE5;
            record.lonE5 = camera.lonE5;
            record.bearing = camera.bearing;
            record.flags = camera.flags;
            record.speedMph = camera.speedMph;
            memcpy(writePtr, &record, sizeof(record));
            writePtr += sizeof(record);
        }
    }

    return buffer;
}

CameraAlertContext makeContext(int32_t latE5, int32_t lonE5, float speedMph = 35.0f,
                               bool courseValid = true, float courseDeg = 0.0f,
                               uint32_t courseAgeMs = 0) {
    CameraAlertContext ctx;
    ctx.gpsValid = true;
    ctx.latE5 = latE5;
    ctx.lonE5 = lonE5;
    ctx.speedMph = speedMph;
    ctx.courseValid = courseValid;
    ctx.courseDeg = courseDeg;
    ctx.courseAgeMs = courseAgeMs;
    return ctx;
}

void processAt(CameraAlertModule& module, uint32_t nowMs, const CameraAlertContext& ctx) {
    mockMillis = nowMs;
    module.process(nowMs, ctx);
}

CameraAlertModule makeModule(RoadMapReader& reader, SettingsManager& settings) {
    CameraAlertModule module;
    module.begin(&reader, &settings);
    return module;
}

}  // namespace

void test_unknown_flag_is_ignored() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, 9, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));

    CameraVoiceEvent event;
    TEST_ASSERT_FALSE(module.isDisplayActive());
    TEST_ASSERT_FALSE(module.consumePendingVoice(event));
}

void test_below_min_speed_clears_alerts() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5, 10.0f));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5, 10.0f));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5, 10.0f));

    TEST_ASSERT_FALSE(module.isDisplayActive());
    TEST_ASSERT_FALSE(module.displayPayload().active);
}

void test_corridor_rejects_side_road_camera() {
    const int32_t camLat = offsetLatE5(BASE_LAT_E5, 200.0f);
    const int32_t camLon = offsetLonE5(camLat, BASE_LON_E5, 65.0f);
    const TestCameraSpec camera{camLat, camLon, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -200.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -150.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -100.0f), BASE_LON_E5));

    TEST_ASSERT_FALSE(module.isDisplayActive());
}

void test_closing_confirmation_requires_two_closing_polls() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 200.0f), offsetLonE5(BASE_LAT_E5, BASE_LON_E5, 5.0f), 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
    TEST_ASSERT_FALSE(module.isDisplayActive());

    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
    TEST_ASSERT_FALSE(module.isDisplayActive());

    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));
    TEST_ASSERT_TRUE(module.isDisplayActive());
    TEST_ASSERT_EQUAL(CameraType::SPEED, module.displayPayload().type);
}

void test_same_coordinates_different_flags_start_new_encounter() {
    const int32_t camLat = offsetLatE5(BASE_LAT_E5, 200.0f);
    const int32_t camLon = BASE_LON_E5;
    const TestCameraSpec speedCamera{camLat, camLon, 0, 1, 45};
    const TestCameraSpec redLightCamera{camLat, camLon, 0, 2, 0};

    std::vector<uint8_t> speedMap = buildCameraMap({speedCamera});
    std::vector<uint8_t> redLightMap = buildCameraMap({redLightCamera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(speedMap.data(), static_cast<uint32_t>(speedMap.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), camLon));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), camLon));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), camLon));
    TEST_ASSERT_TRUE(module.isDisplayActive());
    TEST_ASSERT_EQUAL(CameraType::SPEED, module.displayPayload().type);

    TEST_ASSERT_TRUE(reader.loadFromBuffer(redLightMap.data(), static_cast<uint32_t>(redLightMap.size())));

    processAt(module, 2000, makeContext(offsetLatE5(BASE_LAT_E5, -10.0f), camLon));
    TEST_ASSERT_FALSE(module.isDisplayActive());

    processAt(module, 2500, makeContext(offsetLatE5(BASE_LAT_E5, 40.0f), camLon));
    TEST_ASSERT_FALSE(module.isDisplayActive());

    processAt(module, 3000, makeContext(offsetLatE5(BASE_LAT_E5, 90.0f), camLon));
    TEST_ASSERT_TRUE(module.isDisplayActive());
    TEST_ASSERT_EQUAL(CameraType::RED_LIGHT, module.displayPayload().type);
}

void test_driving_away_clears_confirmed_display() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));
    TEST_ASSERT_TRUE(module.isDisplayActive());

    processAt(module, 2000, makeContext(offsetLatE5(BASE_LAT_E5, -90.0f), BASE_LON_E5));
    TEST_ASSERT_FALSE(module.isDisplayActive());
}

void test_raw_course_fallback_confirms_when_breadcrumbs_are_too_close() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 100.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -120.0f), BASE_LON_E5, 35.0f, true, 0.0f));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -115.0f), BASE_LON_E5, 35.0f, true, 0.0f));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5, 35.0f, true, 0.0f));

    TEST_ASSERT_TRUE(module.isDisplayActive());
    TEST_ASSERT_EQUAL(CameraType::SPEED, module.displayPayload().type);
}

void test_far_voice_queues_once_after_confirmation() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));

    CameraVoiceEvent event;
    TEST_ASSERT_TRUE(module.consumePendingVoice(event));
    TEST_ASSERT_EQUAL(CameraType::SPEED, event.type);
    TEST_ASSERT_FALSE(event.isNearStage);
    module.onVoicePlaybackResult(event, true);

    processAt(module, 2000, makeContext(offsetLatE5(BASE_LAT_E5, -10.0f), BASE_LON_E5));
    TEST_ASSERT_FALSE(module.consumePendingVoice(event));
}

void test_near_voice_wins_when_first_confirmation_is_close() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 100.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -120.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -80.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -40.0f), BASE_LON_E5));

    CameraVoiceEvent event;
    TEST_ASSERT_TRUE(module.consumePendingVoice(event));
    TEST_ASSERT_EQUAL(CameraType::SPEED, event.type);
    TEST_ASSERT_TRUE(event.isNearStage);
    module.onVoicePlaybackResult(event, true);
}

void test_configured_close_alert_range_can_force_far_stage_first() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 100.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    settings.settings.cameraAlertNearRangeCm = 10000;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -120.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -80.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -40.0f), BASE_LON_E5));

    CameraVoiceEvent event;
    TEST_ASSERT_TRUE(module.consumePendingVoice(event));
    TEST_ASSERT_EQUAL(CameraType::SPEED, event.type);
    TEST_ASSERT_FALSE(event.isNearStage);
}

void test_voice_stage_requeues_until_playback_starts() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));

    CameraVoiceEvent event;
    TEST_ASSERT_TRUE(module.consumePendingVoice(event));
    module.onVoicePlaybackResult(event, false);

    CameraVoiceEvent retry;
    TEST_ASSERT_TRUE(module.consumePendingVoice(retry));
    TEST_ASSERT_EQUAL(event.type, retry.type);
    TEST_ASSERT_EQUAL(event.isNearStage, retry.isNearStage);

    module.onVoicePlaybackResult(retry, true);
    TEST_ASSERT_FALSE(module.consumePendingVoice(retry));
}

void test_module_keeps_distance_above_legacy_uint16_cap() {
    const TestCameraSpec camera{
        offsetLatE5(BASE_LAT_E5, 900.0f), BASE_LON_E5, 0, 1, 45};
    std::vector<uint8_t> mapData = buildCameraMap({camera});

    RoadMapReader reader;
    SettingsManager settings;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
    CameraAlertModule module = makeModule(reader, settings);

    processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -150.0f), BASE_LON_E5));
    processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -100.0f), BASE_LON_E5));
    processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -50.0f), BASE_LON_E5));

    TEST_ASSERT_TRUE(module.isDisplayActive());
    TEST_ASSERT_TRUE(module.displayPayload().distanceCm > 65534u);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unknown_flag_is_ignored);
    RUN_TEST(test_below_min_speed_clears_alerts);
    RUN_TEST(test_corridor_rejects_side_road_camera);
    RUN_TEST(test_closing_confirmation_requires_two_closing_polls);
    RUN_TEST(test_same_coordinates_different_flags_start_new_encounter);
    RUN_TEST(test_driving_away_clears_confirmed_display);
    RUN_TEST(test_raw_course_fallback_confirms_when_breadcrumbs_are_too_close);
    RUN_TEST(test_far_voice_queues_once_after_confirmation);
    RUN_TEST(test_near_voice_wins_when_first_confirmation_is_close);
    RUN_TEST(test_configured_close_alert_range_can_force_far_stage_first);
    RUN_TEST(test_voice_stage_requeues_until_playback_starts);
    RUN_TEST(test_module_keeps_distance_above_legacy_uint16_cap);
    return UNITY_END();
}
