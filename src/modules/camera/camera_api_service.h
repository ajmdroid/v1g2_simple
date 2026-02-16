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
inline void handleApiStatus(WebServer& server,
                            CameraRuntimeModule& cameraRuntimeModule,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendStatus(server, cameraRuntimeModule);
}

/// GET /api/cameras/catalog wrapper with route-level policy callbacks.
inline void handleApiCatalog(WebServer& server,
                             StorageManager& storageManager,
                             const std::function<bool()>& checkRateLimit,
                             const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendCatalog(server, storageManager);
}

/// GET /api/cameras/events wrapper with route-level policy callbacks.
inline void handleApiEvents(WebServer& server,
                            CameraRuntimeModule& cameraRuntimeModule,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendEvents(server, cameraRuntimeModule);
}

/// POST /api/cameras/demo wrapper with route-level policy callbacks.
inline void handleApiDemo(WebServer& server,
                          const std::function<bool()>& checkRateLimit,
                          const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDemo(server);
}

/// POST /api/cameras/demo/clear wrapper with route-level policy callbacks.
inline void handleApiDemoClear(WebServer& server,
                               const std::function<bool()>& checkRateLimit,
                               const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDemoClear(server);
}

}  // namespace CameraApiService
