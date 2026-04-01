#pragma once

#include <WebServer.h>

#include "backup_snapshot_cache.h"

// Forward declarations at global scope so function signatures inside the
// namespace refer to the real classes, not a nested incomplete type.
class ObdRuntimeModule;
class SpeedSourceSelector;

namespace BackupApiService {

struct BackupNowRuntime {
    bool (*isStorageReady)(void* ctx);
    void* isStorageReadyCtx;
    bool (*isSDCard)(void* ctx);
    void* isSDCardCtx;
    bool (*backupToSD)(void* ctx);
    void* backupToSDCtx;
};

inline void sendBackupNowResponse(WebServer& server, const BackupNowRuntime& runtime) {
    const bool storageReady = runtime.isStorageReady && runtime.isStorageReady(runtime.isStorageReadyCtx);
    const bool sdCard = runtime.isSDCard && runtime.isSDCard(runtime.isSDCardCtx);
    if (!storageReady || !sdCard) {
        server.send(503, "application/json",
                    "{\"success\":false,\"error\":\"SD card unavailable\"}");
        return;
    }

    const bool backupOk = runtime.backupToSD && runtime.backupToSD(runtime.backupToSDCtx);
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
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx,
                     uint32_t (*millisFn)(void* ctx) = nullptr, void* millisCtx = nullptr);

/// POST /api/settings/backup-now handler to force a backup to SD.
void handleApiBackupNow(WebServer& server,
                        bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                        void (*markUiActivity)(void* ctx), void* uiActivityCtx);

/// POST /api/settings/restore handler with route-level policy callbacks.
/// @param obdRuntimeModule Reference to OBD runtime module for settings sync.
/// @param speedSourceSelector Reference to speed source selector for settings sync.
void handleApiRestore(WebServer& server,
                      ObdRuntimeModule& obdRuntimeModule,
                      SpeedSourceSelector& speedSourceSelector,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                      void (*markUiActivity)(void* ctx), void* uiActivityCtx);

}  // namespace BackupApiService
