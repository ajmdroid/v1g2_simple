#include "backup_api_service.h"

#include <ArduinoJson.h>

#include "../../settings.h"
#include "../../storage_manager.h"
#include "../../v1_profiles.h"
#include "../../backup_payload_builder.h"
#include "../gps/gps_runtime_module.h"
#include "../gps/gps_lockout_safety.h"
#include "../lockout/lockout_band_policy.h"
#include "../obd/obd_runtime_module.h"
#include "../speed/speed_source_selector.h"
#include "json_stream_response.h"

namespace BackupApiService {

static void sendBackup(WebServer& server,
                       BackupSnapshotCache& cachedSnapshot,
                       const std::function<uint32_t()>& millisFn) {
    Serial.println("[HTTP] GET /api/settings/backup");
    server.sendHeader("Content-Disposition", "attachment; filename=\"v1simple_backup.json\"");
    sendCachedBackupSnapshot(
        server,
        cachedSnapshot,
        settingsManager.backupRevision(),
        v1ProfileManager.catalogRevision(),
        [](JsonDocument& doc, uint32_t snapshotMs) {
            BackupPayloadBuilder::buildBackupDocument(
                doc,
                settingsManager.get(),
                v1ProfileManager,
                BackupPayloadBuilder::BackupTransport::HttpDownload,
                snapshotMs);
        },
        millisFn);
}

static void handleBackupNow(WebServer& server) {
    Serial.println("[HTTP] POST /api/settings/backup-now");
    sendBackupNowResponse(server, BackupNowRuntime{
        []() { return storageManager.isReady(); },
        []() { return storageManager.isSDCard(); },
        []() { return settingsManager.backupToSD(); },
    });
}

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

static void handleRestore(WebServer& server) {
    Serial.println("[HTTP] POST /api/settings/restore");
    static constexpr size_t kMaxRestoreBodyBytes = 16 * 1024;
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"No JSON body provided\"}");
        return;
    }

    if (server.hasHeader("Content-Length")) {
        long declaredLength = server.header("Content-Length").toInt();
        if (declaredLength < 0 || static_cast<size_t>(declaredLength) > kMaxRestoreBodyBytes) {
            server.send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
            return;
        }
    }
    
    String body = server.arg("plain");
    if (body.length() > kMaxRestoreBodyBytes) {
        server.send(413, "application/json", "{\"success\":false,\"error\":\"Body too large\"}");
        return;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        Serial.printf("[Settings] Restore parse error: %s\n", err.c_str());
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid JSON\"}");
        return;
    }
    
    // Verify backup format
    if (!doc["_type"].is<const char*>() ||
        !BackupPayloadBuilder::isRecognizedBackupType(doc["_type"].as<const char*>())) {
        server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid backup format\"}");
        return;
    }
    
    const SettingsBackupApplyResult applyResult = settingsManager.applyBackupDocument(doc, true);
    if (!applyResult.success) {
        server.send(500, "application/json", "{\"success\":false,\"error\":\"Failed to persist restored settings\"}");
        return;
    }

    gpsRuntimeModule.setEnabled(settingsManager.get().gpsEnabled);
    obdRuntimeModule.setEnabled(settingsManager.get().obdEnabled);
    obdRuntimeModule.setMinRssi(settingsManager.get().obdMinRssi);
    speedSourceSelector.syncEnabledInputs(settingsManager.get().gpsEnabled,
                                         settingsManager.get().obdEnabled);
    lockoutSetKaLearningEnabled(settingsManager.get().gpsLockoutKaLearningEnabled);
    lockoutSetKLearningEnabled(settingsManager.get().gpsLockoutKLearningEnabled);
    lockoutSetXLearningEnabled(settingsManager.get().gpsLockoutXLearningEnabled);
    
    Serial.printf("[Settings] Restored from uploaded backup (%d profiles)\n", applyResult.profilesRestored);
    
    // Build response with profile count
    String response = "{\"success\":true,\"message\":\"Settings restored successfully";
    if (applyResult.profilesRestored > 0) {
        response += " (" + String(applyResult.profilesRestored) + " profiles)";
    }
    response += "\"}";
    server.send(200, "application/json", response);
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
