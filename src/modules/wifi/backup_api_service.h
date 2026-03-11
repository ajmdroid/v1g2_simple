#pragma once

#include <WebServer.h>

#include <functional>

#include "backup_snapshot_cache.h"

namespace BackupApiService {

struct BackupNowRuntime {
    std::function<bool()> isStorageReady;
    std::function<bool()> isSDCard;
    std::function<bool()> backupToSD;
};

inline void sendBackupNowResponse(WebServer& server, const BackupNowRuntime& runtime) {
    const bool storageReady = runtime.isStorageReady && runtime.isStorageReady();
    const bool sdCard = runtime.isSDCard && runtime.isSDCard();
    if (!storageReady || !sdCard) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"SD card unavailable\"}");
        return;
    }

    const bool backupOk = runtime.backupToSD && runtime.backupToSD();
    if (!backupOk) {
        server.send(500, "application/json",
                    "{\"success\":false,\"error\":\"Backup write failed\"}");
        return;
    }

    server.send(200, "application/json",
                "{\"success\":true,\"message\":\"Backup written to SD\"}");
}

/// GET /api/settings/backup handler with route-level UI activity callback.
void handleApiBackup(WebServer& server,
                     BackupSnapshotCache& cachedSnapshot,
                     const std::function<void()>& markUiActivity,
                     const std::function<uint32_t()>& millisFn = nullptr);

/// POST /api/settings/backup-now handler to force a backup to SD.
void handleApiBackupNow(WebServer& server,
                        const std::function<bool()>& checkRateLimit,
                        const std::function<void()>& markUiActivity);

/// POST /api/settings/restore handler with route-level policy callbacks.
void handleApiRestore(WebServer& server,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity);

}  // namespace BackupApiService
