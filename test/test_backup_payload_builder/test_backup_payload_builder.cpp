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

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("backup_payload_builder_" + std::to_string(++g_tempRootIndex));
}

std::string serializeDoc(const JsonDocument& doc) {
    std::string output;
    serializeJson(doc, output);
    return output;
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

void test_builder_recognizes_http_and_sd_backup_types() {
    TEST_ASSERT_TRUE(BackupPayloadBuilder::isRecognizedBackupType("v1simple_backup"));
    TEST_ASSERT_TRUE(BackupPayloadBuilder::isRecognizedBackupType("v1simple_sd_backup"));
    TEST_ASSERT_FALSE(BackupPayloadBuilder::isRecognizedBackupType("invalid_backup"));
    TEST_ASSERT_FALSE(BackupPayloadBuilder::isRecognizedBackupType(nullptr));
}

void test_builder_aligns_http_and_sd_schema() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager profileManager;
    TEST_ASSERT_TRUE(profileManager.begin(&fs));

    V1Settings settings;
    settings.enableWifi = true;
    settings.apSSID = "V1-Test";
    settings.gpsLockoutKLearningEnabled = true;
    settings.gpsLockoutXLearningEnabled = false;
    settings.cameraAlertsEnabled = false;
    settings.cameraAlertRangeCm = 54321;

    JsonDocument httpDoc;
    JsonDocument sdDoc;

    BackupPayloadBuilder::buildBackupDocument(httpDoc,
                                              settings,
                                              profileManager,
                                              BackupPayloadBuilder::BackupTransport::HttpDownload,
                                              4242);
    BackupPayloadBuilder::buildBackupDocument(sdDoc,
                                              settings,
                                              profileManager,
                                              BackupPayloadBuilder::BackupTransport::SdBackup,
                                              4242);

    TEST_ASSERT_EQUAL_STRING("v1simple_backup", httpDoc["_type"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("v1simple_sd_backup", sdDoc["_type"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(SD_BACKUP_VERSION, httpDoc["_version"].as<int>());
    TEST_ASSERT_EQUAL_INT(SD_BACKUP_VERSION, sdDoc["_version"].as<int>());
    TEST_ASSERT_EQUAL_UINT32(4242, httpDoc["_timestamp"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(4242, httpDoc["timestamp"].as<uint32_t>());
    TEST_ASSERT_FALSE(httpDoc["cameraAlertsEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_INT(54321, httpDoc["cameraAlertRangeCm"].as<int>());
    TEST_ASSERT_TRUE(httpDoc["gpsLockoutKLearningEnabled"].as<bool>());
    TEST_ASSERT_FALSE(httpDoc["gpsLockoutXLearningEnabled"].as<bool>());
    TEST_ASSERT_FALSE(sdDoc["cameraAlertsEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_INT(54321, sdDoc["cameraAlertRangeCm"].as<int>());
    TEST_ASSERT_TRUE(sdDoc["gpsLockoutKLearningEnabled"].as<bool>());
    TEST_ASSERT_FALSE(sdDoc["gpsLockoutXLearningEnabled"].as<bool>());
    TEST_ASSERT_TRUE(httpDoc["cameraAlertNearRangeCm"].isNull());
    TEST_ASSERT_TRUE(httpDoc["cameraTypeBusLane"].isNull());
    TEST_ASSERT_TRUE(httpDoc["colorCameraArrow"].isNull());
    TEST_ASSERT_TRUE(httpDoc["cameraVoiceNearEnabled"].isNull());
    TEST_ASSERT_TRUE(sdDoc["cameraAlertNearRangeCm"].isNull());
    TEST_ASSERT_TRUE(sdDoc["cameraTypeBusLane"].isNull());
    TEST_ASSERT_TRUE(sdDoc["colorCameraArrow"].isNull());
    TEST_ASSERT_TRUE(sdDoc["cameraVoiceNearEnabled"].isNull());

    httpDoc["_type"] = "same_type";
    sdDoc["_type"] = "same_type";
    const std::string httpPayload = serializeDoc(httpDoc);
    const std::string sdPayload = serializeDoc(sdDoc);
    TEST_ASSERT_EQUAL_STRING(httpPayload.c_str(), sdPayload.c_str());
}

void test_builder_includes_profile_payloads_and_snapshot_time() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager profileManager;
    TEST_ASSERT_TRUE(profileManager.begin(&fs));

    V1Profile profile("Road");
    profile.description = "Highway";
    profile.displayOn = false;
    profile.mainVolume = 6;
    profile.mutedVolume = 2;
    for (int i = 0; i < 6; i++) {
        profile.settings.bytes[i] = static_cast<uint8_t>(10 + i);
    }

    ProfileSaveResult saveResult = profileManager.saveProfile(profile);
    TEST_ASSERT_TRUE(saveResult.success);

    JsonDocument doc;
    const BackupPayloadBuilder::BuildResult buildResult =
        BackupPayloadBuilder::buildBackupDocument(
            doc,
            V1Settings{},
            profileManager,
            BackupPayloadBuilder::BackupTransport::HttpDownload,
            7777);

    TEST_ASSERT_EQUAL_INT(1, buildResult.profilesBackedUp);
    TEST_ASSERT_EQUAL_UINT32(7777, doc["_timestamp"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(7777, doc["timestamp"].as<uint32_t>());

    JsonArray profiles = doc["profiles"].as<JsonArray>();
    TEST_ASSERT_EQUAL_UINT(1, profiles.size());

    JsonObject savedProfile = profiles[0].as<JsonObject>();
    TEST_ASSERT_EQUAL_STRING("Road", savedProfile["name"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Highway", savedProfile["description"].as<const char*>());
    TEST_ASSERT_FALSE(savedProfile["displayOn"].as<bool>());
    TEST_ASSERT_EQUAL_INT(6, savedProfile["mainVolume"].as<int>());
    TEST_ASSERT_EQUAL_INT(2, savedProfile["mutedVolume"].as<int>());

    JsonArray bytes = savedProfile["bytes"].as<JsonArray>();
    TEST_ASSERT_EQUAL_UINT(6, bytes.size());
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(10 + i), bytes[i].as<uint8_t>());
    }
}

void test_profile_catalog_revision_advances_on_mutations() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager profileManager;
    TEST_ASSERT_TRUE(profileManager.begin(&fs));

    const uint32_t initialRevision = profileManager.catalogRevision();

    V1Profile profile("City");
    ProfileSaveResult saveResult = profileManager.saveProfile(profile);
    TEST_ASSERT_TRUE(saveResult.success);
    const uint32_t afterSaveRevision = profileManager.catalogRevision();
    TEST_ASSERT_TRUE(afterSaveRevision > initialRevision);

    TEST_ASSERT_TRUE(profileManager.renameProfile("City", "Highway"));
    const uint32_t afterRenameRevision = profileManager.catalogRevision();
    TEST_ASSERT_TRUE(afterRenameRevision > afterSaveRevision);

    TEST_ASSERT_TRUE(profileManager.deleteProfile("Highway"));
    TEST_ASSERT_TRUE(profileManager.catalogRevision() > afterRenameRevision);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_builder_recognizes_http_and_sd_backup_types);
    RUN_TEST(test_builder_aligns_http_and_sd_schema);
    RUN_TEST(test_builder_includes_profile_payloads_and_snapshot_time);
    RUN_TEST(test_profile_catalog_revision_advances_on_mutations);
    return UNITY_END();
}
