/**
 * WiFi Manager for V1 Gen2 Display
 * AP-only: always-on access point serving the local UI/API
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>
#include "settings.h"

// Setup Mode state (AP is always on)
enum SetupModeState {
    SETUP_MODE_OFF = 0,
    SETUP_MODE_AP_ON,
};

class WiFiManager {
public:
    WiFiManager();
    
    // AP control (AP-only for configuration)
    bool startSetupMode();      // Start AP for configuration (idempotent)
    bool stopSetupMode(bool manual = false); // Stop AP (timeout/manual)
    bool toggleSetupMode(bool manual = false); // Toggle AP state (e.g., via button)
    bool isSetupModeActive() const { return setupModeState == SETUP_MODE_AP_ON; }
    
    // Process web server requests (call in loop)
    void process();
    
    // Legacy compatibility (redirects to Setup Mode)
    bool begin() { return startSetupMode(); }
    
    // Status
    bool isConnected() const { return false; }  // No STA mode
    bool isAPActive() const { return setupModeState == SETUP_MODE_AP_ON; }
    String getIPAddress() const { return ""; }  // No STA
    String getAPIPAddress() const;
    
    // Callbacks for alert data (to display on web page)
    void setAlertCallback(std::function<String()> callback) { getAlertJson = callback; }
    void setStatusCallback(std::function<String()> callback) { getStatusJson = callback; }
    
    // Callback for V1 commands (dark mode, mute)
    void setCommandCallback(std::function<bool(const char*, bool)> callback) { sendV1Command = callback; }
    
    // Callback for filesystem access (SD card)
    void setFilesystemCallback(std::function<fs::FS*()> callback) { getFilesystem = callback; }
    
    // Callback for push executor status (auto-push)
    void setPushStatusCallback(std::function<String()> callback) { getPushStatusJson = callback; }
    
    // Web activity tracking (for WiFi priority mode)
    void markUiActivity();  // Call on every HTTP request
    bool isUiActive(unsigned long timeoutMs = 30000) const;  // True if request within timeout

private:
    WebServer server;
    SetupModeState setupModeState;
    unsigned long setupModeStartTime;
    unsigned long lastClientSeenMs = 0;  // Tracks last STA presence for timeout
    
    // Web activity tracking for WiFi priority mode
    unsigned long lastUiActivityMs = 0;
    
    // Rate limiting
    static constexpr int RATE_LIMIT_WINDOW_MS = 1000;  // 1 second window
    static constexpr int RATE_LIMIT_MAX_REQUESTS = 20; // Max requests per window
    unsigned long rateLimitWindowStart;
    int rateLimitRequestCount;
    bool checkRateLimit();  // Returns true if request allowed, false if rate limited
    
    // Status JSON caching (Option 2 optimization)
    static constexpr unsigned long STATUS_CACHE_TTL_MS = 500;  // 500ms cache
    String cachedStatusJson;
    unsigned long lastStatusJsonTime = 0;
    
    std::function<String()> getAlertJson;
    std::function<String()> getStatusJson;
    std::function<bool(const char*, bool)> sendV1Command;
    std::function<fs::FS*()> getFilesystem;
    std::function<String()> getPushStatusJson;
    
    // Setup functions
    void setupAP();
    void setupWebServer();
    void checkAutoTimeout();
    
    // Web handlers
    void handleStatus();
    void handleSettingsApi();
    void handleSettingsSave();
    void handleDarkMode();
    void handleMute();
    void handleV1ProfilesList();
    void handleV1ProfileGet();
    void handleV1ProfileSave();
    void handleV1ProfileDelete();
    void handleV1SettingsPull();
    void handleV1SettingsPush();
    void handleV1CurrentSettings();
    
    // API endpoints
    void handleApiProfilePush();      // POST /api/profile/push - queue profile push
    void handleAutoPushSlotsApi();
    void handleAutoPushSlotSave();
    void handleAutoPushActivate();
    void handleAutoPushPushNow();
    void handleAutoPushStatus();
    void handleDisplayColorsApi();
    void handleDisplayColorsSave();
    void handleDisplayColorsReset();
    void handleDebugMetrics();
    void handleDebugEvents();
    void handleDebugEventsClear();
    void handleDebugEnable();
    void handleSettingsBackup();
    void handleSettingsRestore();
    void handleObdStatus();
    void handleObdScan();
    void handleObdDevices();
    void handleObdConnect();
    void handleNotFound();
    
    // LittleFS file serving (new UI)
    bool serveLittleFSFile(const char* path, const char* contentType);
};

// Global instance
extern WiFiManager wifiManager;

#endif // WIFI_MANAGER_H
