#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

namespace BackupApiService {

/// GET /api/settings/backup — export all settings + profiles as JSON.
void sendBackup(WebServer& server);

/// POST /api/settings/restore — import settings + profiles from JSON body.
void handleRestore(WebServer& server);

/// GET /api/settings/backup wrapper with route-level UI activity callback.
inline void handleApiBackup(WebServer& server,
                            const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendBackup(server);
}

/// POST /api/settings/restore wrapper with route-level policy callbacks.
inline void handleApiRestore(WebServer& server,
                             const std::function<bool()>& checkRateLimit,
                             const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleRestore(server);
}

}  // namespace BackupApiService
