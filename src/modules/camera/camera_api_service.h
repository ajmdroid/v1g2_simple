#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

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

/// GET /api/cameras/status wrapper with route-level policy callbacks.
void handleApiStatus(WebServer& server,
                     CameraRuntimeModule& cameraRuntimeModule,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

/// GET /api/cameras/catalog wrapper with route-level policy callbacks.
void handleApiCatalog(WebServer& server,
                      StorageManager& storageManager,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity);

/// GET /api/cameras/events wrapper with route-level policy callbacks.
void handleApiEvents(WebServer& server,
                     CameraRuntimeModule& cameraRuntimeModule,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

/// POST /api/cameras/demo wrapper with route-level policy callbacks.
void handleApiDemo(WebServer& server,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity);

/// POST /api/cameras/demo/clear wrapper with route-level policy callbacks.
void handleApiDemoClear(WebServer& server,
                        const std::function<bool()>& checkRateLimit,
                        const std::function<void()>& markUiActivity);

}  // namespace CameraApiService
