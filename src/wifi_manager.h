/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: always-on access point serving the local UI/API
 *         plus optional station mode to connect to external network
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>
#include "settings.h"
#include <functional>

// Setup Mode state (AP is always on, STA is optional)
enum SetupModeState {
    SETUP_MODE_OFF = 0,
    SETUP_MODE_AP_ON,
};

// WiFi client (STA) connection state
enum WifiClientState {
    WIFI_CLIENT_DISABLED = 0,
    WIFI_CLIENT_DISCONNECTED,
    WIFI_CLIENT_CONNECTING,
    WIFI_CLIENT_CONNECTED,
    WIFI_CLIENT_FAILED,
};

// Scanned network info
struct ScannedNetwork {
    String ssid;
    int32_t rssi;
    uint8_t encryptionType;  // WIFI_AUTH_OPEN, WIFI_AUTH_WPA2_PSK, etc.
    bool isOpen() const { return encryptionType == WIFI_AUTH_OPEN; }
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
    bool isConnected() const { return wifiClientState == WIFI_CLIENT_CONNECTED; }
    bool isAPActive() const { return setupModeState == SETUP_MODE_AP_ON; }
    String getIPAddress() const;  // STA IP when connected
    String getAPIPAddress() const;
    
    // WiFi client (STA) control - connect to external network
    WifiClientState getWifiClientState() const { return wifiClientState; }
    bool startWifiScan();  // Async scan for networks
    bool isWifiScanRunning() const { return wifiScanRunning; }
    std::vector<ScannedNetwork> getScannedNetworks();  // Get scan results (clears running flag)
    bool connectToNetwork(const String& ssid, const String& password);
    void disconnectFromNetwork();
    void checkWifiClientStatus();  // Call in loop() to manage STA connection
    String getConnectedSSID() const;  // Returns empty if not connected
    
    // Callbacks for alert data (to display on web page)
    void setAlertCallback(std::function<String()> callback) { getAlertJson = callback; }
    void setStatusCallback(std::function<String()> callback) { getStatusJson = callback; }
    
    // Callback for V1 commands (dark mode, mute)
    void setCommandCallback(std::function<bool(const char*, bool)> callback) { sendV1Command = callback; }
    
    // Callback to request a profile push (manual trigger from API)
    void setProfilePushCallback(std::function<bool()> callback) { requestProfilePush = callback; }
    
    // Callback for filesystem access (SD card)
    void setFilesystemCallback(std::function<fs::FS*()> callback) { getFilesystem = callback; }
    
    // Callback for push executor status (auto-push)
    void setPushStatusCallback(std::function<String()> callback) { getPushStatusJson = callback; }
    
    // Callback for GPS status
    void setGpsStatusCallback(std::function<String()> callback) { getGpsStatusJson = callback; }
    void setGpsResetCallback(std::function<void()> callback) { gpsResetCallback = callback; }
    
    // Callback for camera alerts
    void setCameraStatusCallback(std::function<String()> callback) { getCameraStatusJson = callback; }
    void setCameraReloadCallback(std::function<bool()> callback) { cameraReloadCallback = callback; }
    void setCameraUploadCallback(std::function<bool(const String&)> callback) { cameraUploadCallback = callback; }
    void setCameraTestCallback(std::function<void(int)> callback) { cameraTestCallback = callback; }
    
    // Web activity tracking (for WiFi priority mode)
    void markUiActivity();  // Call on every HTTP request
    bool isUiActive(unsigned long timeoutMs = 30000) const;  // True if request within timeout

private:
    WebServer server;
    SetupModeState setupModeState;
    unsigned long setupModeStartTime;
    unsigned long lastClientSeenMs = 0;  // Tracks last STA presence for timeout
    
    // WiFi client (STA) state
    WifiClientState wifiClientState = WIFI_CLIENT_DISABLED;
    bool wifiScanRunning = false;
    unsigned long wifiConnectStartMs = 0;
    static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;  // 15s connection timeout
    String pendingConnectSSID;
    String pendingConnectPassword;
    
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
    std::function<bool()> requestProfilePush;
    std::function<fs::FS*()> getFilesystem;
    std::function<String()> getPushStatusJson;
    std::function<String()> getGpsStatusJson;
    std::function<void()> gpsResetCallback;
    std::function<String()> getCameraStatusJson;
    std::function<bool()> cameraReloadCallback;
    std::function<bool(const String&)> cameraUploadCallback;
    std::function<void(int)> cameraTestCallback;
    
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
    void handleDebugLogsMeta();
    void handleDebugLogsDownload();
    void handleDebugLogsTail();
    void handleDebugLogsClear();
    void handleSettingsBackup();
    void handleSettingsRestore();
    void handleObdStatus();
    void handleObdScan();
    void handleObdScanStop();
    void handleObdDevices();
    void handleObdDevicesClear();
    void handleObdConnect();
    void handleObdForget();
    void handleGpsStatus();
    void handleGpsReset();
    void handleCameraStatus();
    void handleCameraReload();
    void handleCameraUpload();
    void handleCameraTest();
    void handleCameraSyncOsm();
    void handleWifiClientStatus();
    void handleWifiClientScan();
    void handleWifiClientConnect();
    void handleWifiClientDisconnect();
    void handleWifiClientForget();
    void handleNotFound();
    
    // LittleFS file serving (new UI)
    bool serveLittleFSFile(const char* path, const char* contentType);
};

// Global instance
extern WiFiManager wifiManager;

#endif // WIFI_MANAGER_H
