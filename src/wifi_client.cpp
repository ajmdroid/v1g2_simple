/**
 * WiFi Client — STA connection, scan, and reconnect lifecycle.
 * Extracted from wifi_manager.cpp for maintainability.
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include <algorithm>
#include <map>
#include <vector>


String WiFiManager::getAPIPAddress() const {
    if (isSetupModeActive()) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

String WiFiManager::getIPAddress() const {
    if (wifiClientState_ == WIFI_CLIENT_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

String WiFiManager::getConnectedSSID() const {
    if (wifiClientState_ == WIFI_CLIENT_CONNECTED) {
        return WiFi.SSID();
    }
    return "";
}

bool WiFiManager::startWifiScan() {
    if (wifiScanRunning_) {
        Serial.println("[WiFiClient] Scan already in progress");
        return false;
    }

    Serial.println("[WiFiClient] Starting async network scan...");
    WiFi.scanDelete();  // Clear previous results

    // Start async scan (non-blocking)
    int result = WiFi.scanNetworks(true, false, false, 300);  // async=true, show_hidden=false, passive=false, max_ms_per_chan=300
    if (result == WIFI_SCAN_RUNNING) {
        wifiScanRunning_ = true;
        return true;
    }

    Serial.printf("[WiFiClient] Scan failed to start: %d\n", result);
    return false;
}

std::vector<ScannedNetwork> WiFiManager::getScannedNetworks() {
    std::vector<ScannedNetwork> networks;

    int16_t scanResult = WiFi.scanComplete();
    if (scanResult == WIFI_SCAN_RUNNING) {
        // Still scanning
        return networks;  // Empty
    }

    wifiScanRunning_ = false;

    if (scanResult == WIFI_SCAN_FAILED || scanResult < 0) {
        Serial.printf("[WiFiClient] Scan failed: %d\n", scanResult);
        return networks;
    }

    Serial.printf("[WiFiClient] Scan found %d networks\n", scanResult);

    // Deduplicate by SSID (keep strongest signal)
    std::map<String, ScannedNetwork> uniqueNetworks;

    for (int i = 0; i < scanResult; i++) {
        String ssid = WiFi.SSID(i);
        if (ssid.length() == 0) continue;  // Skip hidden networks

        int32_t rssi = WiFi.RSSI(i);
        uint8_t encType = WiFi.encryptionType(i);

        auto it = uniqueNetworks.find(ssid);
        if (it == uniqueNetworks.end() || rssi > it->second.rssi) {
            uniqueNetworks[ssid] = {ssid, rssi, encType};
        }
    }

    // Convert to vector and sort by signal strength
    for (const auto& pair : uniqueNetworks) {
        networks.push_back(pair.second);
    }

    std::sort(networks.begin(), networks.end(), [](const ScannedNetwork& a, const ScannedNetwork& b) {
        return a.rssi > b.rssi;  // Strongest first
    });

    WiFi.scanDelete();  // Free memory
    return networks;
}

bool WiFiManager::connectToNetwork(const String& ssid,
                                   const String& password,
                                   bool persistCredentialsOnSuccess) {
    if (ssid.length() == 0) {
        Serial.println("[WiFiClient] Cannot connect: empty SSID");
        return false;
    }

    // Stage a non-blocking connect sequence to avoid stalling loop().
    pendingConnectSSID_ = ssid;
    pendingConnectPassword_ = password;
    pendingConnectPersistCredentials_ = persistCredentialsOnSuccess;
    wifiConnectStartMs_ = 0;
    wifiClientState_ = WIFI_CLIENT_CONNECTING;
    wifiConnectPhase_ = WifiConnectPhase::PREPARE_OFF;
    wifiConnectPhaseStartMs_ = millis();
    PERF_INC(wifiConnectDeferred);
    return true;
}

bool WiFiManager::enableWifiClientFromSavedCredentials() {
    settingsManager.setWifiClientEnabled(true);

    const String savedSsid = settingsManager.get().wifiClientSSID;
    if (savedSsid.length() == 0) {
        wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
        return true;
    }

    if (connectToNetwork(savedSsid, settingsManager.getWifiClientPassword())) {
        return true;
    }

    wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
    return false;
}

void WiFiManager::disconnectFromNetwork() {
    Serial.println("[WiFiClient] Disconnecting from network");
    wifiConnectPhase_ = WifiConnectPhase::IDLE;
    wifiConnectPhaseStartMs_ = 0;
    wifiConnectStartMs_ = 0;
    WiFi.disconnect(false);  // Don't turn off station mode
    wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
    pendingConnectSSID_ = "";
    pendingConnectPassword_ = "";
    pendingConnectPersistCredentials_ = true;
}

void WiFiManager::disableWifiClient() {
    disconnectFromNetwork();
    settingsManager.setWifiClientEnabled(false);
    wifiClientState_ = WIFI_CLIENT_DISABLED;
    WiFi.mode(WIFI_AP);
}

void WiFiManager::forgetWifiClient() {
    disconnectFromNetwork();
    settingsManager.clearWifiClientCredentials();
    wifiClientState_ = WIFI_CLIENT_DISABLED;
    WiFi.mode(WIFI_AP);
}

void WiFiManager::processWifiClientConnectPhase() {
    if (wifiConnectPhase_ == WifiConnectPhase::IDLE) {
        return;
    }

    unsigned long now = millis();
    switch (wifiConnectPhase_) {
        case WifiConnectPhase::PREPARE_OFF:
            if (isSetupModeActive()) {
                // Keep AP online and use a direct STA begin path.
                // Repeated STA resets in AP+STA mode have proven brittle on some routers.
                Serial.println("[WiFiClient] Preserving AP, preparing STA connect...");
                if (WiFi.getMode() != WIFI_AP_STA) {
                    WiFi.mode(WIFI_AP_STA);
                    wifiConnectPhaseStartMs_ = now;
                    wifiConnectPhase_ = WifiConnectPhase::WAIT_AP_STA;
                } else {
                    wifiConnectPhase_ = WifiConnectPhase::BEGIN_CONNECT;
                }
            } else {
                if (WiFi.getMode() != WIFI_OFF) {
                    Serial.println("[WiFiClient] Cleaning up WiFi before reconnect...");
                    WiFi.disconnect(false, false);  // Graceful release without credential erase
                    WiFi.mode(WIFI_OFF);          // Fully shut down WiFi driver
                }
                wifiConnectPhaseStartMs_ = now;
                wifiConnectPhase_ = WifiConnectPhase::WAIT_OFF;
            }
            break;

        case WifiConnectPhase::WAIT_OFF:
            if (now - wifiConnectPhaseStartMs_ >= WIFI_MODE_SWITCH_SETTLE_MS) {
                wifiConnectPhase_ = WifiConnectPhase::ENABLE_AP_STA;
            }
            break;

        case WifiConnectPhase::ENABLE_AP_STA:
            Serial.println("[WiFiClient] Initializing WiFi in AP+STA mode");
            WiFi.mode(WIFI_AP_STA);
            wifiConnectPhaseStartMs_ = now;
            wifiConnectPhase_ = WifiConnectPhase::WAIT_AP_STA;
            break;

        case WifiConnectPhase::WAIT_AP_STA:
            if (now - wifiConnectPhaseStartMs_ >= WIFI_MODE_SWITCH_SETTLE_MS) {
                wifiConnectPhase_ = WifiConnectPhase::BEGIN_CONNECT;
            }
            break;

        case WifiConnectPhase::BEGIN_CONNECT:
            if (pendingConnectSSID_.length() == 0) {
                wifiConnectPhase_ = WifiConnectPhase::IDLE;
                wifiClientState_ = WIFI_CLIENT_FAILED;
                break;
            }
            // Improve coexistence stability while connecting alongside BLE links.
            WiFi.setSleep(false);
            WiFi.setAutoReconnect(true);
            Serial.printf("[WiFiClient] Connecting to: %s\n", pendingConnectSSID_.c_str());
            WiFi.begin(pendingConnectSSID_.c_str(), pendingConnectPassword_.c_str());
            wifiConnectStartMs_ = now;
            wifiConnectPhase_ = WifiConnectPhase::IDLE;
            break;

        case WifiConnectPhase::IDLE:
        default:
            break;
    }
}

void WiFiManager::checkWifiClientStatus() {
    // Skip if WiFi client is disabled
    if (wifiClientState_ == WIFI_CLIENT_DISABLED) {
        return;
    }

    wl_status_t status = WiFi.status();

    switch (wifiClientState_) {
        case WIFI_CLIENT_CONNECTING: {
            // Non-blocking mode transition is still in progress.
            if (wifiConnectPhase_ != WifiConnectPhase::IDLE || wifiConnectStartMs_ == 0) {
                break;
            }

            if (status == WL_CONNECTED) {
                wifiClientState_ = WIFI_CLIENT_CONNECTED;
                wifiConnectStartMs_ = 0;
                Serial.printf("[WiFiClient] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
                if (isSetupModeActive()) {
                    // Arm AP idle timer from STA connect so setup UI clients have
                    // a full grace window before AP retirement.
                    lastClientSeenMs_ = millis();
                    Serial.println("[WiFiClient] STA connected; AP idle-retire timer armed");
                }

                // Reset failure counter on successful connection
                wifiReconnectFailures_ = 0;

                // Save credentials on successful connection
                if (pendingConnectSSID_.length() > 0) {
                    if (pendingConnectPersistCredentials_) {
                        const V1Settings& currentSettings = settingsManager.get();
                        const bool ssidChanged = (pendingConnectSSID_ != currentSettings.wifiClientSSID);
                        const bool passwordChanged =
                            (pendingConnectPassword_ != settingsManager.getWifiClientPassword());
                        if (ssidChanged || passwordChanged) {
                            settingsManager.setWifiClientCredentials(pendingConnectSSID_, pendingConnectPassword_);
                        } else {
                            Serial.println("[WiFiClient] Connected with unchanged credentials; skipping re-save");
                        }
                    } else {
                        Serial.println("[WiFiClient] Connected via auto-reconnect; skipping credential re-save");
                    }
                    pendingConnectSSID_ = "";
                    pendingConnectPassword_ = "";
                    pendingConnectPersistCredentials_ = true;
                }
            } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                wifiClientState_ = WIFI_CLIENT_FAILED;
                Serial.printf("[WiFiClient] Connection failed: %d\n", status);
                wifiConnectStartMs_ = 0;

                pendingConnectSSID_ = "";
                pendingConnectPassword_ = "";
                pendingConnectPersistCredentials_ = true;
            } else if (millis() - wifiConnectStartMs_ > WIFI_CONNECT_TIMEOUT_MS) {
                wifiClientState_ = WIFI_CLIENT_FAILED;
                Serial.println("[WiFiClient] Connection timeout");
                WiFi.disconnect(false);
                wifiConnectStartMs_ = 0;

                pendingConnectSSID_ = "";
                pendingConnectPassword_ = "";
                pendingConnectPersistCredentials_ = true;
            }
            break;
        }

        case WIFI_CLIENT_CONNECTED: {
            if (status != WL_CONNECTED) {
                wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
                Serial.println("[WiFiClient] Lost connection");

            }
            break;
        }

        case WIFI_CLIENT_DISCONNECTED:
        case WIFI_CLIENT_FAILED: {
            if (lowDmaCooldownRemainingMs() > 0) {
                break;
            }

            // Defer background STA reconnect attempts during early boot until V1 is
            // connected. This protects BLE acquisition from AP+STA mode churn.
            bool v1Connected = isV1Connected_ ? isV1Connected_(isV1ConnectedCtx_) : bleClient.isConnected();
            bool withinBootGrace = (setupModeStartTime_ != 0) &&
                                   ((millis() - setupModeStartTime_) < WIFI_RECONNECT_DEFER_NO_V1_MS);
            if (!v1Connected && withinBootGrace) {
                if (!wifiReconnectDeferredLogged_) {
                    Serial.printf("[WiFiClient] Auto-reconnect deferred (waiting for V1 or %lu ms grace)\n",
                                  (unsigned long)WIFI_RECONNECT_DEFER_NO_V1_MS);
                    wifiReconnectDeferredLogged_ = true;
                }
                break;
            }
            if (wifiReconnectDeferredLogged_) {
                Serial.println("[WiFiClient] Auto-reconnect resumed");
                wifiReconnectDeferredLogged_ = false;
            }

            // Auto-reconnect if we have saved credentials (with failure limit)
            const V1Settings& settings = settingsManager.get();
            if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
                // When WiFi was auto-started and no AP client has connected,
                // skip STA reconnect — the initial WiFi.begin() in startSetupMode()
                // already tried, and the no-client shutdown will reclaim resources.
                if (wasAutoStarted_ && cachedApStaCount_ == 0 && lastUiActivityMs_ == 0) {
                    break;
                }
                // Check if we've exceeded max failures - prevents memory exhaustion
                if (wifiReconnectFailures_ >= WIFI_MAX_RECONNECT_FAILURES) {
                    // Already gave up - don't log spam, just stay in failed state
                    break;
                }

                // Only try auto-reconnect every 30 seconds (first attempt is immediate).
                unsigned long nowMs = millis();
                if (lastReconnectAttemptMs_ == 0 || (nowMs - lastReconnectAttemptMs_) > WIFI_RECONNECT_INTERVAL_MS) {
                    String savedPassword = settingsManager.getWifiClientPassword();
                    lastReconnectAttemptMs_ = nowMs;
                    wifiReconnectFailures_++;

                    if (wifiReconnectFailures_ >= WIFI_MAX_RECONNECT_FAILURES) {
                        Serial.printf("[WiFiClient] Giving up after %d failed attempts. Use BOOT button to retry.\n",
                                      wifiReconnectFailures_);
                        // Stay in FAILED state, user must toggle WiFi to retry
                        break;
                    }

                    Serial.printf("[WiFiClient] Auto-reconnect attempt %d/%d...\n",
                                  wifiReconnectFailures_, WIFI_MAX_RECONNECT_FAILURES);
                    connectToNetwork(settings.wifiClientSSID, savedPassword, false);
                }
            }
            break;
        }

        default:
            break;
    }
}
