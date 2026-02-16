#pragma once

#include <WebServer.h>

#include <functional>

class CameraRuntimeModule;
class StorageManager;

namespace CameraApiService {

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
