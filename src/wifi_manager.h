/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: setup AP for local UI/API plus optional STA for external network.
 * AP may be dropped dynamically (e.g., after STA connect) while WiFi service
 * state remains active.
 */

#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>
#include "settings.h"
#include "modules/wifi/wifi_status_api_service.h"
#include <functional>

namespace WifiAutoPushApiService {
struct Runtime;
}

namespace WifiDisplayColorsApiService {
struct Runtime;
}

namespace WifiTimeApiService {
struct TimeRuntime;
}

namespace WifiSettingsApiService {
struct Runtime;
}

namespace WifiClientApiService {
struct Runtime;
}

namespace WifiV1ProfileApiService {
struct Runtime;
}

namespace WifiV1DevicesApiService {
struct Runtime;
}

// WiFi service state (AP may be enabled or disabled while service is active)
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
    static constexpr uint32_t WIFI_RUNTIME_MIN_FREE_STA_ONLY = 16384;   // 16KB
    static constexpr uint32_t WIFI_RUNTIME_MIN_BLOCK_STA_ONLY = 7168;   // 7KB
    static constexpr uint32_t WIFI_RUNTIME_MIN_FREE_AP_STA = 20480;     // 20KB
    static constexpr uint32_t WIFI_RUNTIME_MIN_BLOCK_AP_STA = 8192;    // 8KB (was 10KB; FreeRTOS task stacks fragment heap)
    // AP+STA can hover a few bytes below free-heap floor from allocator churn.
    // Ignore tiny deficits to avoid WARN/RECOVER oscillation near the boundary.
    static constexpr uint32_t WIFI_RUNTIME_AP_STA_FREE_JITTER_TOLERANCE = 256;
    // STA-only mode can oscillate within a few dozen bytes of the largest-block
    // threshold due to allocator churn; ignore tiny deficits to avoid WARN spam.
    static constexpr uint32_t WIFI_RUNTIME_STA_BLOCK_JITTER_TOLERANCE = 128;
    static constexpr unsigned long WIFI_LOW_DMA_PERSIST_MS = 1500;      // Require sustained low heap before shutdown
    static constexpr unsigned long WIFI_LOW_DMA_RETRY_COOLDOWN_MS = 30000; // Avoid rapid start/stop thrash

    // AP control (AP-only for configuration)
    bool startSetupMode();      // Start AP for configuration (idempotent)
    bool stopSetupMode(bool manual = false, const char* reason = nullptr); // Stop AP (manual/timeout/low_dma)
    bool toggleSetupMode(bool manual = false); // Toggle AP state (e.g., via button)
    bool isWifiServiceActive() const { return setupModeState == SETUP_MODE_AP_ON; }
    bool isSetupModeActive() const { return setupModeState == SETUP_MODE_AP_ON && apInterfaceEnabled; }
    
    // Process web server requests (call in loop)
    void process();
    
    // Legacy compatibility (redirects to Setup Mode)
    bool begin() { return startSetupMode(); }

    // Preflight check for setup-mode start admission.
    bool canStartSetupMode(uint32_t* freeInternal = nullptr, uint32_t* largestInternal = nullptr) const;
    unsigned long lowDmaCooldownRemainingMs() const;
    
    // Reset WiFi reconnect failure counter and debounce timer
    // (call when user manually triggers WiFi)
    void resetReconnectFailures() { wifiReconnectFailures = 0; lastReconnectAttemptMs = 0; }
    
    // Status
    bool isConnected() const { return wifiClientState == WIFI_CLIENT_CONNECTED; }
    bool isAPActive() const { return setupModeState == SETUP_MODE_AP_ON && apInterfaceEnabled; }
    String getIPAddress() const;  // STA IP when connected
    String getAPIPAddress() const;
    
    // WiFi client (STA) control - connect to external network
    WifiClientState getWifiClientState() const { return wifiClientState; }
    bool startWifiScan();  // Async scan for networks
    bool isWifiScanRunning() const { return wifiScanRunning; }
    std::vector<ScannedNetwork> getScannedNetworks();  // Get scan results (clears running flag)
    bool connectToNetwork(const String& ssid,
                          const String& password,
                          bool persistCredentialsOnSuccess = true);
    void disconnectFromNetwork();
    void checkWifiClientStatus();  // Call in loop() to manage STA connection
    String getConnectedSSID() const;  // Returns empty if not connected
    
    // Callbacks for alert data (to display on web page)
    void setAlertCallback(std::function<void(JsonObject)> callback) { mergeAlert = callback; }
    void setStatusCallback(std::function<void(JsonObject)> callback) { mergeStatus = callback; }
    
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

    // Mark this WiFi session as auto-started (shorter no-client grace period)
    void markAutoStarted() { wasAutoStarted = true; }
    
    // Web activity tracking (for WiFi priority mode)
    void markUiActivity();  // Call on every HTTP request
    bool isUiActive(unsigned long timeoutMs = 30000) const;  // True if request within timeout

private:
    WebServer server;
    bool webRoutesInitialized = false;
    SetupModeState setupModeState;
    bool apInterfaceEnabled = false;  // True only when softAP interface is enabled
    unsigned long setupModeStartTime;
    unsigned long lastClientSeenMs = 0;  // Tracks last STA presence for timeout
    unsigned long lastApStaCountPollMs = 0;
    int cachedApStaCount = 0;
    static constexpr unsigned long AP_STA_COUNT_POLL_MS = 250;
    // Keep request handling hot while amortizing lower-priority maintenance work.
    static constexpr unsigned long WIFI_MAINTENANCE_FAST_MS = 10;
    static constexpr unsigned long WIFI_STATUS_CHECK_MS = 50;
    static constexpr unsigned long WIFI_TIMEOUT_CHECK_MS = 250;
    unsigned long lastMaintenanceFastMs = 0;
    unsigned long lastStatusCheckMs = 0;
    unsigned long lastTimeoutCheckMs = 0;
    
    // WiFi client (STA) state
    WifiClientState wifiClientState = WIFI_CLIENT_DISABLED;
    bool wifiScanRunning = false;
    unsigned long wifiConnectStartMs = 0;
    static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;  // 15s connection timeout
    static constexpr unsigned long WIFI_MODE_SWITCH_SETTLE_MS = 100;  // Preserve existing settle windows, non-blocking
    static constexpr unsigned long WIFI_STOP_PHASE_SETTLE_MS = 8;      // Spread teardown work over loop ticks
    String pendingConnectSSID;
    String pendingConnectPassword;
    bool pendingConnectPersistCredentials = true;
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

    enum class WifiStopPhase : uint8_t {
        IDLE = 0,
        STOP_HTTP_SERVER,
        DISCONNECT_STA,
        DISABLE_AP,
        MODE_OFF,
        FINALIZE,
    };
    WifiStopPhase wifiStopPhase = WifiStopPhase::IDLE;
    unsigned long wifiStopPhaseStartMs = 0;
    unsigned long wifiStopStartMs = 0;
    String wifiStopReason;
    bool wifiStopManual = false;
    bool wifiStopHadSta = false;
    bool wifiStopHadAp = false;
    
    // WiFi reconnect failure tracking (prevents memory leak from repeated failed attempts)
    int wifiReconnectFailures = 0;
    unsigned long lastReconnectAttemptMs = 0;  // Moved from static local for proper reset across WiFi sessions
    static constexpr int WIFI_MAX_RECONNECT_FAILURES = 5;  // Give up after 5 failures
    static constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000;  // 30s between attempts
    static constexpr unsigned long WIFI_RECONNECT_DEFER_NO_V1_MS = 90000;  // Protect BLE acquisition on boot
    bool wifiReconnectDeferredLogged = false;
    
    // Web activity tracking for WiFi priority mode
    unsigned long lastUiActivityMs = 0;

    // Low-DMA protection state (prevents rapid restart loops under heap pressure)
    unsigned long lowDmaCooldownUntilMs = 0;
    unsigned long lowDmaSinceMs = 0;
    // If neither STA nor AP has any connected client for long enough, shut WiFi
    // down until manual restart to preserve core runtime headroom.
    static constexpr unsigned long WIFI_NO_CLIENT_SHUTDOWN_MS = 60000;
    // Shorter grace for auto-started WiFi: if nobody connects within one STA
    // timeout cycle, shut down promptly to reclaim DMA headroom.
    static constexpr unsigned long WIFI_NO_CLIENT_SHUTDOWN_AUTO_MS = 20000;
    // When STA is connected, keep AP alive briefly for setup-page races, then
    // retire AP once no AP clients have been seen for this long.
    static constexpr unsigned long WIFI_AP_IDLE_DROP_AFTER_STA_MS = 60000;
    unsigned long lastAnyClientSeenMs = 0;
    bool wasAutoStarted = false;  // True when WiFi was started by boot auto-start (not manual)

    // Rate limiting
    static constexpr int RATE_LIMIT_WINDOW_MS = 60000;  // 60 second window (1 minute)
    static constexpr int RATE_LIMIT_MAX_REQUESTS = 120; // Max 120 requests per minute
    unsigned long rateLimitWindowStart;
    int rateLimitRequestCount;
    bool checkRateLimit();  // Returns true if request allowed, false if rate limited
    
    // Status JSON caching (Option 2 optimization)
    static constexpr unsigned long STATUS_CACHE_TTL_MS = 500;  // 500ms cache
    WifiStatusApiService::StatusJsonCache cachedStatusJson;
    unsigned long lastStatusJsonTime = 0;
    
    std::function<void(JsonObject)> mergeAlert;
    std::function<void(JsonObject)> mergeStatus;
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
    void processStopSetupModePhase();
    void finalizeStopSetupMode();
    bool stopSetupModeImmediate(bool emergencyLowDma);
    WifiAutoPushApiService::Runtime makeAutoPushRuntime();
    WifiDisplayColorsApiService::Runtime makeDisplayColorsRuntime();
    WifiTimeApiService::TimeRuntime makeTimeRuntime();
    WifiStatusApiService::StatusRuntime makeStatusRuntime();
    WifiSettingsApiService::Runtime makeSettingsRuntime();
    WifiClientApiService::Runtime makeWifiClientRuntime();
    WifiV1ProfileApiService::Runtime makeV1ProfileRuntime();
    WifiV1DevicesApiService::Runtime makeV1DevicesRuntime();
    
    // API endpoints
    void handleNotFound();
    
    // LittleFS file serving (new UI)
    bool serveLittleFSFile(const char* path, const char* contentType);
};

// Global instance
extern WiFiManager wifiManager;

#endif // WIFI_MANAGER_H
