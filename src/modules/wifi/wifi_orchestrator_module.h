#pragma once

#include <WiFi.h>
#include <ArduinoJson.h>

#include "wifi_manager.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "storage_manager.h"
#include "settings.h"
#include "modules/auto_push/auto_push_module.h"
class QuietCoordinatorModule;

class WifiOrchestrator {
public:
    WifiOrchestrator(WiFiManager& wifiManager,
                     V1BLEClient& bleClient,
                     PacketParser& parser,
                     SettingsManager& settingsManager,
                     StorageManager& storageManager,
                     AutoPushModule& autoPushModule,
                     QuietCoordinatorModule& quietCoordinator);

    void ensureCallbacksConfigured();

private:
    void configureCallbacks();

    WiFiManager& wifiManager;
    V1BLEClient& bleClient;
    PacketParser& parser;
    SettingsManager& settingsManager;
    StorageManager& storageManager;
    AutoPushModule& autoPushModule;
    QuietCoordinatorModule& quietCoordinator;
    bool callbacksConfigured = false;
};
