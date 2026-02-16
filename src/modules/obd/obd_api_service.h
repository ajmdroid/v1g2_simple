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

void sendStatus(WebServer& server,
                OBDHandler& obdHandler,
                V1BLEClient& bleClient,
                const V1Settings& settings);

void sendDevices(WebServer& server, OBDHandler& obdHandler);
void sendRemembered(WebServer& server, OBDHandler& obdHandler);
void handleScan(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient);
void handleScanStop(WebServer& server, OBDHandler& obdHandler);
void handleDevicesClear(WebServer& server, OBDHandler& obdHandler);
void handleConnect(WebServer& server, OBDHandler& obdHandler, V1BLEClient& bleClient);
void handleDisconnect(WebServer& server, OBDHandler& obdHandler);
void handleConfig(WebServer& server, OBDHandler& obdHandler, SettingsManager& settingsManager);
void handleRememberedAutoConnect(WebServer& server, OBDHandler& obdHandler);
void handleForget(WebServer& server, OBDHandler& obdHandler);

bool parseConnectRequest(WebServer& server, ConnectRequest& out, String& errorMessage);
bool parseConfigRequest(WebServer& server,
                        bool enabledFallback,
                        bool vwDataEnabledFallback,
                        ConfigRequest& requestOut,
                        String& errorMessage);
bool parseRememberedAutoConnectRequest(WebServer& server,
                                       String& addressOut,
                                       bool& enabledOut,
                                       String& errorMessage);
bool parseForgetAddressRequest(WebServer& server, String& addressOut, String& errorMessage);

inline void handleApiStatus(WebServer& server,
                            OBDHandler& obdHandler,
                            V1BLEClient& bleClient,
                            const V1Settings& settings,
                            const std::function<bool()>& checkRateLimit,
                            const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendStatus(server, obdHandler, bleClient, settings);
}

inline void handleApiScan(WebServer& server,
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

inline void handleApiScanStop(WebServer& server,
                              OBDHandler& obdHandler,
                              const std::function<bool()>& checkRateLimit,
                              const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleScanStop(server, obdHandler);
}

inline void handleApiDevices(WebServer& server,
                             OBDHandler& obdHandler,
                             const std::function<bool()>& checkRateLimit,
                             const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendDevices(server, obdHandler);
}

inline void handleApiDevicesClear(WebServer& server,
                                  OBDHandler& obdHandler,
                                  const std::function<bool()>& checkRateLimit,
                                  const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDevicesClear(server, obdHandler);
}

inline void handleApiConnect(WebServer& server,
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

inline void handleApiDisconnect(WebServer& server,
                                OBDHandler& obdHandler,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleDisconnect(server, obdHandler);
}

inline void handleApiConfig(WebServer& server,
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

inline void handleApiRemembered(WebServer& server,
                                OBDHandler& obdHandler,
                                const std::function<bool()>& checkRateLimit,
                                const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendRemembered(server, obdHandler);
}

inline void handleApiRememberedAutoConnect(WebServer& server,
                                           OBDHandler& obdHandler,
                                           const std::function<bool()>& checkRateLimit,
                                           const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleRememberedAutoConnect(server, obdHandler);
}

inline void handleApiForget(WebServer& server,
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
