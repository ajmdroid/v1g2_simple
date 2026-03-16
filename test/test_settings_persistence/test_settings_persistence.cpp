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
           ("settings_persistence_" + std::to_string(++g_tempRootIndex));
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

String activeNamespaceOrEmpty() {
    return mock_preferences::getString(SETTINGS_NS_META, "active", "");
}

bool loadJsonFile(fs::FS& fs, const char* path, JsonDocument& doc) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
        return false;
    }
    const DeserializationError err = deserializeJson(doc, file);
    file.close();
    return !err;
}

std::string readFileToString(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
        return {};
    }

    std::string output;
    while (file.available()) {
        output.push_back(static_cast<char>(file.read()));
    }
    file.close();
    return output;
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

void test_save_load_and_backup_round_trip_current_shape_fields() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager original;
    V1Settings& settings = original.mutableSettings();
    settings.enableWifi = false;
    settings.wifiMode = V1_WIFI_APSTA;
    settings.apSSID = "RoadRig";
    settings.apPassword = "unit-test-pass";
    settings.wifiClientEnabled = true;
    settings.wifiClientSSID = "GarageNet";
    settings.proxyBLE = false;
    settings.proxyName = "Proxy-Rig";
    settings.gpsEnabled = true;
    settings.gpsLockoutMode = LOCKOUT_RUNTIME_ADVISORY;
    settings.gpsLockoutLearnerRadiusE5 = 200;
    settings.gpsLockoutKaLearningEnabled = true;
    settings.turnOffDisplay = true;
    settings.brightness = 123;
    settings.displayStyle = DISPLAY_STYLE_SERPENTINE;
    settings.colorBogey = 0x1234;
    settings.colorGps = 0x4567;
    settings.hideWifiIcon = true;
    settings.enableWifiAtBoot = true;
    settings.enableSignalTraceLogging = false;
    settings.voiceAlertMode = VOICE_MODE_FREQ_ONLY;
    settings.voiceDirectionEnabled = false;
    settings.announceBogeyCount = false;
    settings.muteVoiceIfVolZero = true;
    settings.voiceVolume = 55;
    settings.announceSecondaryAlerts = true;
    settings.secondaryK = true;
    settings.alertVolumeFadeEnabled = true;
    settings.alertVolumeFadeDelaySec = 4;
    settings.alertVolumeFadeVolume = 2;
    settings.autoPushEnabled = true;
    settings.activeSlot = 2;
    settings.slot0Name = "DEFAULT";
    settings.slot1Name = "HIGHWAY";
    settings.slot2Name = "QUIET";
    settings.slot0Color = 0x1111;
    settings.slot1Color = 0x2222;
    settings.slot2Color = 0x3333;
    settings.slot0Volume = 4;
    settings.slot1MuteVolume = 2;
    settings.slot2DarkMode = true;
    settings.slot0MuteToZero = true;
    settings.slot1AlertPersist = 5;
    settings.slot2PriorityArrow = true;
    settings.slot0_default = AutoPushSlot("City", V1_MODE_LOGIC);
    settings.slot1_highway = AutoPushSlot("Highway", V1_MODE_ALL_BOGEYS);
    settings.slot2_comfort = AutoPushSlot("Quiet", V1_MODE_ADVANCED_LOGIC);
    settings.lastV1Address = "AA:BB:CC:DD:EE:FF";
    settings.autoPowerOffMinutes = 7;
    settings.apTimeoutMinutes = 15;
    settings.obdEnabled = true;
    settings.obdSavedAddress = "11:22:33:44:55:66";
    settings.obdMinRssi = -65;

    original.save();

    TEST_ASSERT_EQUAL_UINT32(2u, original.backupRevision());

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "apSSID"));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "voiceMode"));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "obdMinRssi"));
    TEST_ASSERT_TRUE(
        mock_preferences::getString(activeNs.c_str(), "apPassword", "").startsWith(OBFUSCATION_HEX_PREFIX));

    SettingsManager reloaded;
    reloaded.load();
    const V1Settings& loaded = reloaded.get();

    TEST_ASSERT_FALSE(loaded.enableWifi);
    TEST_ASSERT_EQUAL_INT(V1_WIFI_APSTA, loaded.wifiMode);
    TEST_ASSERT_EQUAL_STRING("RoadRig", loaded.apSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("unit-test-pass", loaded.apPassword.c_str());
    TEST_ASSERT_TRUE(loaded.wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("GarageNet", loaded.wifiClientSSID.c_str());
    TEST_ASSERT_FALSE(loaded.proxyBLE);
    TEST_ASSERT_EQUAL_STRING("Proxy-Rig", loaded.proxyName.c_str());
    TEST_ASSERT_TRUE(loaded.gpsEnabled);
    TEST_ASSERT_EQUAL_INT(LOCKOUT_RUNTIME_ADVISORY, loaded.gpsLockoutMode);
    TEST_ASSERT_EQUAL_UINT16(200u, loaded.gpsLockoutLearnerRadiusE5);
    TEST_ASSERT_TRUE(loaded.gpsLockoutKaLearningEnabled);
    TEST_ASSERT_TRUE(loaded.turnOffDisplay);
    TEST_ASSERT_EQUAL_UINT8(123, loaded.brightness);
    TEST_ASSERT_EQUAL_INT(DISPLAY_STYLE_SERPENTINE, loaded.displayStyle);
    TEST_ASSERT_EQUAL_HEX16(0x1234, loaded.colorBogey);
    TEST_ASSERT_EQUAL_HEX16(0x4567, loaded.colorGps);
    TEST_ASSERT_TRUE(loaded.hideWifiIcon);
    TEST_ASSERT_TRUE(loaded.enableWifiAtBoot);
    TEST_ASSERT_FALSE(loaded.enableSignalTraceLogging);
    TEST_ASSERT_EQUAL_INT(VOICE_MODE_FREQ_ONLY, loaded.voiceAlertMode);
    TEST_ASSERT_FALSE(loaded.voiceDirectionEnabled);
    TEST_ASSERT_FALSE(loaded.announceBogeyCount);
    TEST_ASSERT_TRUE(loaded.muteVoiceIfVolZero);
    TEST_ASSERT_EQUAL_UINT8(55, loaded.voiceVolume);
    TEST_ASSERT_TRUE(loaded.announceSecondaryAlerts);
    TEST_ASSERT_TRUE(loaded.secondaryK);
    TEST_ASSERT_TRUE(loaded.alertVolumeFadeEnabled);
    TEST_ASSERT_EQUAL_UINT8(4, loaded.alertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(2, loaded.alertVolumeFadeVolume);
    TEST_ASSERT_TRUE(loaded.autoPushEnabled);
    TEST_ASSERT_EQUAL_INT(2, loaded.activeSlot);
    TEST_ASSERT_EQUAL_STRING("QUIET", loaded.slot2Name.c_str());
    TEST_ASSERT_EQUAL_HEX16(0x1111, loaded.slot0Color);
    TEST_ASSERT_EQUAL_HEX16(0x2222, loaded.slot1Color);
    TEST_ASSERT_EQUAL_HEX16(0x3333, loaded.slot2Color);
    TEST_ASSERT_EQUAL_UINT8(4, loaded.slot0Volume);
    TEST_ASSERT_EQUAL_UINT8(2, loaded.slot1MuteVolume);
    TEST_ASSERT_TRUE(loaded.slot2DarkMode);
    TEST_ASSERT_TRUE(loaded.slot0MuteToZero);
    TEST_ASSERT_EQUAL_UINT8(5, loaded.slot1AlertPersist);
    TEST_ASSERT_TRUE(loaded.slot2PriorityArrow);
    TEST_ASSERT_EQUAL_STRING("City", loaded.slot0_default.profileName.c_str());
    TEST_ASSERT_EQUAL_INT(V1_MODE_LOGIC, loaded.slot0_default.mode);
    TEST_ASSERT_EQUAL_STRING("Quiet", loaded.slot2_comfort.profileName.c_str());
    TEST_ASSERT_EQUAL_INT(V1_MODE_ADVANCED_LOGIC, loaded.slot2_comfort.mode);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", loaded.lastV1Address.c_str());
    TEST_ASSERT_EQUAL_UINT8(7, loaded.autoPowerOffMinutes);
    TEST_ASSERT_EQUAL_UINT8(15, loaded.apTimeoutMinutes);
    TEST_ASSERT_TRUE(loaded.obdEnabled);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", loaded.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_INT8(-65, loaded.obdMinRssi);

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("v1simple_sd_backup", backupDoc["_type"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(1000u, backupDoc["_timestamp"].as<uint32_t>());
    TEST_ASSERT_FALSE(backupDoc["enableWifi"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("RoadRig", backupDoc["apSSID"].as<const char*>());
    TEST_ASSERT_TRUE(backupDoc["wifiClientEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("GarageNet", backupDoc["wifiClientSSID"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(LOCKOUT_RUNTIME_ADVISORY, backupDoc["gpsLockoutMode"].as<int>());
    TEST_ASSERT_TRUE(backupDoc["cameraAlertsEnabled"].isNull());
    TEST_ASSERT_TRUE(backupDoc["cameraAlertRangeCm"].isNull());
    TEST_ASSERT_EQUAL_INT(123, backupDoc["brightness"].as<int>());
    TEST_ASSERT_EQUAL_INT(VOICE_MODE_FREQ_ONLY, backupDoc["voiceAlertMode"].as<int>());
    TEST_ASSERT_TRUE(backupDoc["autoPushEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("Quiet", backupDoc["slot2ProfileName"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(-65, backupDoc["obdMinRssi"].as<int>());
}

void test_serialized_backup_payload_matches_builder_and_writes_same_json() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    V1Settings& settings = manager.mutableSettings();
    settings.apSSID = "PayloadTest";
    settings.brightness = 77;
    settings.proxyBLE = false;

    V1Profile profile("Road");
    profile.description = "Serialized";
    ProfileSaveResult saveResult = v1ProfileManager.saveProfile(profile);
    TEST_ASSERT_TRUE(saveResult.success);

    JsonDocument expectedDoc;
    const BackupPayloadBuilder::BuildResult buildResult =
        BackupPayloadBuilder::buildBackupDocument(expectedDoc,
                                                  settings,
                                                  v1ProfileManager,
                                                  BackupPayloadBuilder::BackupTransport::SdBackup,
                                                  4321);

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(payload, settings, v1ProfileManager, 4321));
    TEST_ASSERT_EQUAL_UINT32(4321u, payload.snapshotMs);
    TEST_ASSERT_EQUAL_INT(buildResult.profilesBackedUp, payload.profilesBackedUp);
    TEST_ASSERT_NOT_NULL(payload.data);
    TEST_ASSERT_TRUE(payload.length > 0);

    std::string expected;
    serializeJson(expectedDoc, expected);
    TEST_ASSERT_EQUAL_UINT(expected.size(), payload.length);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), payload.data, payload.length);

    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), readFileToString(fs, SETTINGS_BACKUP_PATH).c_str());

    releaseSerializedSettingsBackupPayload(payload);
    TEST_ASSERT_NULL(payload.data);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_load_and_backup_round_trip_current_shape_fields);
    RUN_TEST(test_serialized_backup_payload_matches_builder_and_writes_same_json);
    return UNITY_END();
}
