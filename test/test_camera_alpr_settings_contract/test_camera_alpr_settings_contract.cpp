#include <unity.h>

#include <filesystem>
#include <string>

#include "../../src/backup_payload_builder.cpp"
#include "../../src/v1_profiles.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
const int SD_BACKUP_VERSION = 11;
const int SETTINGS_VERSION = 8;

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
	return std::filesystem::temp_directory_path() /
		   ("camera_alpr_settings_contract_" + std::to_string(++g_tempRootIndex));
}

}  // namespace

void setUp() {
	mockMillis = 1000;
	mockMicros = 1000000;
	g_tempRoot = nextTempRoot();
	std::filesystem::remove_all(g_tempRoot);
	std::filesystem::create_directories(g_tempRoot);
}

void tearDown() {
	if (!g_tempRoot.empty()) {
		std::filesystem::remove_all(g_tempRoot);
	}
}

void test_defaults_and_schema_versions_match_alpr_contract() {
	V1Settings settings;

	TEST_ASSERT_EQUAL_INT(8, SETTINGS_VERSION);
	TEST_ASSERT_EQUAL_INT(11, SD_BACKUP_VERSION);
	TEST_ASSERT_TRUE(settings.cameraAlertsEnabled);
	TEST_ASSERT_EQUAL_UINT32(CAMERA_ALERT_RANGE_CM_DEFAULT, settings.cameraAlertRangeCm);
}

void test_backup_round_trip_keeps_only_alpr_camera_settings() {
	fs::FS fs(g_tempRoot);
	V1ProfileManager profileManager;
	TEST_ASSERT_TRUE(profileManager.begin(&fs));

	V1Settings settings;
	settings.cameraAlertsEnabled = false;
	settings.cameraAlertRangeCm = 43210;

	JsonDocument doc;
	BackupPayloadBuilder::buildBackupDocument(
		doc,
		settings,
		profileManager,
		BackupPayloadBuilder::BackupTransport::SdBackup,
		2222);

	TEST_ASSERT_EQUAL_INT(11, doc["_version"].as<int>());
	TEST_ASSERT_FALSE(doc["cameraAlertsEnabled"].as<bool>());
	TEST_ASSERT_EQUAL_INT(43210, doc["cameraAlertRangeCm"].as<int>());
	TEST_ASSERT_TRUE(doc["cameraAlertNearRangeCm"].isNull());
	TEST_ASSERT_TRUE(doc["cameraTypeAlpr"].isNull());
	TEST_ASSERT_TRUE(doc["cameraTypeRedLight"].isNull());
	TEST_ASSERT_TRUE(doc["cameraTypeSpeed"].isNull());
	TEST_ASSERT_TRUE(doc["cameraTypeBusLane"].isNull());
	TEST_ASSERT_TRUE(doc["colorCameraArrow"].isNull());
	TEST_ASSERT_TRUE(doc["colorCameraText"].isNull());
	TEST_ASSERT_TRUE(doc["cameraVoiceFarEnabled"].isNull());
	TEST_ASSERT_TRUE(doc["cameraVoiceNearEnabled"].isNull());
}

void test_serialized_backup_surface_round_trips_alpr_fields() {
	fs::FS fs(g_tempRoot);
	V1ProfileManager profileManager;
	TEST_ASSERT_TRUE(profileManager.begin(&fs));

	V1Settings settings;
	settings.cameraAlertsEnabled = true;
	settings.cameraAlertRangeCm = 55555;

	JsonDocument originalDoc;
	BackupPayloadBuilder::buildBackupDocument(
		originalDoc,
		settings,
		profileManager,
		BackupPayloadBuilder::BackupTransport::HttpDownload,
		3333);

	std::string payload;
	serializeJson(originalDoc, payload);

	JsonDocument reparsedDoc;
	const DeserializationError err = deserializeJson(reparsedDoc, payload);
	TEST_ASSERT_FALSE(err);
	TEST_ASSERT_TRUE(reparsedDoc["cameraAlertsEnabled"].as<bool>());
	TEST_ASSERT_EQUAL_INT(55555, reparsedDoc["cameraAlertRangeCm"].as<int>());
	TEST_ASSERT_TRUE(reparsedDoc["cameraAlertNearRangeCm"].isNull());
}

int main() {
	UNITY_BEGIN();
	RUN_TEST(test_defaults_and_schema_versions_match_alpr_contract);
	RUN_TEST(test_backup_round_trip_keeps_only_alpr_camera_settings);
	RUN_TEST(test_serialized_backup_surface_round_trips_alpr_fields);
	return UNITY_END();
}
