#pragma once

#include <stdint.h>
#include <ArduinoJson.h>
#include <FS.h>

class LockoutStore;

enum class LockoutBootLoadOutcome : uint8_t {
    NoneFound = 0,
    LoadedBinary,
    MigratedJson,
    LoadedJsonSavePending,
    BinaryFailedNoJson,
    JsonInvalid,
};

struct LockoutBootLoadResult {
    LockoutBootLoadOutcome outcome = LockoutBootLoadOutcome::NoneFound;
    bool binaryPresent = false;
    bool jsonPresent = false;
    bool archivedJson = false;
    uint32_t legacyRadiusMigrations = 0;
};

bool loadLockoutZonesBinaryFirst(fs::FS& fs,
                                 LockoutStore& store,
                                 const char* binaryPath,
                                 const char* jsonPath,
                                 const char* jsonBackupPath,
                                 uint32_t (*normalizeLegacyRadius)(JsonDocument&),
                                 LockoutBootLoadResult* result = nullptr);
