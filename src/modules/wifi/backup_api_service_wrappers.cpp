#include "backup_api_service.h"

namespace BackupApiService {

// Internal implementation entrypoints defined in backup_api_service.cpp.
#ifdef UNIT_TEST
void sendBackup(WebServer& server);

void handleApiBackup(WebServer& server,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendBackup(server);
}
#endif

void handleRestore(WebServer& server);

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
