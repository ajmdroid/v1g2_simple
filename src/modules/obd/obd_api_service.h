#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include "../../settings.h"

class OBDHandler;
class V1BLEClient;

namespace ObdApiService {

struct ConnectRequest {
    String address;
    String name;
    String pin;
    bool remember = true;
    bool autoConnect = false;
};

void sendStatus(WebServer& server,
                OBDHandler& obdHandler,
                V1BLEClient& bleClient,
                const V1Settings& settings);

void sendDevices(WebServer& server, OBDHandler& obdHandler);
void sendRemembered(WebServer& server, OBDHandler& obdHandler);

bool parseConnectRequest(WebServer& server, ConnectRequest& out, String& errorMessage);
bool parseVwDataEnabledRequest(WebServer& server,
                               bool fallback,
                               bool& enabledOut,
                               String& errorMessage);
bool parseRememberedAutoConnectRequest(WebServer& server,
                                       String& addressOut,
                                       bool& enabledOut,
                                       String& errorMessage);
bool parseForgetAddressRequest(WebServer& server, String& addressOut, String& errorMessage);

}  // namespace ObdApiService
