#include "obd_api_service.h"

namespace ObdApiService {

void handleScan(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient);
void handleConnect(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient);

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

}  // namespace ObdApiService
