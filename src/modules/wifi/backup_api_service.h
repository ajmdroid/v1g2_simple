#pragma once

#include <WebServer.h>

#include <functional>

namespace BackupApiService {

/// GET /api/settings/backup wrapper with route-level UI activity callback.
void handleApiBackup(WebServer& server,
                     const std::function<void()>& markUiActivity);

/// POST /api/settings/restore wrapper with route-level policy callbacks.
void handleApiRestore(WebServer& server,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity);

}  // namespace BackupApiService
