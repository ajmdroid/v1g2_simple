#pragma once

#include <Arduino.h>
#include <WebServer.h>

namespace BackupApiService {

/// GET /api/settings/backup — export all settings + profiles as JSON.
void sendBackup(WebServer& server);

/// POST /api/settings/restore — import settings + profiles from JSON body.
void handleRestore(WebServer& server);

}  // namespace BackupApiService
