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

    // Internal SRAM guardrails for WiFi lifecycle.
    // AP+STA needs more headroom than AP-only.
    static constexpr uint32_t WIFI_START_MIN_FREE_AP_ONLY = 28672;      // 28KB
    static constexpr uint32_t WIFI_START_MIN_BLOCK_AP_ONLY = 10240;     // 10KB
    static constexpr uint32_t WIFI_START_MIN_FREE_AP_STA = 40960;       // 40KB
    static constexpr uint32_t WIFI_START_MIN_BLOCK_AP_STA = 20480;      // 20KB
    static constexpr uint32_t WIFI_RUNTIME_MIN_FREE_AP_ONLY = 16384;    // 16KB
    static constexpr uint32_t WIFI_RUNTIME_MIN_BLOCK_AP_ONLY = 8192;    // 8KB
    static constexpr uint32_t WIFI_RUNTIME_MIN_FREE_AP_STA = 20480;     // 20KB
    static constexpr uint32_t WIFI_RUNTIME_MIN_BLOCK_AP_STA = 8192;    // 8KB (was 10KB; FreeRTOS task stacks fragment heap)
    static constexpr unsigned long WIFI_LOW_DMA_PERSIST_MS = 1500;      // Require sustained low heap before shutdown
    static constexpr unsigned long WIFI_LOW_DMA_RETRY_COOLDOWN_MS = 30000; // Avoid rapid start/stop thrash

    // AP control (AP-only for configuration)
    bool startSetupMode();      // Start AP for configuration (idempotent)
    bool stopSetupMode(bool manual = false, const char* reason = nullptr); // Stop AP (manual/timeout/low_dma)
    bool toggleSetupMode(bool manual = false); // Toggle AP state (e.g., via button)
    bool isSetupModeActive() const { return setupModeState == SETUP_MODE_AP_ON; }
    
    // Process web server requests (call in loop)
    void process();
    
    // Legacy compatibility (redirects to Setup Mode)
    bool begin() { return startSetupMode(); }

    // Preflight check for setup-mode start admission.
    bool canStartSetupMode(uint32_t* freeInternal = nullptr, uint32_t* largestInternal = nullptr) const;
    unsigned long lowDmaCooldownRemainingMs() const;
    
    // Reset WiFi reconnect failure counter (call when user manually triggers WiFi)
    void resetReconnectFailures() { wifiReconnectFailures = 0; }
    
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
    
    // Callback for V1 connection state (used to defer WiFi client operations)
    void setV1ConnectedCallback(std::function<bool()> callback) { isV1Connected = callback; }
    
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
    static constexpr unsigned long WIFI_MODE_SWITCH_SETTLE_MS = 100;  // Preserve existing settle windows, non-blocking
    String pendingConnectSSID;
    String pendingConnectPassword;
    enum class WifiConnectPhase : uint8_t {
        IDLE = 0,
        PREPARE_OFF,
        WAIT_OFF,
        ENABLE_AP_STA,
        WAIT_AP_STA,
        BEGIN_CONNECT,
    };
    WifiConnectPhase wifiConnectPhase = WifiConnectPhase::IDLE;
    unsigned long wifiConnectPhaseStartMs = 0;
    
    // WiFi reconnect failure tracking (prevents memory leak from repeated failed attempts)
    int wifiReconnectFailures = 0;
    static constexpr int WIFI_MAX_RECONNECT_FAILURES = 5;  // Give up after 5 failures
    static constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000;  // 30s between attempts
    static constexpr unsigned long WIFI_RECONNECT_DEFER_NO_V1_MS = 90000;  // Protect BLE acquisition on boot
    bool wifiReconnectDeferredLogged = false;
    
    // Web activity tracking for WiFi priority mode
    unsigned long lastUiActivityMs = 0;

    // Low-DMA protection state (prevents rapid restart loops under heap pressure)
    unsigned long lowDmaCooldownUntilMs = 0;
    unsigned long lowDmaSinceMs = 0;
    
    // Rate limiting
    static constexpr int RATE_LIMIT_WINDOW_MS = 60000;  // 60 second window (1 minute)
    static constexpr int RATE_LIMIT_MAX_REQUESTS = 120; // Max 120 requests per minute
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
    std::function<bool()> isV1Connected;  // Returns true when V1 is connected (defer WiFi ops until then)

    enum class PushNowStep : uint8_t {
        IDLE = 0,
        WRITE_PROFILE,
        SET_DISPLAY,
        SET_MODE,
        SET_VOLUME,
    };

    struct PushNowState {
        PushNowStep step = PushNowStep::IDLE;
        unsigned long nextAtMs = 0;
        uint8_t retries = 0;
        int slot = 0;
        uint8_t profileBytes[6] = {0};
        bool displayOn = true;
        bool applyMode = false;
        V1Mode mode = V1_MODE_UNKNOWN;
        bool applyVolume = false;
        uint8_t mainVol = 0xFF;
        uint8_t muteVol = 0xFF;
    } pushNowState;
    static constexpr uint8_t PUSH_NOW_MAX_RETRIES = 8;
    static constexpr unsigned long PUSH_NOW_RETRY_DELAY_MS = 30;
    
    // Setup functions
    void setupAP();
    void setupWebServer();
    void checkAutoTimeout();
    void processWifiClientConnectPhase();
    void processPendingPushNow();
    
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
    void handleTimeSet();             // POST /api/time/set - set trusted epoch base
    void handleAutoPushSlotsApi();
    void handleAutoPushSlotSave();
    void handleAutoPushActivate();
    void handleAutoPushPushNow();
    void handleAutoPushStatus();
    void handleDisplayColorsApi();
    void handleDisplayColorsSave();
    void handleDisplayColorsReset();
    void handleWifiClientStatus();
    void handleWifiClientScan();
    void handleWifiClientConnect();
    void handleWifiClientDisconnect();
    void handleWifiClientForget();
    void handleWifiClientEnable();
    void handleNotFound();
    
    // LittleFS file serving (new UI)
    bool serveLittleFSFile(const char* path, const char* contentType);
};

// Global instance
extern WiFiManager wifiManager;

#endif // WIFI_MANAGER_H
