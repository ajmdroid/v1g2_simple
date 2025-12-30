/**
 * WiFi Manager for V1 Gen2 Display
 * Handles WiFi AP/STA modes and web server
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>
#include "settings.h"

// Constants
constexpr unsigned long STA_CONNECTION_RETRY_INTERVAL_MS = 3000;
constexpr unsigned long STA_CONNECTION_TIMEOUT_MS = 5000;
constexpr int NTP_SYNC_RETRY_COUNT = 10;
constexpr unsigned long NTP_SYNC_RETRY_DELAY_MS = 500;

#include <WiFiMulti.h>

class WiFiManager {
public:
    WiFiManager();
    
    // Initialize WiFi based on settings
    bool begin();
    
    // Process web server requests (call in loop)
    void process();
    
    // Stop WiFi
    void stop();
    
    // Status
    bool isConnected() const;
    bool isAPActive() const;
    String getIPAddress() const;
    String getAPIPAddress() const;
    
    // Callbacks for alert data (to display on web page)
    void setAlertCallback(std::function<String()> callback) { getAlertJson = callback; }
    void setStatusCallback(std::function<String()> callback) { getStatusJson = callback; }
    
    // Callback for V1 commands (dark mode, mute)
    void setCommandCallback(std::function<bool(const char*, bool)> callback) { sendV1Command = callback; }
    
    // Callback for filesystem access (SD card)
    void setFilesystemCallback(std::function<fs::FS*()> callback) { getFilesystem = callback; }

private:
    WebServer server;
    WiFiMulti wifiMulti;
    bool apActive;
    bool staConnected;
    bool staEnabledByConfig;  // Track if STA was enabled (by config or auto-enable)
    bool natEnabled;
    unsigned long lastStaRetry;
    bool timeInitialized;
    String connectedSSID;
    
    std::function<String()> getAlertJson;
    std::function<String()> getStatusJson;
    std::function<bool(const char*, bool)> sendV1Command;
    std::function<fs::FS*()> getFilesystem;
    
    // Setup functions
    void setupAP();
    void setupSTA();
    int populateStaNetworks();
    void checkSTAConnection();
    void setupWebServer();
    void initializeTime();
    void enableNAT();
    
    // Web handlers
    void handleStatus();
    void handleSettingsApi();
    void handleSettingsSave();
    void handleTimeSettingsSave();
    void handleDarkMode();
    void handleMute();
    void handleLogsData();
    void handleLogsClear();
    void handleSerialLog();
    void handleSerialLogClear();
    void handleV1ProfilesList();
    void handleV1ProfileGet();
    void handleV1ProfileSave();
    void handleV1ProfileDelete();
    void handleV1SettingsPull();
    void handleV1SettingsPush();
    void handleV1CurrentSettings();
    void handleV1DevicesApi();
    void handleV1DeviceNameSave();
    void handleV1DeviceProfileSave();
    void handleV1DeviceDelete();
    void handleAutoPushSlotsApi();
    void handleAutoPushSlotSave();
    void handleAutoPushActivate();
    void handleAutoPushPushNow();
    void handleDisplayColorsApi();
    void handleDisplayColorsSave();
    void handleDisplayColorsReset();
    void handleTimeSettingsApi();
    void handleSerialLogApi();
    void handleSerialLogToggle();
    void handleSerialLogContent();
    void handleNotFound();
    
    // LittleFS file serving (new UI)
    bool serveLittleFSFile(const char* path, const char* contentType);
};

// Global instance
extern WiFiManager wifiManager;

#endif // WIFI_MANAGER_H
