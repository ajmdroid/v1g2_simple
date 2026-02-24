/**
 * WiFi Client — STA connection, scan, reconnect, push-now state machine.
 * Extracted from wifi_manager.cpp for maintainability.
 */

#include "wifi_manager_internals.h"
#include "perf_metrics.h"
#include "settings.h"
#include "settings_sanitize.h"
#include "v1_profiles.h"
#include "display.h"
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

    // WiFi transitioning - defer SD writes to avoid NVS flash contention
    debugLogger.notifyWifiTransition(false);

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
                    WiFi.disconnect(true, true);  // Disconnect and erase credentials from RAM
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
                debugLogger.notifyWifiTransition(true);
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

void WiFiManager::processPendingPushNow() {
    if (pushNowState.step == PushNowStep::IDLE) {
        return;
    }

    if (!bleClient.isConnected()) {
        Serial.println("[PushNow] Aborted: V1 disconnected");
        pushNowState.step = PushNowStep::IDLE;
        return;
    }

    unsigned long now = millis();
    if (now < pushNowState.nextAtMs) {
        return;
    }

    auto scheduleRetry = [&](const char* op) {
        if (pushNowState.retries < PUSH_NOW_MAX_RETRIES) {
            pushNowState.retries++;
            PERF_INC(pushNowRetries);
            pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
            Serial.printf("[PushNow] %s deferred, retry %u/%u\n",
                          op,
                          static_cast<unsigned int>(pushNowState.retries),
                          static_cast<unsigned int>(PUSH_NOW_MAX_RETRIES));
            return;
        }
        Serial.printf("[PushNow] ERROR: %s failed after %u retries\n",
                      op,
                      static_cast<unsigned int>(PUSH_NOW_MAX_RETRIES));
        PERF_INC(pushNowFailures);
        pushNowState.step = PushNowStep::IDLE;
    };

    switch (pushNowState.step) {
        case PushNowStep::WRITE_PROFILE:
            if (bleClient.writeUserBytes(pushNowState.profileBytes)) {
                bleClient.startUserBytesVerification(pushNowState.profileBytes);
                bleClient.requestUserBytes();
                pushNowState.step = PushNowStep::SET_DISPLAY;
                pushNowState.retries = 0;
                pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
                return;
            }
            scheduleRetry("writeUserBytes");
            return;

        case PushNowStep::SET_DISPLAY:
            if (bleClient.setDisplayOn(pushNowState.displayOn)) {
                pushNowState.step = PushNowStep::SET_MODE;
                pushNowState.retries = 0;
                pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
                return;
            }
            scheduleRetry("setDisplayOn");
            return;

        case PushNowStep::SET_MODE:
            if (!pushNowState.applyMode || bleClient.setMode(static_cast<uint8_t>(pushNowState.mode))) {
                pushNowState.step = PushNowStep::SET_VOLUME;
                pushNowState.retries = 0;
                pushNowState.nextAtMs = now + PUSH_NOW_RETRY_DELAY_MS;
                return;
            }
            scheduleRetry("setMode");
            return;

        case PushNowStep::SET_VOLUME:
            if (!pushNowState.applyVolume || bleClient.setVolume(pushNowState.mainVol, pushNowState.muteVol)) {
                Serial.println("[PushNow] Complete");
                pushNowState.step = PushNowStep::IDLE;
                return;
            }
            scheduleRetry("setVolume");
            return;

        case PushNowStep::IDLE:
        default:
            return;
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
                
                // WiFi stable - resume SD writes (NVS contention window closed)
                debugLogger.notifyWifiTransition(true);
                
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
                
                // WiFi stable (failed but not transitioning) - resume SD writes
                debugLogger.notifyWifiTransition(true);
                
                pendingConnectSSID = "";
                pendingConnectPassword = "";
                pendingConnectPersistCredentials = true;
            } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
                wifiClientState = WIFI_CLIENT_FAILED;
                Serial.println("[WiFiClient] Connection timeout");
                WiFi.disconnect(false);
                wifiConnectStartMs = 0;
                
                // WiFi stable (timed out) - resume SD writes
                debugLogger.notifyWifiTransition(true);
                
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
                
                // WiFi transitioning - defer SD writes to avoid NVS flash contention
                debugLogger.notifyWifiTransition(false);
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
