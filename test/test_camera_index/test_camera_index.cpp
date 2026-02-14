#include <unity.h>

#include <cmath>
#include <utility>

#include "../../src/modules/camera/camera_index.h"
#include "../../src/modules/camera/camera_index.cpp"  // Pull implementation for UNIT_TEST.

#include "esp_heap_caps.h"

namespace {
constexpr int32_t kExpectedLatitudeCellOffset = 9000;
constexpr int32_t kExpectedLongitudeCellOffset = 18000;

uint32_t expectedKeyFromCell(int32_t latCell, int32_t lonCell) {
    const int32_t clampedLat =
        std::clamp(latCell, -kExpectedLatitudeCellOffset, kExpectedLatitudeCellOffset);
    const int32_t clampedLon =
        std::clamp(lonCell, -kExpectedLongitudeCellOffset, kExpectedLongitudeCellOffset);
    const uint32_t latEncoded = static_cast<uint32_t>(clampedLat + kExpectedLatitudeCellOffset);
    const uint32_t lonEncoded = static_cast<uint32_t>(clampedLon + kExpectedLongitudeCellOffset);
    return (latEncoded << 16) | lonEncoded;
}
}  // namespace

void setUp() {}

void tearDown() {}

void test_encode_cell_key_returns_zero_for_non_finite_coordinates() {
    TEST_ASSERT_EQUAL_UINT32(0u, CameraIndex::encodeCellKey(NAN, -80.0f));
    TEST_ASSERT_EQUAL_UINT32(0u, CameraIndex::encodeCellKey(37.0f, INFINITY));
}

void test_encode_cell_key_matches_expected_for_us_coordinate() {
    const float latitudeDeg = 37.7749f;
    const float longitudeDeg = -122.4194f;
    const int32_t latCell = static_cast<int32_t>(std::floor(latitudeDeg / CameraIndex::kCellSizeDeg));
    const int32_t lonCell = static_cast<int32_t>(std::floor(longitudeDeg / CameraIndex::kCellSizeDeg));
    const uint32_t expected = expectedKeyFromCell(latCell, lonCell);

    TEST_ASSERT_EQUAL_UINT32(expected, CameraIndex::encodeCellKey(latitudeDeg, longitudeDeg));
}

void test_encode_cell_key_clamps_out_of_range_values() {
    const uint32_t expected = expectedKeyFromCell(9000, -18000);
    TEST_ASSERT_EQUAL_UINT32(expected, CameraIndex::encodeCellKey(123.4f, -250.0f));
}

void test_encode_cell_key_from_cell_matches_float_path() {
    const float latitudeDeg = -0.001f;
    const float longitudeDeg = -0.001f;
    const int32_t latCell = static_cast<int32_t>(std::floor(latitudeDeg / CameraIndex::kCellSizeDeg));
    const int32_t lonCell = static_cast<int32_t>(std::floor(longitudeDeg / CameraIndex::kCellSizeDeg));

    const uint32_t fromFloat = CameraIndex::encodeCellKey(latitudeDeg, longitudeDeg);
    const uint32_t fromCell = CameraIndex::encodeCellKeyFromCell(latCell, lonCell);
    TEST_ASSERT_EQUAL_UINT32(fromFloat, fromCell);
}

void test_adopt_and_release_round_trip() {
    CameraIndex index;
    CameraIndexOwnedBuffers buffers{};
    buffers.records = static_cast<CameraRecord*>(heap_caps_malloc(sizeof(CameraRecord) * 2u, MALLOC_CAP_8BIT));
    buffers.recordCount = 2;
    buffers.spans = static_cast<CameraCellSpan*>(heap_caps_malloc(sizeof(CameraCellSpan), MALLOC_CAP_8BIT));
    buffers.spanCount = 1;
    buffers.version = 7;

    TEST_ASSERT_NOT_NULL(buffers.records);
    TEST_ASSERT_NOT_NULL(buffers.spans);

    buffers.records[0].cellKey = 123;
    buffers.records[1].cellKey = 456;
    buffers.spans[0].cellKey = 123;
    buffers.spans[0].beginIndex = 0;
    buffers.spans[0].endIndex = 1;

    TEST_ASSERT_TRUE(index.adopt(std::move(buffers)));
    TEST_ASSERT_TRUE(index.isLoaded());
    TEST_ASSERT_EQUAL_UINT32(2, index.cameraCount());
    TEST_ASSERT_EQUAL_UINT32(1, index.bucketCount());
    TEST_ASSERT_EQUAL_UINT32(7, index.version());

    CameraIndexOwnedBuffers released = index.release();
    TEST_ASSERT_FALSE(index.isLoaded());
    TEST_ASSERT_EQUAL_UINT32(2, released.recordCount);
    TEST_ASSERT_EQUAL_UINT32(1, released.spanCount);
    TEST_ASSERT_EQUAL_UINT32(7, released.version);
    TEST_ASSERT_NOT_NULL(released.records);
    TEST_ASSERT_NOT_NULL(released.spans);

    CameraIndex::freeOwnedBuffers(released);
    TEST_ASSERT_NULL(released.records);
    TEST_ASSERT_NULL(released.spans);
}

void test_adopt_rejects_invalid_buffers() {
    CameraIndex index;
    CameraIndexOwnedBuffers invalid{};
    invalid.records = static_cast<CameraRecord*>(heap_caps_malloc(sizeof(CameraRecord), MALLOC_CAP_8BIT));
    invalid.recordCount = 1;
    invalid.spanCount = 1;
    invalid.spans = nullptr;

    TEST_ASSERT_NOT_NULL(invalid.records);
    TEST_ASSERT_FALSE(index.adopt(std::move(invalid)));
    TEST_ASSERT_FALSE(index.isLoaded());
    TEST_ASSERT_NULL(invalid.records);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_encode_cell_key_returns_zero_for_non_finite_coordinates);
    RUN_TEST(test_encode_cell_key_matches_expected_for_us_coordinate);
    RUN_TEST(test_encode_cell_key_clamps_out_of_range_values);
    RUN_TEST(test_encode_cell_key_from_cell_matches_float_path);
    RUN_TEST(test_adopt_and_release_round_trip);
    RUN_TEST(test_adopt_rejects_invalid_buffers);
    return UNITY_END();
}
