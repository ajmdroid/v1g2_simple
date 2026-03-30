#include "../../../src/modules/wifi/backup_api_service.h"

namespace BackupApiService {

// Internal implementation entrypoints defined in backup_api_service.cpp.
void sendBackup(WebServer& server,
                BackupSnapshotCache& cachedSnapshot,
                uint32_t (*millisFn)(void* ctx), void* millisCtx);
void handleBackupNow(WebServer& server);
void handleRestore(WebServer& server);

void handleApiBackup(WebServer& server,
                     BackupSnapshotCache& cachedSnapshot,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx,
                     uint32_t (*millisFn)(void* ctx), void* millisCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendBackup(server, cachedSnapshot, millisFn, millisCtx);
}

void handleApiBackupNow(WebServer& server,
                        bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                        void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleBackupNow(server);
}

void handleApiRestore(WebServer& server,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                      void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleRestore(server);
}

}  // namespace BackupApiService
