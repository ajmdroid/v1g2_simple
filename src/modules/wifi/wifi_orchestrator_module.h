#pragma once

#include <functional>
#include <WiFi.h>
#include <ArduinoJson.h>

#include "wifi_manager.h"
#include "debug_logger.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "storage_manager.h"
#include "gps_handler.h"
#include "camera_manager.h"
#include "settings.h"
#include "modules/camera/camera_alert_module.h"
#include "modules/auto_push/auto_push_module.h"

class WifiOrchestrator {
public:
    WifiOrchestrator(WiFiManager& wifiManager,
                     DebugLogger& debugLogger,
                     V1BLEClient& bleClient,
                     PacketParser& parser,
                     SettingsManager& settingsManager,
                     StorageManager& storageManager,
                     GPSHandler& gpsHandler,
                     CameraManager& cameraManager,
                     CameraAlertModule& cameraAlertModule,
                     AutoPushModule& autoPushModule,
                     std::function<void(int)> profilePushFn);

    void startWifi();

private:
    void configureCallbacks();

    WiFiManager& wifiManager;
    DebugLogger& debugLogger;
    V1BLEClient& bleClient;
    PacketParser& parser;
    SettingsManager& settingsManager;
    StorageManager& storageManager;
    GPSHandler& gpsHandler;
    CameraManager& cameraManager;
    CameraAlertModule& cameraAlertModule;
    AutoPushModule& autoPushModule;
    std::function<void(int)> profilePushFn;
    bool callbacksConfigured = false;
};
