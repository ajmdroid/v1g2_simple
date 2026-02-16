#pragma once

#include <Arduino.h>
#include <WebServer.h>

class CameraRuntimeModule;
class StorageManager;

namespace CameraApiService {

/// GET /api/cameras/status — runtime status, index stats, event log stats.
void sendStatus(WebServer& server,
                CameraRuntimeModule& cameraRuntimeModule);

/// GET /api/cameras/catalog — SD-card dataset inventory (ALPR/speed/redlight).
void sendCatalog(WebServer& server,
                 StorageManager& storageManager);

/// GET /api/cameras/events — recent camera event log entries.
void sendEvents(WebServer& server,
                CameraRuntimeModule& cameraRuntimeModule);

/// POST /api/cameras/demo — trigger camera preview on display.
void handleDemo(WebServer& server);

/// POST /api/cameras/demo/clear — cancel active display preview.
void handleDemoClear(WebServer& server);

}  // namespace CameraApiService
