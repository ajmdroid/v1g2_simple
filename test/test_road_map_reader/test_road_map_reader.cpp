#include <unity.h>

#include <cstring>
#include <vector>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/lockout/road_map_reader.h"
#include "../../src/modules/lockout/road_map_reader.cpp"

namespace {

constexpr int32_t kRoadLatStart = 3974500;
constexpr int32_t kRoadLatEnd = 3974700;
constexpr int32_t kRoadLon = -10499000;
constexpr int32_t kQueryLat = 3974600;
constexpr int32_t kQueryLon = -10499000;
constexpr int32_t kNearQueryLon = -10499020;
constexpr uint8_t kRoadClass = 2;
constexpr uint8_t kSpeedMph = 35;
constexpr uint16_t kToleranceCm = 200;

template <typename T>
void writeValue(uint8_t* dest, const T& value) {
    std::memcpy(dest, &value, sizeof(T));
}

std::vector<uint8_t> makeRoadMapFixture() {
    const uint32_t gridIndexOffset = sizeof(RoadMapHeader);
    const uint32_t segDataOffset = gridIndexOffset + sizeof(RoadMapGridEntry);
    const uint32_t segmentSize = 12 + 4 + 1;

    RoadMapHeader header{};
    std::memcpy(header.magic, "RMAP", 4);
    header.version = 2;
    header.roadClassCount = 1;
    header.minLatE5 = 3974400;
    header.maxLatE5 = 3974800;
    header.minLonE5 = -10499500;
    header.maxLonE5 = -10498500;
    header.gridRows = 1;
    header.gridCols = 1;
    header.cellSizeE5 = 1000;
    header.totalSegments = 1;
    header.totalPoints = 2;
    header.toleranceCm = kToleranceCm;
    header.gridIndexOffset = gridIndexOffset;
    header.segDataOffset = segDataOffset;
    header.fileSize = segDataOffset + segmentSize;

    std::vector<uint8_t> buffer(header.fileSize, 0);
    writeValue(buffer.data(), header);

    RoadMapGridEntry gridEntry{};
    gridEntry.dataOffset = 0;
    gridEntry.segCount = 1;
    writeValue(buffer.data() + gridIndexOffset, gridEntry);

    uint8_t* seg = buffer.data() + segDataOffset;
    seg[0] = kRoadClass;
    seg[1] = 0x01;  // oneway
    const uint16_t pointCount = 2;
    writeValue(seg + 2, pointCount);
    writeValue(seg + 4, kRoadLatStart);
    writeValue(seg + 8, kRoadLon);

    const int16_t deltaLat = static_cast<int16_t>(kRoadLatEnd - kRoadLatStart);
    const int16_t deltaLon = 0;
    writeValue(seg + 12, deltaLat);
    writeValue(seg + 14, deltaLon);
    seg[16] = kSpeedMph;

    return buffer;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_loadFromBuffer_succeeds() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();

    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));
    TEST_ASSERT_TRUE(reader.isLoaded());
}

void test_segment_count_is_1() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();

    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));
    TEST_ASSERT_EQUAL_UINT32(1, reader.segmentCount());
}

void test_snapToRoad_returns_expected_segment_metadata() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();

    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));

    const RoadSnapResult snap = reader.snapToRoad(kQueryLat, kQueryLon);
    TEST_ASSERT_TRUE(snap.valid);
    TEST_ASSERT_EQUAL_INT32(kQueryLat, snap.latE5);
    TEST_ASSERT_EQUAL_INT32(kQueryLon, snap.lonE5);
    TEST_ASSERT_EQUAL_UINT16(0, snap.headingDeg);
    TEST_ASSERT_EQUAL_UINT16(0, snap.distanceCm);
    TEST_ASSERT_EQUAL_UINT8(kRoadClass, snap.roadClass);
    TEST_ASSERT_EQUAL_UINT8(kSpeedMph, snap.speedMph);
    TEST_ASSERT_TRUE(snap.oneway);
}

void test_snapToRoad_near_segment_returns_clamped_distance() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();

    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));

    const RoadSnapResult snap = reader.snapToRoad(kQueryLat, kNearQueryLon, 100);
    TEST_ASSERT_TRUE(snap.valid);
    TEST_ASSERT_EQUAL_INT32(kQueryLat, snap.latE5);
    TEST_ASSERT_EQUAL_INT32(kRoadLon, snap.lonE5);
    TEST_ASSERT_TRUE(snap.distanceCm > 1000);
    TEST_ASSERT_TRUE(snap.distanceCm < 3000);
}

void test_snapToRoad_outside_bounds_returns_invalid() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();

    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));
    TEST_ASSERT_FALSE(reader.snapToRoad(0, 0, 100).valid);
}

void test_loadFromBuffer_rejects_bad_magic() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();
    fixture[0] = 'X';

    RoadMapReader reader;
    TEST_ASSERT_FALSE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));
    TEST_ASSERT_FALSE(reader.isLoaded());
}

void test_loadFromBuffer_rejects_short_buffer() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();

    RoadMapReader reader;
    TEST_ASSERT_FALSE(reader.loadFromBuffer(fixture.data(), 32));
}

void test_failed_reload_clears_prior_state() {
    std::vector<uint8_t> fixture = makeRoadMapFixture();

    RoadMapReader reader;
    TEST_ASSERT_TRUE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));

    fixture[0] = 'X';
    TEST_ASSERT_FALSE(reader.loadFromBuffer(fixture.data(), static_cast<uint32_t>(fixture.size())));
    TEST_ASSERT_FALSE(reader.isLoaded());
    TEST_ASSERT_EQUAL_UINT32(0, reader.segmentCount());
    TEST_ASSERT_FALSE(reader.snapToRoad(kQueryLat, kQueryLon, 100).valid);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_loadFromBuffer_succeeds);
    RUN_TEST(test_segment_count_is_1);
    RUN_TEST(test_snapToRoad_returns_expected_segment_metadata);
    RUN_TEST(test_snapToRoad_near_segment_returns_clamped_distance);
    RUN_TEST(test_snapToRoad_outside_bounds_returns_invalid);
    RUN_TEST(test_loadFromBuffer_rejects_bad_magic);
    RUN_TEST(test_loadFromBuffer_rejects_short_buffer);
    RUN_TEST(test_failed_reload_clears_prior_state);
    return UNITY_END();
}
