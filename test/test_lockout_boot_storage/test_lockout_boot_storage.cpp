#include <unity.h>
#include <ArduinoJson.h>
#include <filesystem>
#include <string>

#include "../mocks/Arduino.h"
#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/lockout/lockout_entry.h"
#include "../../src/modules/lockout/lockout_index.h"
#include "../../src/modules/lockout/lockout_band_policy.cpp"
#include "../../src/modules/lockout/lockout_index.cpp"
#include "../../src/modules/lockout/lockout_store.h"
#include "../../src/modules/lockout/lockout_store.cpp"
#include "../../src/modules/lockout/lockout_boot_storage.h"
#include "../../src/modules/lockout/lockout_boot_storage.cpp"

static LockoutIndex testIndex;
static LockoutStore store;

void setUp() {
    lockoutSetKaLearningEnabled(false);
    testIndex.clear();
    store.begin(&testIndex);
}

void tearDown() {}

static LockoutEntry makeEntry(int32_t latE5, int32_t lonE5, uint16_t freqMHz, uint8_t bandMask) {
    LockoutEntry entry;
    entry.latE5 = latE5;
    entry.lonE5 = lonE5;
    entry.radiusE5 = 1350;
    entry.bandMask = bandMask;
    entry.freqMHz = freqMHz;
    entry.freqTolMHz = 10;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_LEARNED;
    entry.firstSeenMs = 1700000000000LL;
    entry.lastSeenMs = 1700000060000LL;
    entry.lastPassMs = 1700000090000LL;
    entry.lastCountedMissMs = 1700000110000LL;
    entry.setActive(true);
    return entry;
}

static std::filesystem::path makeFsRoot(const char* testName) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("v1g2_lockout_boot_" + std::string(testName));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

static std::filesystem::path resolveFsPath(const std::filesystem::path& root, const char* logicalPath) {
    std::filesystem::path relative = logicalPath ? std::filesystem::path(logicalPath) : std::filesystem::path();
    if (relative.is_absolute()) {
        relative = relative.relative_path();
    }
    return root / relative;
}

static uint32_t normalizeNoop(JsonDocument&) {
    return 0;
}

static void writeJsonZones(fs::FS& fs, const char* path, const LockoutEntry& entry) {
    JsonDocument doc;
    doc["_type"] = LockoutStore::kTypeTag;
    doc["_version"] = LockoutStore::kVersion;
    JsonObject zone = doc["zones"].to<JsonArray>().add<JsonObject>();
    zone["lat"] = entry.latE5;
    zone["lon"] = entry.lonE5;
    zone["rad"] = entry.radiusE5;
    zone["band"] = entry.bandMask;
    zone["freq"] = entry.freqMHz;
    zone["ftol"] = entry.freqTolMHz;
    zone["conf"] = entry.confidence;
    zone["flags"] = entry.flags;
    zone["first"] = entry.firstSeenMs;
    zone["last"] = entry.lastSeenMs;
    zone["pass"] = entry.lastPassMs;
    zone["mms"] = entry.lastCountedMissMs;

    File file = fs.open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_GREATER_THAN(0, serializeJson(doc, file));
    file.close();
}

static const LockoutEntry* findEntryByLat(int32_t latE5) {
    for (size_t i = 0; i < testIndex.capacity(); ++i) {
        const LockoutEntry* entry = testIndex.at(i);
        if (entry && entry->isActive() && entry->latE5 == latE5) {
            return entry;
        }
    }
    return nullptr;
}

void test_boot_loader_prefers_binary_over_stale_json() {
    const std::filesystem::path root = makeFsRoot("prefer_binary");
    fs::FS fs(root);

    const LockoutEntry binaryEntry = makeEntry(1000000, -1000000, 24120, 0x04);
    const LockoutEntry staleJsonEntry = makeEntry(2000000, -2000000, 10525, 0x08);

    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(binaryEntry));
    TEST_ASSERT_TRUE(store.saveBinary(fs, LockoutStore::kBinaryPath));
    writeJsonZones(fs, "/v1simple_lockout_zones.json", staleJsonEntry);

    testIndex.clear();
    LockoutBootLoadResult result;
    TEST_ASSERT_TRUE(loadLockoutZonesBinaryFirst(fs,
                                                 store,
                                                 LockoutStore::kBinaryPath,
                                                 "/v1simple_lockout_zones.json",
                                                 LockoutStore::kJsonMigratedBackupPath,
                                                 normalizeNoop,
                                                 &result));

    TEST_ASSERT_EQUAL(static_cast<int>(LockoutBootLoadOutcome::LoadedBinary),
                      static_cast<int>(result.outcome));
    TEST_ASSERT_NOT_NULL(findEntryByLat(binaryEntry.latE5));
    TEST_ASSERT_NULL(findEntryByLat(staleJsonEntry.latE5));
    TEST_ASSERT_TRUE(fs.exists("/v1simple_lockout_zones.json"));
}

void test_boot_loader_migrates_json_then_loads_binary_on_subsequent_boot() {
    const std::filesystem::path root = makeFsRoot("migrate_then_binary");
    fs::FS fs(root);

    const LockoutEntry migratedEntry = makeEntry(3000000, -3000000, 24148, 0x04);
    writeJsonZones(fs, "/v1simple_lockout_zones.json", migratedEntry);

    LockoutBootLoadResult firstBoot;
    TEST_ASSERT_TRUE(loadLockoutZonesBinaryFirst(fs,
                                                 store,
                                                 LockoutStore::kBinaryPath,
                                                 "/v1simple_lockout_zones.json",
                                                 LockoutStore::kJsonMigratedBackupPath,
                                                 normalizeNoop,
                                                 &firstBoot));
    TEST_ASSERT_EQUAL(static_cast<int>(LockoutBootLoadOutcome::MigratedJson),
                      static_cast<int>(firstBoot.outcome));
    TEST_ASSERT_TRUE(fs.exists(LockoutStore::kBinaryPath));
    TEST_ASSERT_FALSE(fs.exists("/v1simple_lockout_zones.json"));
    TEST_ASSERT_TRUE(fs.exists(LockoutStore::kJsonMigratedBackupPath));
    TEST_ASSERT_NOT_NULL(findEntryByLat(migratedEntry.latE5));

    testIndex.clear();
    LockoutBootLoadResult secondBoot;
    TEST_ASSERT_TRUE(loadLockoutZonesBinaryFirst(fs,
                                                 store,
                                                 LockoutStore::kBinaryPath,
                                                 "/v1simple_lockout_zones.json",
                                                 LockoutStore::kJsonMigratedBackupPath,
                                                 normalizeNoop,
                                                 &secondBoot));
    TEST_ASSERT_EQUAL(static_cast<int>(LockoutBootLoadOutcome::LoadedBinary),
                      static_cast<int>(secondBoot.outcome));
    TEST_ASSERT_NOT_NULL(findEntryByLat(migratedEntry.latE5));
}

void test_boot_loader_does_not_rollback_to_migrated_backup_after_binary_corruption() {
    const std::filesystem::path root = makeFsRoot("no_rollback_backup");
    fs::FS fs(root);

    const LockoutEntry migratedEntry = makeEntry(4000000, -4000000, 24148, 0x04);
    writeJsonZones(fs, "/v1simple_lockout_zones.json", migratedEntry);

    LockoutBootLoadResult migrated;
    TEST_ASSERT_TRUE(loadLockoutZonesBinaryFirst(fs,
                                                 store,
                                                 LockoutStore::kBinaryPath,
                                                 "/v1simple_lockout_zones.json",
                                                 LockoutStore::kJsonMigratedBackupPath,
                                                 normalizeNoop,
                                                 &migrated));
    TEST_ASSERT_EQUAL(static_cast<int>(LockoutBootLoadOutcome::MigratedJson),
                      static_cast<int>(migrated.outcome));

    const std::filesystem::path binaryPath = resolveFsPath(root, LockoutStore::kBinaryPath);
    std::filesystem::resize_file(binaryPath, std::filesystem::file_size(binaryPath) - 1);

    testIndex.clear();
    const LockoutEntry sentinel = makeEntry(9000000, -9000000, 10525, 0x08);
    TEST_ASSERT_GREATER_OR_EQUAL(0, testIndex.add(sentinel));

    LockoutBootLoadResult afterCorruption;
    TEST_ASSERT_FALSE(loadLockoutZonesBinaryFirst(fs,
                                                  store,
                                                  LockoutStore::kBinaryPath,
                                                  "/v1simple_lockout_zones.json",
                                                  LockoutStore::kJsonMigratedBackupPath,
                                                  normalizeNoop,
                                                  &afterCorruption));
    TEST_ASSERT_EQUAL(static_cast<int>(LockoutBootLoadOutcome::BinaryFailedNoJson),
                      static_cast<int>(afterCorruption.outcome));
    TEST_ASSERT_NOT_NULL(findEntryByLat(sentinel.latE5));
    TEST_ASSERT_NULL(findEntryByLat(migratedEntry.latE5));
    TEST_ASSERT_TRUE(fs.exists(LockoutStore::kJsonMigratedBackupPath));
}

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_boot_loader_prefers_binary_over_stale_json);
    RUN_TEST(test_boot_loader_migrates_json_then_loads_binary_on_subsequent_boot);
    RUN_TEST(test_boot_loader_does_not_rollback_to_migrated_backup_after_binary_corruption);

    return UNITY_END();
}
