#include "camera_api_service.h"

namespace CameraApiService {

#ifdef UNIT_TEST
void sendStatus(WebServer& server,
                CameraRuntimeModule& cameraRuntimeModule);

void handleApiStatus(WebServer& server,
                     CameraRuntimeModule& cameraRuntimeModule,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendStatus(server, cameraRuntimeModule);
}

void sendCatalog(WebServer& server,
                 StorageManager& storageManager);

void handleApiCatalog(WebServer& server,
                      StorageManager& storageManager,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendCatalog(server, storageManager);
}

void sendEvents(WebServer& server,
                CameraRuntimeModule& cameraRuntimeModule);

void handleApiEvents(WebServer& server,
                     CameraRuntimeModule& cameraRuntimeModule,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendEvents(server, cameraRuntimeModule);
}

void handleDemo(WebServer& server);

void handleApiDemo(WebServer& server,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDemo(server);
}
#endif

void handleDemoClear(WebServer& server);

void handleApiDemoClear(WebServer& server,
                        const std::function<bool()>& checkRateLimit,
                        const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDemoClear(server);
}

}  // namespace CameraApiService
