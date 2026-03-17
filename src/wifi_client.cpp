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
    if (wifiClientState == WIFI_CLIENT_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

String WiFiManager::getConnectedSSID() const {
    if (wifiClientState == WIFI_CLIENT_CONNECTED) {
        return WiFi.SSID();
    }
    return "";
}

bool WiFiManager::startWifiScan() {
    if (wifiScanRunning) {
        Serial.println("[WiFiClient] Scan already in progress");
        return false;
    }
    
    Serial.println("[WiFiClient] Starting async network scan...");
    WiFi.scanDelete();  // Clear previous results
    
    // Start async scan (non-blocking)
    int result = WiFi.scanNetworks(true, false, false, 300);  // async=true, show_hidden=false, passive=false, max_ms_per_chan=300
    if (result == WIFI_SCAN_RUNNING) {
        wifiScanRunning = true;
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
    
    wifiScanRunning = false;
    
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
    pendingConnectSSID = ssid;
    pendingConnectPassword = password;
    pendingConnectPersistCredentials = persistCredentialsOnSuccess;
    wifiConnectStartMs = 0;
    wifiClientState = WIFI_CLIENT_CONNECTING;
    wifiConnectPhase = WifiConnectPhase::PREPARE_OFF;
    wifiConnectPhaseStartMs = millis();
    PERF_INC(wifiConnectDeferred);
    return true;
}

bool WiFiManager::enableWifiClientFromSavedCredentials() {
    settingsManager.setWifiClientEnabled(true);

    const String savedSsid = settingsManager.get().wifiClientSSID;
    if (savedSsid.length() == 0) {
        wifiClientState = WIFI_CLIENT_DISCONNECTED;
        return true;
    }

    if (connectToNetwork(savedSsid, settingsManager.getWifiClientPassword())) {
        return true;
    }

    wifiClientState = WIFI_CLIENT_DISCONNECTED;
    return false;
}

void WiFiManager::disconnectFromNetwork() {
    Serial.println("[WiFiClient] Disconnecting from network");
    wifiConnectPhase = WifiConnectPhase::IDLE;
    wifiConnectPhaseStartMs = 0;
    wifiConnectStartMs = 0;
    WiFi.disconnect(false);  // Don't turn off station mode
    wifiClientState = WIFI_CLIENT_DISCONNECTED;
    pendingConnectSSID = "";
    pendingConnectPassword = "";
    pendingConnectPersistCredentials = true;
}

void WiFiManager::disableWifiClient() {
    disconnectFromNetwork();
    settingsManager.setWifiClientEnabled(false);
    wifiClientState = WIFI_CLIENT_DISABLED;
    WiFi.mode(WIFI_AP);
}

void WiFiManager::forgetWifiClient() {
    disconnectFromNetwork();
    settingsManager.clearWifiClientCredentials();
    wifiClientState = WIFI_CLIENT_DISABLED;
    WiFi.mode(WIFI_AP);
}

void WiFiManager::processWifiClientConnectPhase() {
    if (wifiConnectPhase == WifiConnectPhase::IDLE) {
        return;
    }

    unsigned long now = millis();
    switch (wifiConnectPhase) {
        case WifiConnectPhase::PREPARE_OFF:
            if (isSetupModeActive()) {
                // Keep AP online and use a direct STA begin path.
                // Repeated STA resets in AP+STA mode have proven brittle on some routers.
                Serial.println("[WiFiClient] Preserving AP, preparing STA connect...");
                if (WiFi.getMode() != WIFI_AP_STA) {
                    WiFi.mode(WIFI_AP_STA);
                    wifiConnectPhaseStartMs = now;
                    wifiConnectPhase = WifiConnectPhase::WAIT_AP_STA;
                } else {
                    wifiConnectPhase = WifiConnectPhase::BEGIN_CONNECT;
                }
            } else {
                if (WiFi.getMode() != WIFI_OFF) {
                    Serial.println("[WiFiClient] Cleaning up WiFi before reconnect...");
                    WiFi.disconnect(false, false);  // Graceful release without credential erase
                    WiFi.mode(WIFI_OFF);          // Fully shut down WiFi driver
                }
                wifiConnectPhaseStartMs = now;
                wifiConnectPhase = WifiConnectPhase::WAIT_OFF;
            }
            break;

        case WifiConnectPhase::WAIT_OFF:
            if (now - wifiConnectPhaseStartMs >= WIFI_MODE_SWITCH_SETTLE_MS) {
                wifiConnectPhase = WifiConnectPhase::ENABLE_AP_STA;
            }
            break;

        case WifiConnectPhase::ENABLE_AP_STA:
            Serial.println("[WiFiClient] Initializing WiFi in AP+STA mode");
            WiFi.mode(WIFI_AP_STA);
            wifiConnectPhaseStartMs = now;
            wifiConnectPhase = WifiConnectPhase::WAIT_AP_STA;
            break;

        case WifiConnectPhase::WAIT_AP_STA:
            if (now - wifiConnectPhaseStartMs >= WIFI_MODE_SWITCH_SETTLE_MS) {
                wifiConnectPhase = WifiConnectPhase::BEGIN_CONNECT;
            }
            break;

        case WifiConnectPhase::BEGIN_CONNECT:
            if (pendingConnectSSID.length() == 0) {
                wifiConnectPhase = WifiConnectPhase::IDLE;
                wifiClientState = WIFI_CLIENT_FAILED;
                break;
            }
            // Improve coexistence stability while connecting alongside BLE links.
            WiFi.setSleep(false);
            WiFi.setAutoReconnect(true);
            Serial.printf("[WiFiClient] Connecting to: %s\n", pendingConnectSSID.c_str());
            WiFi.begin(pendingConnectSSID.c_str(), pendingConnectPassword.c_str());
            wifiConnectStartMs = now;
            wifiConnectPhase = WifiConnectPhase::IDLE;
            break;

        case WifiConnectPhase::IDLE:
        default:
            break;
    }
}

void WiFiManager::checkWifiClientStatus() {
    // Skip if WiFi client is disabled
    if (wifiClientState == WIFI_CLIENT_DISABLED) {
        return;
    }
    
    wl_status_t status = WiFi.status();
    
    switch (wifiClientState) {
        case WIFI_CLIENT_CONNECTING: {
            // Non-blocking mode transition is still in progress.
            if (wifiConnectPhase != WifiConnectPhase::IDLE || wifiConnectStartMs == 0) {
                break;
            }

            if (status == WL_CONNECTED) {
                wifiClientState = WIFI_CLIENT_CONNECTED;
                wifiConnectStartMs = 0;
                Serial.printf("[WiFiClient] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
                if (isSetupModeActive()) {
                    // Arm AP idle timer from STA connect so setup UI clients have
                    // a full grace window before AP retirement.
                    lastClientSeenMs = millis();
                    Serial.println("[WiFiClient] STA connected; AP idle-retire timer armed");
                }
                
                // Reset failure counter on successful connection
                wifiReconnectFailures = 0;
                
                // Save credentials on successful connection
                if (pendingConnectSSID.length() > 0) {
                    if (pendingConnectPersistCredentials) {
                        const V1Settings& currentSettings = settingsManager.get();
                        const bool ssidChanged = (pendingConnectSSID != currentSettings.wifiClientSSID);
                        const bool passwordChanged =
                            (pendingConnectPassword != settingsManager.getWifiClientPassword());
                        if (ssidChanged || passwordChanged) {
                            settingsManager.setWifiClientCredentials(pendingConnectSSID, pendingConnectPassword);
                        } else {
                            Serial.println("[WiFiClient] Connected with unchanged credentials; skipping re-save");
                        }
                    } else {
                        Serial.println("[WiFiClient] Connected via auto-reconnect; skipping credential re-save");
                    }
                    pendingConnectSSID = "";
                    pendingConnectPassword = "";
                    pendingConnectPersistCredentials = true;
                }
            } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
                wifiClientState = WIFI_CLIENT_FAILED;
                Serial.printf("[WiFiClient] Connection failed: %d\n", status);
                wifiConnectStartMs = 0;
                
                pendingConnectSSID = "";
                pendingConnectPassword = "";
                pendingConnectPersistCredentials = true;
            } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
                wifiClientState = WIFI_CLIENT_FAILED;
                Serial.println("[WiFiClient] Connection timeout");
                WiFi.disconnect(false);
                wifiConnectStartMs = 0;
                
                pendingConnectSSID = "";
                pendingConnectPassword = "";
                pendingConnectPersistCredentials = true;
            }
            break;
        }
        
        case WIFI_CLIENT_CONNECTED: {
            if (status != WL_CONNECTED) {
                wifiClientState = WIFI_CLIENT_DISCONNECTED;
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
            bool v1Connected = isV1Connected ? isV1Connected() : bleClient.isConnected();
            bool withinBootGrace = (setupModeStartTime != 0) &&
                                   ((millis() - setupModeStartTime) < WIFI_RECONNECT_DEFER_NO_V1_MS);
            if (!v1Connected && withinBootGrace) {
                if (!wifiReconnectDeferredLogged) {
                    Serial.printf("[WiFiClient] Auto-reconnect deferred (waiting for V1 or %lu ms grace)\n",
                                  (unsigned long)WIFI_RECONNECT_DEFER_NO_V1_MS);
                    wifiReconnectDeferredLogged = true;
                }
                break;
            }
            if (wifiReconnectDeferredLogged) {
                Serial.println("[WiFiClient] Auto-reconnect resumed");
                wifiReconnectDeferredLogged = false;
            }

            // Auto-reconnect if we have saved credentials (with failure limit)
            const V1Settings& settings = settingsManager.get();
            if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
                // When WiFi was auto-started and no AP client has connected,
                // skip STA reconnect — the initial WiFi.begin() in startSetupMode()
                // already tried, and the no-client shutdown will reclaim resources.
                if (wasAutoStarted && cachedApStaCount == 0 && lastUiActivityMs == 0) {
                    break;
                }
                // Check if we've exceeded max failures - prevents memory exhaustion
                if (wifiReconnectFailures >= WIFI_MAX_RECONNECT_FAILURES) {
                    // Already gave up - don't log spam, just stay in failed state
                    break;
                }
                
                // Only try auto-reconnect every 30 seconds (first attempt is immediate).
                unsigned long nowMs = millis();
                if (lastReconnectAttemptMs == 0 || (nowMs - lastReconnectAttemptMs) > WIFI_RECONNECT_INTERVAL_MS) {
                    String savedPassword = settingsManager.getWifiClientPassword();
                    lastReconnectAttemptMs = nowMs;
                    wifiReconnectFailures++;
                    
                    if (wifiReconnectFailures >= WIFI_MAX_RECONNECT_FAILURES) {
                        Serial.printf("[WiFiClient] Giving up after %d failed attempts. Use BOOT button to retry.\n",
                                      wifiReconnectFailures);
                        // Stay in FAILED state, user must toggle WiFi to retry
                        break;
                    }
                    
                    Serial.printf("[WiFiClient] Auto-reconnect attempt %d/%d...\n",
                                  wifiReconnectFailures, WIFI_MAX_RECONNECT_FAILURES);
                    connectToNetwork(settings.wifiClientSSID, savedPassword, false);
                }
            }
            break;
        }
        
        default:
            break;
    }
}
