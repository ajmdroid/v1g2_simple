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
void handleApiBackup(WebServer& server,
                     const std::function<void()>& markUiActivity);

/// POST /api/settings/restore wrapper with route-level policy callbacks.
void handleApiRestore(WebServer& server,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity);

}  // namespace BackupApiService
