#pragma once

#include <ArduinoJson.h>
#include <cstdint>

#include "settings.h"
#include "v1_profiles.h"

namespace BackupPayloadBuilder {

enum class BackupTransport : uint8_t {
    HttpDownload = 0,
    SdBackup,
};

struct BuildResult {
    int profilesBackedUp = 0;
};

const char* backupTypeForTransport(BackupTransport transport);
bool isRecognizedBackupType(const char* type);

BuildResult buildBackupDocument(JsonDocument& doc,
                                const V1Settings& settings,
                                const V1ProfileManager& profileManager,
                                BackupTransport transport,
                                uint32_t snapshotMs);

}  // namespace BackupPayloadBuilder
