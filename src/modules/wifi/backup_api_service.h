#pragma once

#include <WebServer.h>

#include <functional>

#include "backup_snapshot_cache.h"

namespace BackupApiService {

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
