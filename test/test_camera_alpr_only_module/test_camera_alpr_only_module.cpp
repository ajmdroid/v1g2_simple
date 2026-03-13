#include <unity.h>

#include <vector>

#include "../mocks/Arduino.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/perf_metrics.h"
#include "../../src/modules/lockout/road_map_reader.cpp"
#include "../../src/modules/camera_alert/camera_alert_module.cpp"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordCameraProcessUs(uint32_t /*us*/) {}

namespace {

constexpr int32_t BASE_LAT_E5 = 3974000;
constexpr int32_t BASE_LON_E5 = -10499000;

struct TestCameraSpec {
	int32_t latE5;
	int32_t lonE5;
	uint16_t bearing;
	uint8_t flags;
	uint8_t speedMph;
};

int32_t metresNorthToE5(float metresNorth) {
	return static_cast<int32_t>(lroundf(metresNorth / 1.11f));
}

int32_t offsetLatE5(int32_t latE5, float metresNorth) {
	return latE5 + metresNorthToE5(metresNorth);
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
							   bool gpsValid = true) {
	CameraAlertContext ctx;
	ctx.gpsValid = gpsValid;
	ctx.latE5 = latE5;
	ctx.lonE5 = lonE5;
	ctx.speedMph = speedMph;
	ctx.courseValid = true;
	ctx.courseDeg = 0.0f;
	ctx.courseAgeMs = 0;
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

void setUp() {
	mockMillis = 1000;
	mockMicros = 1000000;
}

void tearDown() {}

void test_alpr_encounter_activates_after_confirmation() {
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

	TEST_ASSERT_TRUE(module.displayPayload().active);
}

void test_single_range_gate_rejects_out_of_range_camera() {
	const TestCameraSpec camera{
		offsetLatE5(BASE_LAT_E5, 1400.0f), BASE_LON_E5, 0, static_cast<uint8_t>(CameraType::ALPR), 45};
	std::vector<uint8_t> mapData = buildCameraMap({camera});
	RoadMapReader reader;
	SettingsManager settings;
	settings.settings.cameraAlertRangeCm = 30000;
	TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
	CameraAlertModule module = makeModule(reader, settings);

	processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
	processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
	processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));

	TEST_ASSERT_FALSE(module.displayPayload().active);
}

void test_non_alpr_flag_is_rejected() {
	const TestCameraSpec camera{
		offsetLatE5(BASE_LAT_E5, 200.0f), BASE_LON_E5, 0, 2, 45};
	std::vector<uint8_t> mapData = buildCameraMap({camera});
	RoadMapReader reader;
	SettingsManager settings;
	TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
	CameraAlertModule module = makeModule(reader, settings);

	processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
	processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
	processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));

	TEST_ASSERT_FALSE(module.displayPayload().active);
}

void test_farther_alpr_is_not_shadowed_by_nearer_legacy_camera() {
	const TestCameraSpec legacyCamera{
		offsetLatE5(BASE_LAT_E5, 170.0f), BASE_LON_E5, 0, 2, 45};
	const TestCameraSpec alprCamera{
		offsetLatE5(BASE_LAT_E5, 220.0f), BASE_LON_E5, 0, static_cast<uint8_t>(CameraType::ALPR), 45};
	std::vector<uint8_t> mapData = buildCameraMap({legacyCamera, alprCamera});
	RoadMapReader reader;
	SettingsManager settings;
	TEST_ASSERT_TRUE(reader.loadFromBuffer(mapData.data(), static_cast<uint32_t>(mapData.size())));
	CameraAlertModule module = makeModule(reader, settings);

	processAt(module, 500, makeContext(offsetLatE5(BASE_LAT_E5, -160.0f), BASE_LON_E5));
	processAt(module, 1000, makeContext(offsetLatE5(BASE_LAT_E5, -110.0f), BASE_LON_E5));
	processAt(module, 1500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));

	TEST_ASSERT_TRUE(module.displayPayload().active);
}

void test_clear_on_disable_gps_loss_and_low_speed() {
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
	TEST_ASSERT_TRUE(module.displayPayload().active);

	settings.settings.cameraAlertsEnabled = false;
	processAt(module, 2000, makeContext(offsetLatE5(BASE_LAT_E5, -10.0f), BASE_LON_E5));
	TEST_ASSERT_FALSE(module.displayPayload().active);

	settings.settings.cameraAlertsEnabled = true;
	processAt(module, 2500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));
	processAt(module, 3000, makeContext(offsetLatE5(BASE_LAT_E5, -10.0f), BASE_LON_E5));
	processAt(module, 3500, makeContext(offsetLatE5(BASE_LAT_E5, 40.0f), BASE_LON_E5));
	TEST_ASSERT_TRUE(module.displayPayload().active);

	processAt(module, 4000, makeContext(offsetLatE5(BASE_LAT_E5, 90.0f), BASE_LON_E5, 35.0f, false));
	TEST_ASSERT_FALSE(module.displayPayload().active);

	processAt(module, 4500, makeContext(offsetLatE5(BASE_LAT_E5, -60.0f), BASE_LON_E5));
	processAt(module, 5000, makeContext(offsetLatE5(BASE_LAT_E5, -10.0f), BASE_LON_E5));
	processAt(module, 5500, makeContext(offsetLatE5(BASE_LAT_E5, 40.0f), BASE_LON_E5));
	TEST_ASSERT_TRUE(module.displayPayload().active);

	processAt(module, 6000, makeContext(offsetLatE5(BASE_LAT_E5, 90.0f), BASE_LON_E5, 10.0f));
	TEST_ASSERT_FALSE(module.displayPayload().active);
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_alpr_encounter_activates_after_confirmation);
	RUN_TEST(test_single_range_gate_rejects_out_of_range_camera);
	RUN_TEST(test_non_alpr_flag_is_rejected);
	RUN_TEST(test_farther_alpr_is_not_shadowed_by_nearer_legacy_camera);
	RUN_TEST(test_clear_on_disable_gps_loss_and_low_speed);
	return UNITY_END();
}
