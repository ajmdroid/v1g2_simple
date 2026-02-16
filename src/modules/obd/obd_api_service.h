#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

class OBDHandler;
class V1BLEClient;
struct V1Settings;
class SettingsManager;

namespace ObdApiService {

struct ConnectRequest {
    String address;
    String name;
    String pin;
    bool remember = true;
    bool autoConnect = false;
};

struct ConfigRequest {
    bool hasEnabled = false;
    bool enabled = false;
    bool hasVwDataEnabled = false;
    bool vwDataEnabled = false;
};

void handleApiStatus(WebServer& server,
                     OBDHandler& obdHandler,
                     V1BLEClient& bleClient,
                     const V1Settings& settings,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

void handleApiScan(WebServer& server,
                   OBDHandler& obdHandler,
                   V1BLEClient& bleClient,
                   const std::function<bool()>& checkRateLimit,
                   const std::function<void()>& markUiActivity,
                   const std::function<bool()>& checkObdEnabled);

void handleApiScanStop(WebServer& server,
                       OBDHandler& obdHandler,
                       const std::function<bool()>& checkRateLimit,
                       const std::function<void()>& markUiActivity);

void handleApiDevices(WebServer& server,
                      OBDHandler& obdHandler,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity);

void handleApiDevicesClear(WebServer& server,
                           OBDHandler& obdHandler,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity);

void handleApiConnect(WebServer& server,
                      OBDHandler& obdHandler,
                      V1BLEClient& bleClient,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity,
                      const std::function<bool()>& checkObdEnabled);

void handleApiDisconnect(WebServer& server,
                         OBDHandler& obdHandler,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

void handleApiConfig(WebServer& server,
                     OBDHandler& obdHandler,
                     SettingsManager& settingsManager,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

void handleApiRemembered(WebServer& server,
                         OBDHandler& obdHandler,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity);

void handleApiRememberedAutoConnect(WebServer& server,
                                    OBDHandler& obdHandler,
                                    const std::function<bool()>& checkRateLimit,
                                    const std::function<void()>& markUiActivity);

void handleApiForget(WebServer& server,
                     OBDHandler& obdHandler,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity);

}  // namespace ObdApiService
