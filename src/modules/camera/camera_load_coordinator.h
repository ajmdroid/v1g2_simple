#pragma once

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include "camera_manager.h"
#include "storage_manager.h"
#include "debug_logger.h"

class CameraLoadCoordinator {
public:
    void begin(CameraManager* cameraMgr, StorageManager* storageMgr, DebugLogger* dbgLogger);

    void markPending(bool pending = true);
    
    // Start loading immediately (don't wait for BLE connection)
    // Call this at boot to start camera loading in parallel with BLE init
    void startImmediateLoad();

    bool isComplete() const { return complete; }

    void process(bool bleConnected);

private:
    void doLoad();
    
    CameraManager* cameraManager = nullptr;
    StorageManager* storageManager = nullptr;
    DebugLogger* debugLogger = nullptr;

    bool pending = false;
    bool loadStarted = false;
    bool complete = false;
};
