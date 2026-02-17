#include "obd_api_service.h"

namespace ObdApiService {

void sendRemembered(WebServer& server, OBDHandler& obdHandler);
void handleScan(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient);
void handleScanStop(WebServer& server, OBDHandler& obdHandler);
void handleDevicesClear(WebServer& server, OBDHandler& obdHandler);
void handleConnect(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient);
void handleDisconnect(WebServer& server, OBDHandler& obdHandler);
void handleConfig(WebServer& server, OBDHandler& obdHandler, SettingsManager& settingsManager);
void handleRememberedAutoConnect(WebServer& server, OBDHandler& obdHandler);
void handleForget(WebServer& server, OBDHandler& obdHandler);

void handleApiScan(WebServer& server,
                   OBDHandler& obdHandler,
                   V1BLEClient& bleClient,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity,
                   const std::function<bool()>& checkObdEnabled) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    if (checkObdEnabled && !checkObdEnabled()) return;
    handleScan(server, obdHandler, bleClient);
}

void handleApiScanStop(WebServer& server,
                       OBDHandler& obdHandler,
                       const std::function<bool()>& checkRateLimit,
                       const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleScanStop(server, obdHandler);
}

void handleApiDevicesClear(WebServer& server,
                           OBDHandler& obdHandler,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDevicesClear(server, obdHandler);
}

void handleApiConnect(WebServer& server,
                      OBDHandler& obdHandler,
                      V1BLEClient& bleClient,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity,
                      const std::function<bool()>& checkObdEnabled) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    if (checkObdEnabled && !checkObdEnabled()) return;
    handleConnect(server, obdHandler, bleClient);
}

void handleApiDisconnect(WebServer& server,
                         OBDHandler& obdHandler,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDisconnect(server, obdHandler);
}

void handleApiConfig(WebServer& server,
                     OBDHandler& obdHandler,
                     SettingsManager& settingsManager,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleConfig(server, obdHandler, settingsManager);
}

void handleApiRemembered(WebServer& server,
                         OBDHandler& obdHandler,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendRemembered(server, obdHandler);
}

void handleApiRememberedAutoConnect(WebServer& server,
                                    OBDHandler& obdHandler,
                                    const std::function<bool()>& checkRateLimit,
                                    const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleRememberedAutoConnect(server, obdHandler);
}

void handleApiForget(WebServer& server,
                     OBDHandler& obdHandler,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleForget(server, obdHandler);
}

}  // namespace ObdApiService
