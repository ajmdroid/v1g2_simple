#include <unity.h>

#include <filesystem>
#include <string>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/settings.h"
#include "../../src/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace ArduinoJson {

inline void convertFromJson(JsonVariantConst src, ::String& dst) {
    const char* raw = src.as<const char*>();
    dst = ::String(raw ? raw : "");
}

inline bool canConvertFromJson(JsonVariantConst src, const ::String&) {
    return src.is<const char*>();
}

}  // namespace ArduinoJson

#include "../../src/v1_profiles.cpp"
#include "../../src/backup_payload_builder.cpp"
#include "../../src/settings.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_restore.cpp"

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("camera_alpr_settings_contract_" + std::to_string(++g_tempRootIndex));
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    storageManager.reset();
    v1ProfileManager = V1ProfileManager();
    settingsManager = SettingsManager();
    mockMillis = 1000;
    mockMicros = 1000000;
}

bool writeJsonFile(fs::FS& fs, const char* path, const JsonDocument& doc) {
    File file = fs.open(path, FILE_WRITE);
    if (!file) {
        return false;
    }
    if (serializeJson(doc, file) == 0) {
        file.close();
        return false;
    }
    file.flush();
    file.close();
    return true;
}

String activeNamespaceOrEmpty() {
    return mock_preferences::getString(SETTINGS_NS_META, "active", "");
}

void assertNamespaceHasKey(const String& ns, const char* key) {
    TEST_ASSERT_TRUE_MESSAGE(mock_preferences::namespaceHasKey(ns.c_str(), key), key);
}

void assertNamespaceLacksKey(const String& ns, const char* key) {
    TEST_ASSERT_FALSE_MESSAGE(mock_preferences::namespaceHasKey(ns.c_str(), key), key);
}

}  // namespace

void setUp() {
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
    resetRuntimeState();
}

void tearDown() {
    std::filesystem::remove_all(g_tempRoot);
}

void test_schema_versions_match_alpr_contract() {
    TEST_ASSERT_EQUAL_INT(8, SETTINGS_VERSION);
    TEST_ASSERT_EQUAL_INT(11, SD_BACKUP_VERSION);
}

void test_backup_payload_omits_removed_camera_fields() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    V1Settings settings;
    settings.cameraAlertsEnabled = false;
    settings.cameraAlertRangeCm = 43210;

    JsonDocument doc;
    BackupPayloadBuilder::buildBackupDocument(
        doc,
        settings,
        v1ProfileManager,
        BackupPayloadBuilder::BackupTransport::SdBackup,
        2222);

    TEST_ASSERT_EQUAL_INT(SD_BACKUP_VERSION, doc["_version"].as<int>());
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

void test_restore_from_sd_round_trips_only_alpr_camera_settings() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    V1Settings sourceSettings;
    sourceSettings.cameraAlertsEnabled = false;
    sourceSettings.cameraAlertRangeCm = 54321;

    JsonDocument doc;
    BackupPayloadBuilder::buildBackupDocument(
        doc,
        sourceSettings,
        v1ProfileManager,
        BackupPayloadBuilder::BackupTransport::SdBackup,
        3333);

    doc["cameraAlertNearRangeCm"] = 99999;
    doc["cameraTypeAlpr"] = false;
    doc["cameraTypeRedLight"] = true;
    doc["cameraTypeSpeed"] = true;
    doc["cameraTypeBusLane"] = true;
    doc["colorCameraArrow"] = 0x1234;
    doc["colorCameraText"] = 0x5678;
    doc["cameraVoiceFarEnabled"] = true;
    doc["cameraVoiceNearEnabled"] = true;

    TEST_ASSERT_TRUE(writeJsonFile(fs, SETTINGS_BACKUP_PATH, doc));

    SettingsManager restored;
    TEST_ASSERT_TRUE(restored.restoreFromSD());
    TEST_ASSERT_FALSE(restored.get().cameraAlertsEnabled);
    TEST_ASSERT_EQUAL_UINT32(54321u, restored.get().cameraAlertRangeCm);

    SettingsManager reloaded;
    reloaded.load();
    TEST_ASSERT_FALSE(reloaded.get().cameraAlertsEnabled);
    TEST_ASSERT_EQUAL_UINT32(54321u, reloaded.get().cameraAlertRangeCm);

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);

    assertNamespaceHasKey(activeNs, "camEn");
    assertNamespaceHasKey(activeNs, "camRngCm");
    assertNamespaceLacksKey(activeNs, "camNearCm");
    assertNamespaceLacksKey(activeNs, "camTAlpr");
    assertNamespaceLacksKey(activeNs, "camTSpd");
    assertNamespaceLacksKey(activeNs, "camTRL");
    assertNamespaceLacksKey(activeNs, "camTBus");
    assertNamespaceLacksKey(activeNs, "camVFar");
    assertNamespaceLacksKey(activeNs, "camVNear");
    assertNamespaceLacksKey(activeNs, "colCamArr");
    assertNamespaceLacksKey(activeNs, "colCamTxt");
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_schema_versions_match_alpr_contract);
    RUN_TEST(test_backup_payload_omits_removed_camera_fields);
    RUN_TEST(test_restore_from_sd_round_trips_only_alpr_camera_settings);
    return UNITY_END();
}
