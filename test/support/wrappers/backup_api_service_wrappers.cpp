#include "../../../src/modules/wifi/backup_api_service.h"

namespace BackupApiService {

// Internal implementation entrypoints defined in backup_api_service.cpp.
void sendBackup(WebServer& server,
                BackupSnapshotCache& cachedSnapshot,
                const std::function<uint32_t()>& millisFn);
void handleBackupNow(WebServer& server);
void handleRestore(WebServer& server);

void handleApiBackup(WebServer& server,
                     BackupSnapshotCache& cachedSnapshot,
                     const std::function<void()>& markUiActivity,
                     const std::function<uint32_t()>& millisFn) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendBackup(server, cachedSnapshot, millisFn);
}

void handleApiBackupNow(WebServer& server,
                        const std::function<bool()>& checkRateLimit,
                        const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleBackupNow(server);
}

void handleApiRestore(WebServer& server,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleRestore(server);
}

}  // namespace BackupApiService
