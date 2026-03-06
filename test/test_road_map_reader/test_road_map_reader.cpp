// =============================================================================
// RoadMapReader camera fixture validation
//
// Loads the Phase 0 camera fixture (test/fixtures/camera_types_road_map.bin)
// through the real RoadMapReader path and verifies that all 4 camera types
// are queryable via nearestCamera().
// =============================================================================

#include <unity.h>
#include <cstdio>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Include the real implementation (native build, test_build_src = false).
#include "../../src/modules/lockout/road_map_reader.h"
#include "../../src/modules/lockout/road_map_reader.cpp"

// ---------------------------------------------------------------------------
// Fixture loading — native fopen, not SD
// ---------------------------------------------------------------------------

static uint8_t* fixtureData = nullptr;
static uint32_t fixtureSize = 0;

static bool loadFixtureFile(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    fixtureSize = static_cast<uint32_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    fixtureData = new uint8_t[fixtureSize];
    size_t n = fread(fixtureData, 1, fixtureSize, f);
    fclose(f);
    return n == fixtureSize;
}

// ---------------------------------------------------------------------------
// Fixture camera positions (E5) — must match generate_camera_fixture.py
// ---------------------------------------------------------------------------

// Camera 0: speed  (39.7400, -104.9900)  → (3974000, -10499000)
// Camera 1: red_lt (39.7420, -104.9910)  → (3974200, -10499100)
// Camera 2: bus_ln (39.7440, -104.9895)  → (3974400, -10498950)
// Camera 3: ALPR   (39.7460, -104.9905)  → (3974600, -10499050)

// Query coordinates very close to each camera so nearestCamera returns that one.
static constexpr int32_t CAM0_LAT = 3974000;
static constexpr int32_t CAM0_LON = -10499000;
static constexpr int32_t CAM1_LAT = 3974200;
static constexpr int32_t CAM1_LON = -10499100;
static constexpr int32_t CAM2_LAT = 3974400;
static constexpr int32_t CAM2_LON = -10498950;
static constexpr int32_t CAM3_LAT = 3974600;
static constexpr int32_t CAM3_LON = -10499050;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

void test_loadFromBuffer_succeeds() {
    RoadMapReader reader;
    TEST_ASSERT_TRUE_MESSAGE(
        reader.loadFromBuffer(fixtureData, fixtureSize),
        "loadFromBuffer should return true for valid fixture");
    TEST_ASSERT_TRUE(reader.isLoaded());
}

void test_camera_count_is_4() {
    RoadMapReader reader;
    reader.loadFromBuffer(fixtureData, fixtureSize);
    TEST_ASSERT_EQUAL_UINT32(4, reader.cameraCount());
}

void test_segment_count_is_1() {
    RoadMapReader reader;
    reader.loadFromBuffer(fixtureData, fixtureSize);
    TEST_ASSERT_EQUAL_UINT32(1, reader.segmentCount());
}

void test_nearest_camera_type_speed() {
    RoadMapReader reader;
    reader.loadFromBuffer(fixtureData, fixtureSize);
    CameraResult r = reader.nearestCamera(CAM0_LAT, CAM0_LON, 900);
    TEST_ASSERT_TRUE_MESSAGE(r.valid, "speed camera not found");
    TEST_ASSERT_EQUAL_UINT8(1, r.flags);
    TEST_ASSERT_EQUAL_UINT8(45, r.speedMph);
    TEST_ASSERT_EQUAL_UINT16(0, r.bearing);
}

void test_nearest_camera_type_red_light() {
    RoadMapReader reader;
    reader.loadFromBuffer(fixtureData, fixtureSize);
    CameraResult r = reader.nearestCamera(CAM1_LAT, CAM1_LON, 900);
    TEST_ASSERT_TRUE_MESSAGE(r.valid, "red_light camera not found");
    TEST_ASSERT_EQUAL_UINT8(2, r.flags);
    TEST_ASSERT_EQUAL_UINT8(0, r.speedMph);
    TEST_ASSERT_EQUAL_UINT16(180, r.bearing);
}

void test_nearest_camera_type_bus_lane() {
    RoadMapReader reader;
    reader.loadFromBuffer(fixtureData, fixtureSize);
    CameraResult r = reader.nearestCamera(CAM2_LAT, CAM2_LON, 900);
    TEST_ASSERT_TRUE_MESSAGE(r.valid, "bus_lane camera not found");
    TEST_ASSERT_EQUAL_UINT8(3, r.flags);
    TEST_ASSERT_EQUAL_UINT16(90, r.bearing);
}

void test_nearest_camera_type_alpr() {
    RoadMapReader reader;
    reader.loadFromBuffer(fixtureData, fixtureSize);
    CameraResult r = reader.nearestCamera(CAM3_LAT, CAM3_LON, 900);
    TEST_ASSERT_TRUE_MESSAGE(r.valid, "ALPR camera not found");
    TEST_ASSERT_EQUAL_UINT8(4, r.flags);
    TEST_ASSERT_EQUAL_UINT8(30, r.speedMph);
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, r.bearing);
}

void test_no_camera_outside_grid() {
    RoadMapReader reader;
    reader.loadFromBuffer(fixtureData, fixtureSize);
    // Query far from any camera (near equator)
    CameraResult r = reader.nearestCamera(0, 0, 900);
    TEST_ASSERT_FALSE(r.valid);
}

void test_loadFromBuffer_rejects_bad_magic() {
    uint8_t bad[145];
    memcpy(bad, fixtureData, fixtureSize);
    bad[0] = 'X';  // corrupt magic
    RoadMapReader reader;
    TEST_ASSERT_FALSE(reader.loadFromBuffer(bad, fixtureSize));
    TEST_ASSERT_FALSE(reader.isLoaded());
}

void test_loadFromBuffer_rejects_short_buffer() {
    RoadMapReader reader;
    TEST_ASSERT_FALSE(reader.loadFromBuffer(fixtureData, 32));
}

void test_failed_reload_clears_prior_state() {
    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixtureData, fixtureSize));

    uint8_t bad[145];
    memcpy(bad, fixtureData, fixtureSize);
    bad[0] = 'X';  // corrupt magic after a valid load

    TEST_ASSERT_FALSE(reader.loadFromBuffer(bad, fixtureSize));
    TEST_ASSERT_FALSE(reader.isLoaded());
    TEST_ASSERT_EQUAL_UINT32(0, reader.cameraCount());

    CameraResult r = reader.nearestCamera(CAM0_LAT, CAM0_LON, 900);
    TEST_ASSERT_FALSE(r.valid);
}

void test_reload_without_camera_section_clears_camera_lookup() {
    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixtureData, fixtureSize));

    uint8_t noCams[145];
    memcpy(noCams, fixtureData, fixtureSize);
    memset(noCams + 56, 0, 8);  // cameraIndexOffset = 0, cameraCount = 0

    TEST_ASSERT_TRUE(reader.loadFromBuffer(noCams, fixtureSize));
    TEST_ASSERT_TRUE(reader.isLoaded());
    TEST_ASSERT_EQUAL_UINT32(0, reader.cameraCount());

    CameraResult r = reader.nearestCamera(CAM0_LAT, CAM0_LON, 900);
    TEST_ASSERT_FALSE(r.valid);
}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

int main() {
    // Load fixture once — shared across all tests.
    if (!loadFixtureFile("test/fixtures/camera_types_road_map.bin")) {
        // PlatformIO native runner may cwd to project root or .pio/build/native
        if (!loadFixtureFile("../../test/fixtures/camera_types_road_map.bin")) {
            fprintf(stderr, "FATAL: cannot open camera fixture\n");
            return 1;
        }
    }

    UNITY_BEGIN();
    RUN_TEST(test_loadFromBuffer_succeeds);
    RUN_TEST(test_camera_count_is_4);
    RUN_TEST(test_segment_count_is_1);
    RUN_TEST(test_nearest_camera_type_speed);
    RUN_TEST(test_nearest_camera_type_red_light);
    RUN_TEST(test_nearest_camera_type_bus_lane);
    RUN_TEST(test_nearest_camera_type_alpr);
    RUN_TEST(test_no_camera_outside_grid);
    RUN_TEST(test_loadFromBuffer_rejects_bad_magic);
    RUN_TEST(test_loadFromBuffer_rejects_short_buffer);
    RUN_TEST(test_failed_reload_clears_prior_state);
    RUN_TEST(test_reload_without_camera_section_clears_camera_lookup);
    int result = UNITY_END();

    delete[] fixtureData;
    return result;
}
