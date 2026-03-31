/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: setup AP for local UI/API plus optional STA for external network.
 * AP may be dropped dynamically (e.g., after STA connect) while WiFi service
 * state remains active.
 */

#pragma once
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>
#include "../include/wifi_rate_limiter.h"
#include "settings.h"
#include "modules/wifi/backup_snapshot_cache.h"
#include "modules/wifi/wifi_autopush_api_service.h"
#include "modules/wifi/wifi_control_api_service.h"
#include "modules/wifi/wifi_status_api_service.h"

namespace WifiDisplayColorsApiService {
struct Runtime;
}

namespace WifiAudioApiService {
struct Runtime;
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
    SETUP_MODE_STOPPING,
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
    bool startSetupMode(bool autoStarted = false);      // Start or re-enable AP for configuration
    bool stopSetupMode(bool manual = false, const char* reason = nullptr); // Stop AP (manual/timeout/low_dma)
    bool isWifiServiceActive() const { return setupModeState_ == SETUP_MODE_AP_ON; }
    bool isSetupModeActive() const { return setupModeState_ == SETUP_MODE_AP_ON && apInterfaceEnabled_; }
    bool isStopping() const;
    bool hasPendingLifecycleWork() const;
    void setBoundaryTransitionAdmission(bool allow);

    // Process web server requests (call in loop)
    void process();

    // Legacy compatibility (redirects to Setup Mode)
    bool begin() { return startSetupMode(false); }

    // Preflight check for setup-mode start admission.
    bool canStartSetupMode(uint32_t* freeInternal = nullptr, uint32_t* largestInternal = nullptr) const;
    unsigned long lowDmaCooldownRemainingMs() const;

    // Reset WiFi reconnect failure counter and debounce timer
    // (call when user manually triggers WiFi)
    void resetReconnectFailures() { wifiReconnectFailures_ = 0; lastReconnectAttemptMs_ = 0; }

    // Returns true when STA has exhausted all reconnect attempts and given up.
    // Clears automatically when resetReconnectFailures() is called.
    bool isReconnectGaveUp() const { return wifiReconnectFailures_ >= WIFI_MAX_RECONNECT_FAILURES; }

    // Status
    bool isConnected() const { return !isStopping() && wifiClientState_ == WIFI_CLIENT_CONNECTED; }
    String getIPAddress() const;  // STA IP when connected
    String getAPIPAddress() const;

    // WiFi client (STA) control - connect to external network
    bool startWifiScan();  // Async scan for networks
    std::vector<ScannedNetwork> getScannedNetworks();  // Get scan results (clears running flag)
    bool connectToNetwork(const String& ssid,
                          const String& password,
                          bool persistCredentialsOnSuccess = true);
    void disconnectFromNetwork();
    void checkWifiClientStatus();  // Called internally by process() to manage STA connection
    String getConnectedSSID() const;  // Returns empty if not connected

    // Callbacks for alert data (to display on web page)
    void setAlertCallback(void (*fn)(JsonObject, void*), void* ctx) { mergeAlert_ = fn; mergeAlertCtx_ = ctx; }
    void setStatusCallback(void (*fn)(JsonObject, void*), void* ctx) { mergeStatus_ = fn; mergeStatusCtx_ = ctx; }
    void appendStatusCallback(void (*fn)(JsonObject, void*), void* ctx) {
        mergeStatus2_ = fn;
        mergeStatus2Ctx_ = ctx;
    }

    // Callback for V1 commands (dark mode, mute)
    void setCommandCallback(bool (*fn)(const char*, bool, void*), void* ctx) { sendV1Command_ = fn; sendV1CommandCtx_ = ctx; }

    // Callback to request a profile push (manual trigger from API)
    void setProfilePushCallback(WifiControlApiService::ProfilePushResult (*fn)(void*), void* ctx) { requestProfilePush_ = fn; requestProfilePushCtx_ = ctx; }

    // Callback for filesystem access (SD card)
    void setFilesystemCallback(fs::FS* (*fn)(void*), void* ctx) { getFilesystem_ = fn; getFilesystemCtx_ = ctx; }

    // Callback for push executor status (auto-push)
    void setPushStatusCallback(String (*fn)(void*), void* ctx) { getPushStatusJson_ = fn; getPushStatusJsonCtx_ = ctx; }

    // Callback for manual push-now requests routed through the shared executor.
    void setPushNowCallback(
        WifiAutoPushApiService::PushNowQueueResult (*fn)(
            const WifiAutoPushApiService::PushNowRequest&, void*),
        void* ctx) {
        queuePushNow_ = fn;
        queuePushNowCtx_ = ctx;
    }

    // Callback for V1 connection state (used to defer WiFi client operations)
    void setV1ConnectedCallback(bool (*fn)(void*), void* ctx) { isV1Connected_ = fn; isV1ConnectedCtx_ = ctx; }

    // Web activity tracking (for WiFi priority mode)
    void markUiActivity();  // Call on every HTTP request
    bool isUiActive(unsigned long timeoutMs = 30000) const;  // True if request within timeout

private:
    WebServer server_;
    bool webRoutesInitialized_ = false;
    SetupModeState setupModeState_;
    bool apInterfaceEnabled_ = false;  // True only when softAP interface is enabled
    unsigned long setupModeStartTime_;
    unsigned long lastClientSeenMs_ = 0;  // Tracks last STA presence for timeout
    unsigned long lastApStaCountPollMs_ = 0;
    int cachedApStaCount_ = 0;
    static constexpr unsigned long AP_STA_COUNT_POLL_MS = 250;
    // Keep request handling hot while amortizing lower-priority maintenance work.
    static constexpr unsigned long WIFI_MAINTENANCE_FAST_MS = 10;
    static constexpr unsigned long WIFI_STATUS_CHECK_MS = 50;
    static constexpr unsigned long WIFI_TIMEOUT_CHECK_MS = 250;
    unsigned long lastMaintenanceFastMs_ = 0;
    unsigned long lastStatusCheckMs_ = 0;
    unsigned long lastTimeoutCheckMs_ = 0;

    // WiFi client (STA) state
    WifiClientState wifiClientState_ = WIFI_CLIENT_DISABLED;
    bool wifiScanRunning_ = false;
    unsigned long wifiConnectStartMs_ = 0;
    static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;  // 15s connection timeout
    static constexpr unsigned long WIFI_MODE_SWITCH_SETTLE_MS = 100;  // Preserve existing settle windows, non-blocking
    static constexpr unsigned long WIFI_STOP_PHASE_SETTLE_MS = 8;      // Spread teardown work over loop ticks
    String pendingConnectSSID_;
    String pendingConnectPassword_;
    bool pendingConnectPersistCredentials_ = true;
    enum class WifiConnectPhase : uint8_t {
        IDLE = 0,
        PREPARE_OFF,
        WAIT_OFF,
        ENABLE_AP_STA,
        WAIT_AP_STA,
        BEGIN_CONNECT,
    };
    WifiConnectPhase wifiConnectPhase_ = WifiConnectPhase::IDLE;
    unsigned long wifiConnectPhaseStartMs_ = 0;

    enum class WifiStopPhase : uint8_t {
        IDLE = 0,
        STOP_HTTP_SERVER,
        DISCONNECT_STA,
        DISABLE_AP,
        MODE_OFF,
        FINALIZE,
    };
    WifiStopPhase wifiStopPhase_ = WifiStopPhase::IDLE;
    unsigned long wifiStopPhaseStartMs_ = 0;
    unsigned long wifiStopStartMs_ = 0;
    String wifiStopReason_;
    bool wifiStopManual_ = false;
    bool wifiStopHadSta_ = false;
    bool wifiStopHadAp_ = false;
    bool allowBoundaryTransitionWork_ = false;

    // WiFi reconnect failure tracking (prevents memory leak from repeated failed attempts)
    int wifiReconnectFailures_ = 0;
    unsigned long lastReconnectAttemptMs_ = 0;  // Moved from static local for proper reset across WiFi sessions
    static constexpr int WIFI_MAX_RECONNECT_FAILURES = 5;  // Give up after 5 failures
    static constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000;  // 30s between attempts
    static constexpr unsigned long WIFI_RECONNECT_DEFER_NO_V1_MS = 90000;  // Protect BLE acquisition on boot
    bool wifiReconnectDeferredLogged_ = false;

    // Web activity tracking for WiFi priority mode
    unsigned long lastUiActivityMs_ = 0;

    // Low-DMA protection state (prevents rapid restart loops under heap pressure)
    unsigned long lowDmaCooldownUntilMs_ = 0;
    unsigned long lowDmaSinceMs_ = 0;
    // If neither STA nor AP has any connected client for long enough, shut WiFi
    // down until manual restart to preserve core runtime headroom.
    static constexpr unsigned long WIFI_NO_CLIENT_SHUTDOWN_MS = 60000;
    // Shorter grace for auto-started WiFi: if nobody connects within one STA
    // timeout cycle, shut down promptly to reclaim DMA headroom.
    static constexpr unsigned long WIFI_NO_CLIENT_SHUTDOWN_AUTO_MS = 20000;
    // When STA is connected, keep AP alive briefly for setup-page races, then
    // retire AP once no AP clients have been seen for this long.
    static constexpr unsigned long WIFI_AP_IDLE_DROP_AFTER_STA_MS = 60000;
    unsigned long lastAnyClientSeenMs_ = 0;
    bool wasAutoStarted_ = false;  // True when WiFi was started by boot auto-start (not manual)

    // Rate limiting
    static constexpr unsigned long RATE_LIMIT_WINDOW_MS = SlidingWindowRateLimiter::WINDOW_MS;
    static constexpr size_t RATE_LIMIT_MAX_REQUESTS = SlidingWindowRateLimiter::MAX_REQUESTS;
    SlidingWindowRateLimiter rateLimiter_;
    bool checkRateLimit();  // Returns true if request allowed, false if rate limited

    // Status JSON caching (Option 2 optimization)
    static constexpr unsigned long STATUS_CACHE_TTL_MS = 500;  // 500ms cache
    WifiStatusApiService::StatusJsonCache cachedStatusJson_;
    unsigned long lastStatusJsonTime_ = 0;
    BackupApiService::BackupSnapshotCache cachedBackupSnapshot_;

    void (*mergeAlert_)(JsonObject, void* ctx) = nullptr;
    void* mergeAlertCtx_ = nullptr;
    void (*mergeStatus_)(JsonObject, void* ctx) = nullptr;
    void* mergeStatusCtx_ = nullptr;
    void (*mergeStatus2_)(JsonObject, void* ctx) = nullptr;   // appended by appendStatusCallback
    void* mergeStatus2Ctx_ = nullptr;
    bool (*sendV1Command_)(const char*, bool, void* ctx) = nullptr;
    void* sendV1CommandCtx_ = nullptr;
    WifiControlApiService::ProfilePushResult (*requestProfilePush_)(void* ctx) = nullptr;
    void* requestProfilePushCtx_ = nullptr;
    fs::FS* (*getFilesystem_)(void* ctx) = nullptr;
    void* getFilesystemCtx_ = nullptr;
    String (*getPushStatusJson_)(void* ctx) = nullptr;
    void* getPushStatusJsonCtx_ = nullptr;
    WifiAutoPushApiService::PushNowQueueResult (*queuePushNow_)(
        const WifiAutoPushApiService::PushNowRequest&, void* ctx) = nullptr;
    void* queuePushNowCtx_ = nullptr;
    bool (*isV1Connected_)(void* ctx) = nullptr;   // Returns true when V1 is connected (defer WiFi ops until then)
    void* isV1ConnectedCtx_ = nullptr;

    // Setup functions
    void setupAP();
    bool setupWebServer();
    void checkAutoTimeout();
    void processWifiClientConnectPhase();
    bool enableWifiClientFromSavedCredentials();
    void disableWifiClient();
    void forgetWifiClient();
    void processStopSetupModePhase();
    void finalizeStopSetupMode();
    bool stopSetupModeImmediate(bool emergencyLowDma);
    WifiAutoPushApiService::Runtime makeAutoPushRuntime();
    WifiDisplayColorsApiService::Runtime makeDisplayColorsRuntime();
    WifiAudioApiService::Runtime makeAudioRuntime();
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
