#include "lockout_boot_storage.h"
#include "lockout_store.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

namespace {

bool archiveMigratedLockoutJson(fs::FS& fs, const char* jsonPath, const char* jsonBackupPath) {
    if (!jsonPath || jsonPath[0] == '\0' || !fs.exists(jsonPath)) {
        return true;
    }

    if (!jsonBackupPath || jsonBackupPath[0] == '\0') {
        return fs.remove(jsonPath);
    }

    if (fs.exists(jsonBackupPath)) {
        fs.remove(jsonBackupPath);
    }

    if (fs.rename(jsonPath, jsonBackupPath)) {
        return true;
    }

    return fs.remove(jsonPath);
}

bool loadJsonDocument(fs::FS& fs, const char* jsonPath, JsonDocument& outDoc) {
    if (!jsonPath || jsonPath[0] == '\0' || !fs.exists(jsonPath)) {
        return false;
    }

    File file = fs.open(jsonPath, FILE_READ);
    if (!(file && file.size() > 0 && file.size() < 65536)) {
        if (file) {
            file.close();
        }
        return false;
    }

    const DeserializationError err = deserializeJson(outDoc, file);
    file.close();
    return err == DeserializationError::Ok;
}

}  // namespace

bool loadLockoutZonesBinaryFirst(fs::FS& fs,
                                 LockoutStore& store,
                                 const char* binaryPath,
                                 const char* jsonPath,
                                 const char* jsonBackupPath,
                                 uint32_t (*normalizeLegacyRadius)(JsonDocument&),
                                 LockoutBootLoadResult* result) {
    LockoutBootLoadResult localResult = {};
    localResult.binaryPresent = binaryPath && binaryPath[0] != '\0' && fs.exists(binaryPath);
    localResult.jsonPresent = jsonPath && jsonPath[0] != '\0' && fs.exists(jsonPath);

    if (binaryPath && binaryPath[0] != '\0' && store.loadBinary(fs, binaryPath)) {
        localResult.outcome = LockoutBootLoadOutcome::LoadedBinary;
        if (result) {
            *result = localResult;
        }
        return true;
    }

    if (!localResult.jsonPresent) {
        localResult.outcome = localResult.binaryPresent
                                  ? LockoutBootLoadOutcome::BinaryFailedNoJson
                                  : LockoutBootLoadOutcome::NoneFound;
        if (result) {
            *result = localResult;
        }
        return false;
    }

    JsonDocument doc;
    if (!loadJsonDocument(fs, jsonPath, doc)) {
        localResult.outcome = LockoutBootLoadOutcome::JsonInvalid;
        if (result) {
            *result = localResult;
        }
        return false;
    }

    if (normalizeLegacyRadius) {
        localResult.legacyRadiusMigrations = normalizeLegacyRadius(doc);
    }

    if (!store.fromJson(doc)) {
        localResult.outcome = LockoutBootLoadOutcome::JsonInvalid;
        if (result) {
            *result = localResult;
        }
        return false;
    }

    if (!binaryPath || binaryPath[0] == '\0' || !store.saveBinary(fs, binaryPath)) {
        store.markDirty();
        localResult.outcome = LockoutBootLoadOutcome::LoadedJsonSavePending;
        if (result) {
            *result = localResult;
        }
        return true;
    }

    localResult.archivedJson = archiveMigratedLockoutJson(fs, jsonPath, jsonBackupPath);
    localResult.outcome = LockoutBootLoadOutcome::MigratedJson;
    if (result) {
        *result = localResult;
    }
    return true;
}
