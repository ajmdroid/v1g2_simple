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

#include "../../src/perf_metrics.h"
#include "../../src/modules/lockout/road_map_reader.h"
#include "../../src/modules/lockout/road_map_reader.cpp"
#include "../../src/modules/camera_alert/camera_alert_module.h"
#include "../../src/modules/camera_alert/camera_alert_module.cpp"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordCameraProcessUs(uint32_t /*us*/) {}

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
	const uint32_t gridIndexOffset = sizeof(RoadMapHeader);
	const uint32_t gridSize = sizeof(RoadMapGridEntry);
	const uint32_t segDataOffset = gridIndexOffset + gridSize;
	const uint32_t cameraIndexOffset = segDataOffset;
	const uint32_t cameraDataOffset = cameraIndexOffset + ((cameras.empty()) ? 0u : gridSize);
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
							   bool gpsValid = true, bool courseValid = true, float courseDeg = 0.0f,
							   uint32_t courseAgeMs = 0) {
	CameraAlertContext ctx;
	ctx.gpsValid = gpsValid;
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
		offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, 1, 45};
	std::vector<uint8_t> mapData = buildCameraMap({camera});

	RoadMapReader reader;
	SettingsManager settings;
	TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
	CameraAlertModule module = makeModule(reader, settings);

	processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
	processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
	processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));

	TEST_ASSERT_FALSE(module.isDisplayActive());
	TEST_ASSERT_FALSE(module.displayPayload().active);
}

void test_below_min_speed_clears_alerts() {
	const TestCameraSpec camera{
		offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, static_cast<uint8_t>(CameraType::ALPR), 45};
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
	const TestCameraSpec camera{camLat, camLon, 0, static_cast<uint8_t>(CameraType::ALPR), 45};
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

void test_forward_corridor_rejects_exact_perpendicular_boundary() {
	const int32_t camLat = BASE_LAT_E5;
	const int32_t camLon = offsetLonE5(BASE_LAT_E5, BASE_LON_E5, 40.0f);

	TEST_ASSERT_FALSE(cameraInForwardCorridor(BASE_LAT_E5, BASE_LON_E5, camLat, camLon, 0.0f));
}

void test_closing_confirmation_requires_two_closing_polls_for_alpr() {
	const TestCameraSpec camera{
		offsetLatE5(BASE_LAT_E5, 200.0f), offsetLonE5(BASE_LAT_E5, BASE_LON_E5, 5.0f), 0,
		static_cast<uint8_t>(CameraType::ALPR), 45};
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
	TEST_ASSERT_TRUE(module.displayPayload().active);
	TEST_ASSERT_TRUE(module.displayPayload().distanceCm < CAMERA_DISTANCE_INVALID_CM);
}

void test_driving_away_clears_confirmed_display() {
	const TestCameraSpec camera{
		offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, static_cast<uint8_t>(CameraType::ALPR), 45};
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

void test_module_keeps_distance_above_legacy_uint16_cap() {
	const TestCameraSpec camera{
		offsetLatE5(BASE_LAT_E5, 900.0f), BASE_LON_E5, 0, static_cast<uint8_t>(CameraType::ALPR), 45};
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
	RUN_TEST(test_forward_corridor_rejects_exact_perpendicular_boundary);
	RUN_TEST(test_closing_confirmation_requires_two_closing_polls_for_alpr);
	RUN_TEST(test_driving_away_clears_confirmed_display);
	RUN_TEST(test_module_keeps_distance_above_legacy_uint16_cap);
	return UNITY_END();
}
