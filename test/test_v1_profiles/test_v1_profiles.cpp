#include <unity.h>

#include <filesystem>
#include <string>

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
           ("v1_profiles_" + std::to_string(++g_tempRootIndex));
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

V1Profile makeProfile(const String& name,
                      uint8_t baseByte,
                      const String& description = "profile") {
    V1Profile profile(name);
    profile.description = description;
    profile.displayOn = true;
    profile.mainVolume = 6;
    profile.mutedVolume = 2;
    for (int i = 0; i < 6; ++i) {
        profile.settings.bytes[i] = static_cast<uint8_t>(baseByte + i);
    }
    return profile;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
}

void tearDown() {
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    if (!g_tempRoot.empty()) {
        std::filesystem::remove_all(g_tempRoot);
    }
}

void test_save_profile_short_write_new_file_leaves_no_live_json() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    fs::mock_set_fs_write_budget(32);

    const ProfileSaveResult result = manager.saveProfile(makeProfile("Road", 10, "new"));

    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_TRUE(result.error.indexOf("Partial write detected") >= 0);
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.tmp"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.bak"));
}

void test_save_profile_short_write_existing_file_preserves_previous_profile() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    const V1Profile original = makeProfile("Road", 20, "original");
    ProfileSaveResult initialSave = manager.saveProfile(original);
    TEST_ASSERT_TRUE(initialSave.success);
    const std::string before = readFileToString(fs, "/v1profiles/Road.json");

    fs::mock_set_fs_write_budget(32);
    const ProfileSaveResult result = manager.saveProfile(makeProfile("Road", 80, "updated"));

    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_TRUE(result.error.indexOf("Partial write detected") >= 0);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.tmp"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.bak"));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("original", loaded.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(20, loaded.settings.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(25, loaded.settings.bytes[5]);
}

void test_save_profile_normal_path_still_succeeds() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    const V1Profile profile = makeProfile("Quiet", 30, "normal");
    const ProfileSaveResult result = manager.saveProfile(profile);

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_TRUE(fs.exists("/v1profiles/Quiet.json"));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Quiet", loaded));
    TEST_ASSERT_EQUAL_STRING("normal", loaded.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(30, loaded.settings.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(35, loaded.settings.bytes[5]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_profile_short_write_new_file_leaves_no_live_json);
    RUN_TEST(test_save_profile_short_write_existing_file_preserves_previous_profile);
    RUN_TEST(test_save_profile_normal_path_still_succeeds);
    return UNITY_END();
}
