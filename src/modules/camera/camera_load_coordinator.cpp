#include "camera_load_coordinator.h"

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
    // Start loading cameras now (don't wait for BLE connection)
    // With binary format, loading is fast (~1.6s for 71k cameras)
    if (!cameraManager || !storageManager) return;
    if (loadStarted || complete) return;
    
    loadStarted = true;
    pending = false;
    
    doLoad();
}

void CameraLoadCoordinator::process(bool bleConnected) {
    if (!cameraManager || !storageManager) return;
    if (!pending || complete) return;
    
    // Legacy path: if pending and BLE connected, start loading
    // (This path is now rarely used since we call startImmediateLoad() at boot)
    if (!bleConnected) return;

    pending = false;
    loadStarted = true;
    
    doLoad();
}

void CameraLoadCoordinator::doLoad() {
    Serial.println("[Camera] Initializing camera alerts...");
    fs::FS* sdFs = storageManager->getFilesystem();

    // Debug: check what files exist on SD
    if (sdFs) {
        Serial.printf("[Camera] SD filesystem available, checking for files...\n");
        Serial.printf("[Camera] /alpr.bin exists: %s\n", sdFs->exists("/alpr.bin") ? "YES" : "no");
        Serial.printf("[Camera] /alpr.json exists: %s\n", sdFs->exists("/alpr.json") ? "YES" : "no");
        Serial.printf("[Camera] /speed_cam.bin exists: %s\n", sdFs->exists("/speed_cam.bin") ? "YES" : "no");
        Serial.printf("[Camera] /redlight_cam.bin exists: %s\n", sdFs->exists("/redlight_cam.bin") ? "YES" : "no");
    } else {
        Serial.println("[Camera] SD filesystem is NULL!");
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
