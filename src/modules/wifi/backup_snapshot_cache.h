#pragma once

#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace BackupApiService {

struct BackupSnapshotCache {
    char* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool inPsram = false;
    uint32_t snapshotMs = 0;
    uint32_t settingsRevision = 0;
    uint32_t profileRevision = 0;
    bool valid = false;
};

using BackupSnapshotBuildFn = std::function<void(JsonDocument&, uint32_t snapshotMs)>;

bool sendCachedBackupSnapshot(WebServer& server,
                              BackupSnapshotCache& cache,
                              uint32_t settingsRevision,
                              uint32_t profileRevision,
                              const BackupSnapshotBuildFn& buildSnapshot,
                              const std::function<uint32_t()>& millisFn = nullptr);

void releaseBackupSnapshotCache(BackupSnapshotCache& cache);

}  // namespace BackupApiService
