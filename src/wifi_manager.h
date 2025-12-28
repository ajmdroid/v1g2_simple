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

private:
    WebServer server;
    bool apActive;
    
    std::function<String()> getAlertJson;
    std::function<String()> getStatusJson;
    std::function<bool(const char*, bool)> sendV1Command;
    
    // Setup functions
    void setupAP();
    void setupWebServer();
    
    // Web handlers
    void handleStatus();
    void handleSettings();
    void handleSettingsSave();
    void handleDarkMode();
    void handleMute();
    void handleLogs();
    void handleLogsData();
    void handleLogsClear();
    void handleV1Settings();
    void handleV1ProfilesList();
    void handleV1ProfileGet();
    void handleV1ProfileSave();
    void handleV1ProfileDelete();
    void handleV1SettingsPull();
    void handleV1SettingsPush();
    void handleV1CurrentSettings();
    void handleAutoPush();
    void handleAutoPushSlotSave();
    void handleAutoPushActivate();
    void handleAutoPushPushNow();
    void handleDisplayColors();
    void handleDisplayColorsSave();
    void handleDisplayColorsReset();
    void handleNotFound();
    
    // HTML generation
    String generateSettingsHTML();
    String generateLogsHTML();
    String generateV1SettingsHTML();
    String generateAutoPushHTML();
    String generateAutoPushSettingsJSON();
    String generateDisplayColorsHTML();
    String generateProfileOptions(const String& selected);
};

// Global instance
extern WiFiManager wifiManager;

#endif // WIFI_MANAGER_H
