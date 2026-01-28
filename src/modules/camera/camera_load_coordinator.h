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

    bool isComplete() const { return complete; }

    void process(bool bleConnected);

private:
    CameraManager* cameraManager = nullptr;
    StorageManager* storageManager = nullptr;
    DebugLogger* debugLogger = nullptr;

    bool pending = false;
    bool complete = false;
};
