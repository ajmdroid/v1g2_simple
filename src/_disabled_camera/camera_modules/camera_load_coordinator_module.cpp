#include "camera_load_coordinator_module.h"
#include "settings.h"

extern SettingsManager settingsManager;

void CameraLoadCoordinator::begin(CameraManager* cameraMgr, StorageManager* storageMgr, DebugLogger* dbgLogger) {
    cameraManager = cameraMgr;
    storageManager = storageMgr;
    debugLogger = dbgLogger;
}

void CameraLoadCoordinator::markPending(bool isPending) {
    pending = isPending;
    if (!isPending) {
        complete = false;
    }
}

void CameraLoadCoordinator::startImmediateLoad() {
    if (!settingsManager.isFeaturesRuntimeEnabled() || !settingsManager.get().cameraAlertsEnabled) return;
    // Start loading cameras now (don't wait for BLE connection)
    // With binary format, loading is fast (~1.6s for 71k cameras)
    if (!cameraManager || !storageManager) return;
    if (loadStarted || complete) return;
    
    loadStarted = true;
    pending = false;
    
    doLoad();
}

void CameraLoadCoordinator::process(bool bleConnected) {
    if (!settingsManager.isFeaturesRuntimeEnabled() || !settingsManager.get().cameraAlertsEnabled) return;
    if (!cameraManager || !storageManager) return;

    // Poll background load completion
    if (loadStarted && !complete) {
        if (!cameraManager->isBackgroundLoading() && cameraManager->isLoaded()) {
            complete = true;
            Serial.printf("[Camera] Background load complete: %d cameras\n",
                          cameraManager->getCameraCount());
        }
        return;
    }

    if (!pending || complete) return;
    
    // Legacy path: if pending and BLE connected, start loading
    // (This path is now rarely used since we call startImmediateLoad() at boot)
    if (!bleConnected) return;

    pending = false;
    loadStarted = true;
    
    doLoad();
}

void CameraLoadCoordinator::doLoad() {
    if (!settingsManager.isFeaturesRuntimeEnabled() || !settingsManager.get().cameraAlertsEnabled) return;
    fs::FS* sdFs = storageManager->getFilesystem();

    if (!sdFs) {
        Serial.println("[Camera] SD filesystem is NULL!");
        return;
    }

    bool hasCachedData = cameraManager->loadRegionalCache(&LittleFS, "/cameras_cache.json");
    if (hasCachedData) {
        Serial.printf("[Camera] Regional cache loaded: %d cameras (instant alerts ready)\n", 
                      cameraManager->getRegionalCacheCount());
    }

    if (sdFs && (sdFs->exists("/alpr.bin") || sdFs->exists("/alpr.json") || 
                 sdFs->exists("/redlight_cam.bin") || sdFs->exists("/redlight_cam.json") || 
                 sdFs->exists("/speed_cam.bin") || sdFs->exists("/speed_cam.json"))) {
        cameraManager->setFilesystem(sdFs);

        if (cameraManager->startBackgroundLoad()) {
            Serial.println("[Camera] Background database load started (won't block V1)");
        } else {
            Serial.println("[Camera] Background load failed, using synchronous load...");
            if (cameraManager->begin(sdFs)) {
                Serial.printf("[Camera] Database loaded: %d cameras\n", 
                              cameraManager->getCameraCount());
                complete = true;
            }
        }
    } else if (!hasCachedData) {
        Serial.println("[Camera] No camera database or cache found");
    }

    // Mark complete if we have any data source ready
    if (hasCachedData || (sdFs && cameraManager->isLoaded())) {
        complete = true;
    }
}
