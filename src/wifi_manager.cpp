/**
 * WiFi Manager for V1 Gen2 Display
 * Handles WiFi AP/STA modes and web server
 */

#include "wifi_manager.h"
#include "settings.h"
#include "display.h"
#include "alert_logger.h"
#include "alert_db.h"
#include "serial_logger.h"
#include "v1_profiles.h"
#include "ble_client.h"
#include "time_manager.h"
#include "../include/config.h"
#include "../include/color_themes.h"
#include <algorithm>
#include <ArduinoJson.h>
#include "esp_netif.h"
#include <ESPmDNS.h>

// External BLE client for V1 commands
extern V1BLEClient bleClient;

// HTML entity encoding for safe HTML generation
static String htmlEscape(const String& input) {
    String output;
    output.reserve(input.length() + 20);  // Reserve extra space for escape sequences
    for (size_t i = 0; i < input.length(); i++) {
        char c = input.charAt(i);
        switch (c) {
            case '&':  output += "&amp;";  break;
            case '<':  output += "&lt;";   break;
            case '>':  output += "&gt;";   break;
            case '"':  output += "&quot;"; break;
            case '\'': output += "&#39;";  break;
            default:   output += c;        break;
        }
    }
    return output;
}

// CSV split helper for fixed column count
namespace {
bool splitCsvFixed(const String& line, String parts[], size_t expected) {
    int start = 0;
    size_t idx = 0;
    while (idx < expected) {
        int sep = line.indexOf(',', start);
        if (sep == -1) {
            parts[idx++] = line.substring(start);
            break;
        }
        parts[idx++] = line.substring(start, sep);
        start = sep + 1;
    }
    return idx == expected;
}
} // namespace

// Global instance
WiFiManager wifiManager;

WiFiManager::WiFiManager() : server(80), apActive(false), staConnected(false), staEnabledByConfig(false), natEnabled(false), lastStaRetry(0), timeInitialized(false) {
}

bool WiFiManager::begin() {
    const V1Settings& settings = settingsManager.get();
    
    if (!settings.enableWifi) {
        SerialLog.println("WiFi disabled in settings");
        return false;
    }
    
    SerialLog.println("Starting WiFi...");

    bool wantAP = settings.wifiMode == V1_WIFI_AP || settings.wifiMode == V1_WIFI_APSTA;
    bool wantSTA = settings.wifiMode == V1_WIFI_STA || settings.wifiMode == V1_WIFI_APSTA;
    staEnabledByConfig = wantSTA;  // Track if STA is enabled by config

    // If networks are configured but STA is disabled, auto-enable STA alongside AP
    int networksConfigured = 0;
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (settings.wifiNetworks[i].isValid()) networksConfigured++;
    }
    if (networksConfigured == 0 && settings.staSSID.length() > 0) networksConfigured++;
    if (!wantSTA && networksConfigured > 0) {
        SerialLog.println("[WiFi] STA disabled but networks configured; enabling AP+STA for connectivity");
        wantSTA = true;
        wantAP = true;
        staEnabledByConfig = true;
    }

    if (wantAP && wantSTA) {
        WiFi.mode(WIFI_AP_STA);
    } else if (wantAP) {
        WiFi.mode(WIFI_AP);
    } else {
        WiFi.mode(WIFI_STA);
    }

    if (wantAP) {
        setupAP();
    }
    
    if (wantSTA) {
        setupSTA();
    } else {
        SerialLog.println("[WiFi] STA mode disabled in settings");
    }
    
    setupWebServer();
    server.begin();
    SerialLog.println("Web server started on port 80");
    
    return true;
}

void WiFiManager::setupAP() {
    const V1Settings& settings = settingsManager.get();
    bool wantSTA = settings.wifiMode == V1_WIFI_APSTA;
    
    String apSSID = settings.apSSID.length() > 0 ? settings.apSSID : "V1-Display";
    String apPass = settings.apPassword.length() >= 8 ? settings.apPassword : "valentine1";
    
    SerialLog.printf("Starting AP: %s\n", apSSID.c_str());
    
    // Ensure correct mode for AP operation
    WiFi.mode(wantSTA ? WIFI_AP_STA : WIFI_AP);
    
    // Configure AP IP BEFORE starting softAP
    IPAddress apIP(192, 168, 35, 5);      // AP IP address for web UI access
    IPAddress gateway(192, 168, 35, 5);   // Gateway = AP IP so clients route through us
    IPAddress subnet(255, 255, 255, 0);
    
    // Configure IP address first (must be before softAP)
    // Note: softAPConfig args are (local_ip, gateway, subnet, dhcp_lease_start, dns_server)
    // We must provide dhcp_lease_start if we want to provide dns_server
    IPAddress dhcpStart(192, 168, 35, 100);
    
    // Use the upstream DNS if available, otherwise fallback to Google
    IPAddress dns = WiFi.dnsIP(0);
    if (dns == IPAddress(0,0,0,0)) dns = IPAddress(8,8,8,8);
    
    if (!WiFi.softAPConfig(apIP, gateway, subnet, dhcpStart, dns)) {
        SerialLog.println("[WiFi] softAPConfig failed!");
    }
    
    // Now start the AP
    if (!WiFi.softAP(apSSID.c_str(), apPass.c_str())) {
        SerialLog.println("[WiFi] softAP failed!");
    }
    
    apActive = true;
    
    IPAddress ip = WiFi.softAPIP();
    SerialLog.printf("AP IP address: %s\n", ip.toString().c_str());
    SerialLog.printf("AP Gateway: %s\n", gateway.toString().c_str());
    SerialLog.printf("AP Subnet: %s\n", subnet.toString().c_str());
    
    // Ensure DHCP server is properly configured for AP
    // Get AP network interface and verify DHCP
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        // Note: WiFi.softAPConfig already configured DHCP with DNS
        // We just verify it's running
        esp_netif_dhcp_status_t status;
        esp_netif_dhcps_get_status(ap_netif, &status);
        if (status == ESP_NETIF_DHCP_STOPPED) {
             esp_netif_dhcps_start(ap_netif);
             SerialLog.println("[WiFi] AP DHCP server started (was stopped)");
        } else {
             SerialLog.println("[WiFi] AP DHCP server is running");
        }
    } else {
        SerialLog.println("[WiFi] WARNING: Could not get AP netif for DHCP");
    }
}

void WiFiManager::setupSTA() {
    SerialLog.println("[WiFi] Setting up STA mode...");
    int networksAdded = populateStaNetworks();
    if (networksAdded == 0) {
        SerialLog.println("[WiFi] No networks configured for STA");
        return;
    }
    SerialLog.printf("[WiFi] Added %d network(s) to WiFiMulti\n", networksAdded);
}

int WiFiManager::populateStaNetworks() {
    wifiMulti = WiFiMulti();  // Reset network list before re-populating
    const V1Settings& settings = settingsManager.get();
    int networksAdded = 0;

    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        if (settings.wifiNetworks[i].isValid()) {
            wifiMulti.addAP(settings.wifiNetworks[i].ssid.c_str(), 
                          settings.wifiNetworks[i].password.c_str());
            SerialLog.printf("[WiFi] Added network %d: %s\n", i, settings.wifiNetworks[i].ssid.c_str());
            networksAdded++;
        }
    }

    if (networksAdded == 0 && settings.staSSID.length() > 0) {
        wifiMulti.addAP(settings.staSSID.c_str(), settings.staPassword.c_str());
        SerialLog.printf("[WiFi] Added legacy network: %s\n", settings.staSSID.c_str());
        networksAdded++;
    }

    return networksAdded;
}

void WiFiManager::checkSTAConnection() {
    
    if (!staEnabledByConfig) {
        if (staConnected) {
            staConnected = false;
            natEnabled = false;
            connectedSSID = "";
        }
        return;
    }
    
    // Try connecting every STA_CONNECTION_RETRY_INTERVAL_MS
    if (millis() - lastStaRetry < STA_CONNECTION_RETRY_INTERVAL_MS) {
        return;
    }
    lastStaRetry = millis();

    if (wifiMulti.run(STA_CONNECTION_TIMEOUT_MS) == WL_CONNECTED) {
        if (!staConnected) {
            staConnected = true;
            connectedSSID = WiFi.SSID();
            SerialLog.println("\n=== WiFi Connected ===");
            SerialLog.printf("SSID: %s\n", connectedSSID.c_str());
            SerialLog.printf("IP: %s\n", WiFi.localIP().toString().c_str());
            SerialLog.printf("Signal: %d dBm\n", WiFi.RSSI());
            
            if (apActive) {
                enableNAT();
            }
            
            if (!timeInitialized) {
                initializeTime();
            }
        }
    } else {
        if (staConnected) {
            SerialLog.println("[WiFi] Disconnected");
            staConnected = false;
            connectedSSID = "";
        }
    }
}

void WiFiManager::initializeTime() {
    SerialLog.println("Initializing NTP time sync...");
    // Configure NTP with UTC timezone (0 offset, 0 DST)
    // All times stored and displayed in UTC
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    
    // Wait briefly for time to sync
    struct tm timeinfo;
    int retries = 0;
    while (!getLocalTime(&timeinfo) && retries < NTP_SYNC_RETRY_COUNT) {
        delay(NTP_SYNC_RETRY_DELAY_MS);
        retries++;
    }
    
    if (retries < NTP_SYNC_RETRY_COUNT) {
        SerialLog.println("Time synchronized via NTP!");
        SerialLog.printf("  Current time (UTC): %04d-%02d-%02dT%02d:%02d:%02dZ\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        timeInitialized = true;
        
        // Update alert loggers with real timestamp
        time_t now = time(nullptr);
        alertLogger.setTimestampUTC((uint32_t)now);
        alertDB.setTimestampUTC((uint32_t)now);
        SerialLog.printf("[WiFi] Alert timestamps set: %lu\n", (uint32_t)now);
    } else {
        SerialLog.println("Failed to sync time via NTP");
    }
}

void WiFiManager::enableNAT() {
    // NAT setup: Forward traffic from AP clients through STA connection
    
    if (natEnabled) {
        return;  // Already enabled
    }
    
    SerialLog.println("[WiFi] Enabling NAT/NAPT...");

    // 1. Enable NAPT on AP interface (The LAN interface)
    // This tells LwIP to perform NAT on packets arriving on this interface
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif) {
        esp_err_t err = esp_netif_napt_enable(ap_netif);
        if (err == ESP_OK) {
            SerialLog.println("[WiFi] NAPT enabled on AP interface (LAN)");
        } else {
            SerialLog.printf("[WiFi] Failed to enable NAPT on AP: %d\n", err);
        }
    }

    // 2. Ensure IP Forwarding is enabled (should be by build flag)
    // The build flag CONFIG_LWIP_IP_FORWARD=1 handles this.
    
    natEnabled = true;
    SerialLog.printf("[WiFi] NAT: Enabled - STA IP: %s, AP IP: %s\n", 
                  WiFi.localIP().toString().c_str(), WiFi.softAPIP().toString().c_str());
    SerialLog.println("[WiFi] NAT: Clients should now have internet access");
}

void WiFiManager::setupWebServer() {
    server.on("/", HTTP_GET, [this]() { handleSettings(); });  // Root redirects to settings
    server.on("/status", HTTP_GET, [this]() { handleStatus(); });
    server.on("/settings", HTTP_GET, [this]() { handleSettings(); });
    server.on("/settings", HTTP_POST, [this]() { handleSettingsSave(); });
    server.on("/time", HTTP_GET, [this]() { handleTimeSettings(); });
    server.on("/time", HTTP_POST, [this]() { handleTimeSettingsSave(); });
    server.on("/darkmode", HTTP_POST, [this]() { handleDarkMode(); });
    server.on("/mute", HTTP_POST, [this]() { handleMute(); });
    server.on("/logs", HTTP_GET, [this]() { handleLogs(); });
    server.on("/api/logs", HTTP_GET, [this]() { handleLogsData(); });
    server.on("/api/logs/clear", HTTP_POST, [this]() { handleLogsClear(); });
    
    // Serial log endpoints for debugging
    server.on("/seriallog", HTTP_GET, [this]() { handleSerialLogPage(); });
    server.on("/serial_log.txt", HTTP_GET, [this]() { handleSerialLog(); });
    server.on("/api/serial_log/clear", HTTP_POST, [this]() { handleSerialLogClear(); });

    // Lightweight health and captive-portal helpers
    server.on("/ping", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /ping");
        server.send(200, "text/plain", "OK");
    });
    // Android/ChromeOS captive portal probes
    server.on("/generate_204", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /generate_204");
        server.send(204, "text/plain", "");
    });
    server.on("/gen_204", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /gen_204");
        server.send(204, "text/plain", "");
    });
    // iOS/macOS captive portal
    server.on("/hotspot-detect.html", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /hotspot-detect.html");
        server.sendHeader("Location", "/settings", true);
        server.send(302, "text/html", "");
    });
    // Windows captive portal variants
    server.on("/fwlink", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /fwlink");
        server.sendHeader("Location", "/settings", true);
        server.send(302, "text/html", "");
    });
    server.on("/ncsi.txt", HTTP_GET, [this]() {
        SerialLog.println("[HTTP] GET /ncsi.txt");
        server.send(200, "text/plain", "Microsoft NCSI");
    });
    
    // V1 Settings/Profiles routes
    server.on("/v1settings", HTTP_GET, [this]() { handleV1Settings(); });
    server.on("/api/v1/profiles", HTTP_GET, [this]() { handleV1ProfilesList(); });
    server.on("/api/v1/profile", HTTP_GET, [this]() { handleV1ProfileGet(); });
    server.on("/api/v1/profile", HTTP_POST, [this]() { handleV1ProfileSave(); });
    server.on("/api/v1/profile/delete", HTTP_POST, [this]() { handleV1ProfileDelete(); });
    server.on("/api/v1/pull", HTTP_POST, [this]() { handleV1SettingsPull(); });
    server.on("/api/v1/push", HTTP_POST, [this]() { handleV1SettingsPush(); });
    server.on("/api/v1/current", HTTP_GET, [this]() { handleV1CurrentSettings(); });
    
    // Auto-Push routes
    server.on("/autopush", HTTP_GET, [this]() { handleAutoPush(); });
    server.on("/api/autopush/slot", HTTP_POST, [this]() { handleAutoPushSlotSave(); });
    server.on("/api/autopush/activate", HTTP_POST, [this]() { handleAutoPushActivate(); });
    server.on("/api/autopush/push", HTTP_POST, [this]() { handleAutoPushPushNow(); });
    
    // Display Colors routes
    server.on("/displaycolors", HTTP_GET, [this]() { handleDisplayColors(); });
    server.on("/api/displaycolors", HTTP_POST, [this]() { handleDisplayColorsSave(); });
    server.on("/api/displaycolors/reset", HTTP_POST, [this]() { handleDisplayColorsReset(); });
    
    server.onNotFound([this]() { handleNotFound(); });
}

void WiFiManager::process() {
    server.handleClient();
    
    if (staEnabledByConfig) {
        checkSTAConnection();
    }
}

void WiFiManager::stop() {
    server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    apActive = false;
    staConnected = false;
    SerialLog.println("WiFi stopped");
}

bool WiFiManager::isConnected() const {
    return staConnected;
}

bool WiFiManager::isAPActive() const {
    return apActive;
}

String WiFiManager::getIPAddress() const {
    if (staConnected) {
        return WiFi.localIP().toString();
    }
    return "";
}

String WiFiManager::getAPIPAddress() const {
    if (apActive) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

void WiFiManager::handleStatus() {
    String json = "{";
    json += "\"connected\":" + String(staConnected ? "true" : "false") + ",";
    json += "\"ap_active\":" + String(apActive ? "true" : "false") + ",";
    json += "\"ip\":\"" + getIPAddress() + "\",";
    json += "\"ap_ip\":\"" + getAPIPAddress() + "\"";
    
    if (getStatusJson) {
        json += "," + getStatusJson();
    }
    
    json += "}";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettings() {
    SerialLog.println("[HTTP] GET /settings");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Stream HTML in chunks
    streamLayoutHeader("V1 Display Settings", "/settings");
    streamSettingsBody();
    streamLayoutFooter();
}

void WiFiManager::handleSettingsSave() {
    SerialLog.println("=== handleSettingsSave() called ===");
    
    // Track if WiFi settings changed (these require restart)
    bool wifiChanged = false;
    const V1Settings& currentSettings = settingsManager.get();

    if (server.hasArg("ssid")) {
        String ssid = server.arg("ssid");
        String pass = server.arg("password");
        
        // Preserve existing password if placeholder sent
        if (pass == "********") {
            pass = currentSettings.password;
        }
        if (ssid != currentSettings.ssid || pass != currentSettings.password) {
            wifiChanged = true;
        }
        settingsManager.updatePrimaryWiFi(ssid, pass);
    }
    
    if (server.hasArg("ap_ssid")) {
        String apSsid = server.arg("ap_ssid");
        String apPass = server.arg("ap_password");
        
        // If password is placeholder, keep existing password
        if (apPass == "********") {
            apPass = currentSettings.apPassword;
        }
        
        if (apSsid.length() == 0 || apPass.length() < 8) {
            server.send(400, "text/plain", "AP SSID required and password must be at least 8 characters");
            return;
        }
        if (apSsid != currentSettings.apSSID || apPass != currentSettings.apPassword) {
            wifiChanged = true;
        }
        settingsManager.updateAPCredentials(apSsid, apPass);
    }
    
    // WiFi mode (AP, STA, AP+STA)
    WiFiModeSetting mode = V1_WIFI_AP;
    if (server.hasArg("wifi_mode")) {
        mode = static_cast<WiFiModeSetting>(server.arg("wifi_mode").toInt());
        mode = static_cast<WiFiModeSetting>(std::max(0, std::min(3, (int)mode)));
    }
    if (mode != currentSettings.wifiMode) {
        wifiChanged = true;
    }
    settingsManager.updateWiFiMode(mode);
    
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        settingsManager.updateBrightness((uint8_t)brightness);
    }
    
    if (server.hasArg("color_theme")) {
        int theme = server.arg("color_theme").toInt();
        theme = std::max(0, std::min(theme, 2));  // Clamp to valid theme range
        settingsManager.updateColorTheme(static_cast<ColorTheme>(theme));
    }
    
    // All changes are queued in the settingsManager instance. Now, save them all at once.
    SerialLog.println("--- Calling settingsManager.save() ---");
    settingsManager.save();
    
    // The settingsManager instance is already up-to-date, no need to reload.
    // We can directly apply any changes that need to take immediate effect.
    if (server.hasArg("color_theme")) {
        display.updateColorTheme();
        SerialLog.println("Display color theme updated");
    }
    
    // Redirect with appropriate message
    String redirect = "/settings?saved=1";
    if (wifiChanged) {
        redirect += "&wifi=1";
    }
    server.sendHeader("Location", redirect);
    server.send(302);
}

void WiFiManager::handleDarkMode() {
    if (!server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"Missing state parameter\"}");
        return;
    }
    
    bool darkMode = server.arg("state") == "1" || server.arg("state") == "true";
    bool success = false;
    
    if (sendV1Command) {
        // Dark mode = display OFF, so invert the parameter
        success = sendV1Command("display", !darkMode);
    }
    
    SerialLog.printf("Dark mode request: %s, success: %s\n", darkMode ? "ON" : "OFF", success ? "yes" : "no");
    
    String json = "{\"success\":" + String(success ? "true" : "false") + 
                  ",\"darkMode\":" + String(darkMode ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleMute() {
    if (!server.hasArg("state")) {
        server.send(400, "application/json", "{\"error\":\"Missing state parameter\"}");
        return;
    }
    
    bool muted = server.arg("state") == "1" || server.arg("state") == "true";
    bool success = false;
    
    if (sendV1Command) {
        success = sendV1Command("mute", muted);
    }
    
    SerialLog.printf("Mute request: %s, success: %s\n", muted ? "ON" : "OFF", success ? "yes" : "no");
    
    String json = "{\"success\":" + String(success ? "true" : "false") + 
                  ",\"muted\":" + String(muted ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleTimeSettings() {
    SerialLog.println("[HTTP] GET /time");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Stream HTML in chunks
    streamLayoutHeader("Time Settings", "/time");
    streamTimeSettingsBody();
    streamLayoutFooter();
}

void WiFiManager::handleTimeSettingsSave() {
    bool changed = false;
    const V1Settings& settings = settingsManager.get();
    
    // Update timesync enabled flag
    bool enableTime = server.hasArg("enableTimesync");
    if (settings.enableTimesync != enableTime) {
        settingsManager.updateTimesync(enableTime);
        changed = true;
    }
    
    // Update multiple WiFi networks
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
        String ssidKey = "wifi" + String(i) + "ssid";
        String pwdKey = "wifi" + String(i) + "pwd";
        
        if (server.hasArg(ssidKey.c_str())) {
            String newSSID = server.arg(ssidKey.c_str());
            String newPwd = server.arg(pwdKey.c_str());
            
            // If password is placeholder, keep existing password  
            if (newPwd == "********") {
                newPwd = settings.wifiNetworks[i].password;
            }
            
            if (settings.wifiNetworks[i].ssid != newSSID || settings.wifiNetworks[i].password != newPwd) {
                settingsManager.updateWiFiNetwork(i, newSSID, newPwd);
                changed = true;
                SerialLog.printf("WiFi network %d updated: %s\n", i+1, newSSID.c_str());
            }
        }
    }
    
    // Keep legacy fields in sync with network[0] for backward compat
    if (settingsManager.get().wifiNetworks[0].isValid()) {
        settingsManager.updateStaSSID(
            settingsManager.get().wifiNetworks[0].ssid, 
            settingsManager.get().wifiNetworks[0].password
        );
    }
    
    // Manual time setting
    if (server.hasArg("timestamp")) {
        time_t timestamp = (time_t)server.arg("timestamp").toInt();
        if (timestamp > 1609459200) {  // Valid if after 2021-01-01
            timeManager.setTime(timestamp);
            SerialLog.printf("Time set manually to: %ld\n", timestamp);
        }
    }
    
    if (changed) {
        settingsManager.save();
        SerialLog.printf("WiFi settings saved. Timesync: %s\n", enableTime ? "enabled" : "disabled");
        
        int networksAdded = populateStaNetworks();
        if (networksAdded > 0) {
            if (!staEnabledByConfig) {
                staEnabledByConfig = true;
                SerialLog.println("[WiFi] STA mode auto-enabled after configuring networks");
            }
            SerialLog.printf("[WiFi] STA network list refreshed (%d entries)\n", networksAdded);
            if (staConnected) {
                WiFi.disconnect(true);
                staConnected = false;
                connectedSSID = "";
                natEnabled = false;
            }
            lastStaRetry = 0;  // Trigger immediate retry with new credentials
        }
    }
    
    String response = changed ? 
        "Settings saved! <a href='/time'>Back to Time Settings</a>" :
        "No changes made. <a href='/time'>Back to Time Settings</a>";
    server.send(200, "text/html", response);
}

void WiFiManager::handleLogs() {
    SerialLog.println("[HTTP] GET /logs");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Stream HTML in chunks
    streamLayoutHeader("Alert Logs", "/logs");
    streamLogsBody();
    streamLayoutFooter();
}

void WiFiManager::handleLogsData() {
    if (!alertLogger.isReady()) {
        server.send(503, "application/json", "{\"error\":\"SD card not mounted\"}");
        return;
    }

    // Optional: support /api/logs?n=50
    size_t maxLines = ALERT_LOG_MAX_RECENT;
    if (server.hasArg("n")) {
        int n = server.arg("n").toInt();
        if (n > 0) {
            if (n > 500) n = 500; // hard cap to protect RAM/CPU
            maxLines = (size_t)n;
        }
    }

    fs::FS* fs = alertLogger.getFilesystem();
    if (!fs) {
        server.send(500, "application/json", "{\"error\":\"filesystem unavailable\"}");
        return;
    }

    File file = fs->open(ALERT_LOG_PATH, FILE_READ);
    if (!file) {
        server.send(500, "application/json", "{\"error\":\"failed to open log\"}");
        return;
    }

    // Pass 1: count valid data lines
    size_t total = 0;
    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("ms,")) continue; // skip header/blank
        total++;
    }
    file.close();

    size_t startIndex = 0;
    if (total > maxLines) startIndex = total - maxLines;

    // Pass 2: stream JSON array of last N lines
    file = fs->open(ALERT_LOG_PATH, FILE_READ);
    if (!file) {
        server.send(500, "application/json", "{\"error\":\"failed to open log\"}");
        return;
    }

    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");
    server.sendContent("[");

    bool first = true;
    size_t idx = 0;

    while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() == 0 || line.startsWith("ms,")) continue;

        if (idx++ < startIndex) continue;

        // CSV columns expected (9):
        // 0=ms,1=event,2=band,3=freq,4=dir,5=front,6=rear,7=count,8=muted
        String cols[9];
        if (!splitCsvFixed(line, cols, 9)) continue;

        if (!first) server.sendContent(",");
        first = false;

        // Stream one object per row.
        // Emit BOTH new keys (ts, dir) and legacy keys (ms, direction).
        // Handle empty numeric fields by defaulting to 0
        String obj;
        obj.reserve(220);
        obj += "{\"ts\":";
        obj += cols[0].length() > 0 ? cols[0] : "0";
        obj += ",\"ms\":";
        obj += cols[0].length() > 0 ? cols[0] : "0";
        obj += ",\"utc\":0";
        obj += ",\"event\":\"";
        obj += cols[1];
        obj += "\",\"band\":\"";
        obj += cols[2];
        obj += "\",\"freq\":";
        obj += cols[3].length() > 0 ? cols[3] : "0";
        obj += ",\"dir\":\"";
        obj += cols[4];
        obj += "\",\"direction\":\"";
        obj += cols[4];
        obj += "\",\"front\":";
        obj += cols[5].length() > 0 ? cols[5] : "0";
        obj += ",\"rear\":";
        obj += cols[6].length() > 0 ? cols[6] : "0";
        obj += ",\"count\":";
        obj += cols[7].length() > 0 ? cols[7] : "0";
        obj += ",\"muted\":";
        obj += ((cols[8] == "1" || cols[8] == "true") ? "true" : "false");
        obj += "}";

        server.sendContent(obj);
    }

    file.close();
    server.sendContent("]");
}

void WiFiManager::handleLogsClear() {
    if (!alertLogger.isReady()) {
        server.send(503, "application/json", "{\"error\":\"SD card not mounted\"}");
        return;
    }

    bool cleared = alertLogger.clear();
    server.send(cleared ? 200 : 500,
                "application/json",
                String("{\"success\":") + (cleared ? "true" : "false") + "}");
}

void WiFiManager::handleSerialLog() {
    if (!alertLogger.isReady()) {
        server.send(503, "text/plain", "SD card not mounted");
        return;
    }
    
    fs::FS* fs = alertLogger.getFilesystem();
    if (!fs->exists("/serial_log.txt")) {
        server.send(404, "text/plain", "No serial log file found");
        return;
    }
    
    File file = fs->open("/serial_log.txt", FILE_READ);
    if (!file) {
        server.send(500, "text/plain", "Failed to open serial log");
        return;
    }
    
    server.streamFile(file, "text/plain");
    file.close();
}

void WiFiManager::handleSerialLogClear() {
    if (!SerialLog.isEnabled()) {
        server.send(503, "application/json", "{\"error\":\"Serial logging not enabled\"}");
        return;
    }
    
    bool cleared = SerialLog.clear();
    server.send(cleared ? 200 : 500,
                "application/json",
                String("{\"success\":") + (cleared ? "true" : "false") + "}");
}

void WiFiManager::handleSerialLogPage() {
    SerialLog.println("[HTTP] GET /seriallog");
    
    bool sdReady = alertLogger.isReady();
    bool logEnabled = SerialLog.isEnabled();
    size_t logSize = SerialLog.getLogSize();
    
    // Format size for display
    String sizeStr;
    if (logSize < 1024) {
        sizeStr = String(logSize) + " B";
    } else if (logSize < 1024 * 1024) {
        sizeStr = String(logSize / 1024.0, 1) + " KB";
    } else if (logSize < 1024 * 1024 * 1024) {
        sizeStr = String(logSize / (1024.0 * 1024.0), 1) + " MB";
    } else {
        sizeStr = String(logSize / (1024.0 * 1024.0 * 1024.0), 2) + " GB";
    }
    
    String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>Serial Log - V1 Display</title>
    <meta name="viewport" content="width=device-width,initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 0; padding: 20px; background: #1a1a2e; color: #eee; }
        .container { max-width: 600px; margin: 0 auto; }
        h1 { color: #00d4ff; margin-bottom: 5px; }
        .subtitle { color: #888; margin-bottom: 20px; }
        .card { background: #252540; border-radius: 12px; padding: 20px; margin-bottom: 20px; }
        .status { display: flex; justify-content: space-between; align-items: center; padding: 10px 0; border-bottom: 1px solid #333; }
        .status:last-child { border-bottom: none; }
        .status-label { color: #888; }
        .status-value { font-weight: 600; }
        .status-ok { color: #4ade80; }
        .status-warn { color: #fbbf24; }
        .status-err { color: #f87171; }
        .btn { display: inline-block; padding: 12px 24px; border-radius: 8px; text-decoration: none; font-weight: 600; cursor: pointer; border: none; font-size: 16px; margin: 5px; }
        .btn-primary { background: #00d4ff; color: #000; }
        .btn-primary:hover { background: #00b8e6; }
        .btn-danger { background: #dc2626; color: #fff; }
        .btn-danger:hover { background: #b91c1c; }
        .btn-secondary { background: #444; color: #fff; }
        .btn-secondary:hover { background: #555; }
        .btn:disabled { opacity: 0.5; cursor: not-allowed; }
        .actions { text-align: center; margin-top: 20px; }
        .back { display: inline-block; margin-top: 20px; color: #00d4ff; text-decoration: none; }
        .note { color: #888; font-size: 14px; margin-top: 15px; padding: 10px; background: #1a1a2e; border-radius: 8px; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Serial Log</h1>
        <p class="subtitle">Debug output saved to SD card</p>
        
        <div class="card">
            <div class="status">
                <span class="status-label">SD Card</span>
                <span class="status-value )rawliteral";
    html += sdReady ? "status-ok\">Mounted" : "status-err\">Not Available";
    html += R"rawliteral(</span>
            </div>
            <div class="status">
                <span class="status-label">Logging</span>
                <span class="status-value )rawliteral";
    html += logEnabled ? "status-ok\">Enabled" : "status-warn\">Disabled";
    html += R"rawliteral(</span>
            </div>
            <div class="status">
                <span class="status-label">Log Size</span>
                <span class="status-value">)rawliteral";
    html += sizeStr;
    html += R"rawliteral(</span>
            </div>
            <div class="status">
                <span class="status-label">Max Size</span>
                <span class="status-value">2 GB</span>
            </div>
        </div>
        
        <div class="actions">
            <a href="/serial_log.txt" class="btn btn-primary" download="serial_log.txt">Download Log</a>
            <button class="btn btn-danger" onclick="clearLog()" )rawliteral";
    html += logEnabled ? "" : "disabled";
    html += R"rawliteral(>Clear Log</button>
        </div>
        
        <div class="note">
            <strong>Note:</strong> Serial log captures all debug output (what you'd see in <code>pio device monitor</code>). 
            Useful for debugging issues in the field. Log rotates automatically at 2GB.
        </div>
        
        <a href="/" class="back">← Back to Home</a>
    </div>
    
    <script>
    function clearLog() {
        if (!confirm('Clear the serial log? This cannot be undone.')) return;
        fetch('/api/serial_log/clear', { method: 'POST' })
            .then(r => r.json())
            .then(d => {
                if (d.success) {
                    alert('Log cleared!');
                    location.reload();
                } else {
                    alert('Failed to clear log');
                }
            })
            .catch(e => alert('Error: ' + e));
    }
    </script>
</body>
</html>
)rawliteral";
    
    server.send(200, "text/html", html);
}

void WiFiManager::handleV1Settings() {
    SerialLog.println("[HTTP] GET /v1settings");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Stream HTML in chunks
    streamLayoutHeader("V1 Profiles", "/v1settings");
    streamV1SettingsBody();
    streamLayoutFooter();
}

void WiFiManager::handleV1ProfilesList() {
    std::vector<String> profiles = v1ProfileManager.listProfiles();
    
    String json = "[";
    for (size_t i = 0; i < profiles.size(); i++) {
        if (i > 0) json += ",";
        json += "\"" + profiles[i] + "\"";
    }
    json += "]";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleV1ProfileGet() {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }
    
    String name = server.arg("name");
    V1Profile profile;
    
    if (!v1ProfileManager.loadProfile(name, profile)) {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
        return;
    }
    
    server.send(200, "application/json", v1ProfileManager.profileToJson(profile));
}

void WiFiManager::handleV1ProfileSave() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    SerialLog.printf("[V1Settings] Save request body: %s\n", body.c_str());
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }
    
    V1Profile profile;
    profile.name = name;
    profile.displayOn = doc["displayOn"] | true;  // Default to on
    
    // Parse settings from JSON
    JsonObject settingsObj = doc["settings"];
    if (!settingsObj.isNull()) {
        if (!v1ProfileManager.jsonToSettings(settingsObj, profile.settings)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    } else {
        // Direct settings in root
        JsonObject rootObj = doc.as<JsonObject>();
        if (!v1ProfileManager.jsonToSettings(rootObj, profile.settings)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    }
    
    if (v1ProfileManager.saveProfile(profile)) {
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to save profile\"}");
    }
}

void WiFiManager::handleV1ProfileDelete() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    
    String body = server.arg("plain");
    if (body.length() > 2048) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    JsonDocument doc;
    deserializeJson(doc, body);
    
    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }
    
    if (v1ProfileManager.deleteProfile(name)) {
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
    }
}

void WiFiManager::handleV1CurrentSettings() {
    if (!v1ProfileManager.hasCurrentSettings()) {
        server.send(200, "application/json", "{\"available\":false}");
        return;
    }
    
    String json = "{\"available\":true,\"settings\":";
    json += v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings());
    json += "}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleV1SettingsPull() {
    if (!bleClient.isConnected()) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }
    
    // Request user bytes from V1
    if (bleClient.requestUserBytes()) {
        // Response will come async via BLE callback
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Request sent. Check current settings.\"}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to send request\"}");
    }
}

void WiFiManager::handleV1SettingsPush() {
    if (!bleClient.isConnected()) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }
    
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }
    
    String body = server.arg("plain");
    SerialLog.printf("[V1Settings] Push request: %s\n", body.c_str());
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }
    
    uint8_t bytes[6];
    
    // Check for bytes array first
    JsonArray bytesArray = doc["bytes"];
    if (bytesArray && bytesArray.size() == 6) {
        for (int i = 0; i < 6; i++) {
            bytes[i] = bytesArray[i].as<uint8_t>();
        }
        SerialLog.println("[V1Settings] Using raw bytes from request");
    } else {
        // Parse from individual settings (already deserialized)
        V1UserSettings settings;
        JsonObject settingsObj = doc["settings"].as<JsonObject>();
        if (settingsObj.isNull()) {
            settingsObj = doc.as<JsonObject>();
        }
        if (!v1ProfileManager.jsonToSettings(settingsObj, settings)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
        memcpy(bytes, settings.bytes, 6);
        SerialLog.printf("[V1Settings] Built bytes from settings: %02X %02X %02X %02X %02X %02X\n",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    }
    
    // Get displayOn setting (defaults to true if not specified)
    bool displayOn = doc["displayOn"] | true;
    
    bool success = bleClient.writeUserBytes(bytes);
    if (success) {
        // Also set display on/off
        bleClient.setDisplayOn(displayOn);
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to write settings\"}");
    }
}

void WiFiManager::handleNotFound() {
    server.send(404, "text/plain", "Not found");
}

String WiFiManager::generateStyleSheet() {
    // Unified Pro Design System - Navy/Slate/Blue palette
    String css = R"CSS(<style>
/* === Reset & Base === */
*{box-sizing:border-box;margin:0;padding:0;}
body{
    font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Oxygen,Ubuntu,sans-serif;
    background:#0c1520;
    color:#e2e8f0;
    line-height:1.5;
    min-height:100vh;
}

/* === Layout === */
.container{max-width:720px;margin:0 auto;padding:20px 16px;}

/* === Navigation === */
.topnav{
    display:flex;gap:6px;flex-wrap:wrap;justify-content:center;
    padding:12px 16px;background:#111b27;border-bottom:1px solid #1e2d3d;
}
.topnav a{text-decoration:none;}
.navbtn{
    display:inline-flex;align-items:center;gap:6px;
    padding:8px 14px;border-radius:6px;border:none;
    background:#1e2d3d;color:#94a3b8;
    font-size:0.875rem;font-weight:500;cursor:pointer;
    transition:all 0.15s ease;
}
.navbtn:hover{background:#263548;color:#e2e8f0;}
.navbtn.active{background:#3b82f6;color:#fff;}
.navbtn svg{width:18px;height:18px;stroke:currentColor;fill:none;stroke-width:2;}

/* === Typography === */
.page-title{
    text-align:center;font-size:1.5rem;font-weight:600;
    color:#f1f5f9;margin:20px 0 24px;
}
h2{font-size:1.1rem;font-weight:600;color:#f1f5f9;margin-bottom:12px;}
h3{font-size:1rem;font-weight:600;color:#e2e8f0;margin-bottom:10px;}
.muted{color:#64748b;font-size:0.875rem;}
.subtitle{color:#64748b;font-size:0.9rem;text-align:center;margin-bottom:20px;}

/* === Cards === */
.card{
    background:#141f2e;border:1px solid #1e2d3d;
    border-radius:10px;padding:20px;margin-bottom:16px;
}
.card h2{color:#f1f5f9;border-bottom:1px solid #1e2d3d;padding-bottom:10px;margin-bottom:16px;}

/* === Forms === */
.form-group{margin-bottom:16px;}
label{display:block;margin-bottom:6px;color:#94a3b8;font-size:0.875rem;font-weight:500;}
input,select,textarea{
    width:100%;padding:10px 12px;
    background:#0c1520;border:1px solid #1e2d3d;border-radius:6px;
    color:#e2e8f0;font-size:0.95rem;
    transition:border-color 0.15s,box-shadow 0.15s;
}
input:focus,select:focus,textarea:focus{
    outline:none;border-color:#3b82f6;
    box-shadow:0 0 0 3px rgba(59,130,246,0.15);
}
input::placeholder{color:#475569;}
input[type="number"]{-moz-appearance:textfield;}
input[type="number"]::-webkit-inner-spin-button{-webkit-appearance:none;}

/* === Buttons === */
.btn{
    display:inline-flex;align-items:center;justify-content:center;gap:6px;
    padding:10px 18px;border:none;border-radius:6px;
    font-size:0.95rem;font-weight:500;cursor:pointer;
    transition:all 0.15s ease;text-decoration:none;
}
.btn-primary{background:#3b82f6;color:#fff;}
.btn-primary:hover{background:#2563eb;}
.btn-secondary{background:#1e2d3d;color:#e2e8f0;}
.btn-secondary:hover{background:#263548;}
.btn-success{background:#10b981;color:#fff;}
.btn-success:hover{background:#059669;}
.btn-danger,.btn.danger{background:#ef4444;color:#fff;}
.btn-danger:hover,.btn.danger:hover{background:#dc2626;}
.btn-full{width:100%;margin-top:8px;}
.btn-group{display:flex;gap:8px;flex-wrap:wrap;}
.btn-group .btn{flex:1;min-width:100px;}
.btn:disabled{opacity:0.5;cursor:not-allowed;}

/* === Messages === */
.msg{padding:12px 16px;border-radius:6px;margin-bottom:16px;text-align:center;font-weight:500;}
.msg.success{background:#064e3b;color:#6ee7b7;border:1px solid #10b981;}
.msg.error{background:#450a0a;color:#fca5a5;border:1px solid #ef4444;}
.msg.info{background:#1e3a5f;color:#93c5fd;border:1px solid #3b82f6;}

/* === Tables === */
table{width:100%;border-collapse:collapse;font-size:0.875rem;}
thead{background:#111b27;}
th{padding:10px 8px;text-align:left;color:#94a3b8;font-weight:600;border-bottom:1px solid #1e2d3d;cursor:pointer;}
th:hover{color:#e2e8f0;}
th.sort-asc::after{content:' ↑';color:#3b82f6;}
th.sort-desc::after{content:' ↓';color:#3b82f6;}
td{padding:10px 8px;border-bottom:1px solid #1e2d3d;color:#cbd5e1;}
tr:hover{background:#111b27;}

/* === Modal === */
.modal{
    display:none;position:fixed;inset:0;
    background:rgba(0,0,0,0.7);z-index:1000;
    align-items:center;justify-content:center;padding:16px;
}
.modal.show{display:flex;}
.modal-box{
    background:#141f2e;border:1px solid #1e2d3d;border-radius:10px;
    padding:24px;max-width:500px;width:100%;max-height:90vh;overflow-y:auto;
}
.modal-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px;}
.modal-close{background:none;border:none;color:#94a3b8;font-size:1.5rem;cursor:pointer;padding:0;}
.modal-close:hover{color:#e2e8f0;}
.modal-btns{display:flex;gap:10px;margin-top:20px;}
.modal-btns .btn{flex:1;}

/* === Status Badges === */
.status{padding:12px;border-radius:6px;margin-bottom:16px;}
.status.connected{background:#064e3b;border:1px solid #10b981;color:#6ee7b7;}
.status.disconnected{background:#450a0a;border:1px solid #ef4444;color:#fca5a5;}

/* === Actions Bar === */
.actions{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px;}
.actions .btn{flex:none;}

/* === Filters === */
.filters{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:12px;}
.filter-btn{
    padding:6px 12px;border:1px solid #1e2d3d;border-radius:6px;
    background:#111b27;color:#94a3b8;font-size:0.8rem;cursor:pointer;
    transition:all 0.15s;
}
.filter-btn:hover{border-color:#3b82f6;color:#e2e8f0;}
.filter-btn.active{background:#3b82f6;color:#fff;border-color:#3b82f6;}
.filter-btn .count{opacity:0.7;margin-left:4px;}

/* === Stats === */
.stats{color:#64748b;font-size:0.875rem;margin-bottom:12px;}
.stats b{color:#e2e8f0;}

/* === Profile List === */
.profile-list{list-style:none;}
.profile-item{
    background:#111b27;padding:12px 16px;margin:6px 0;border-radius:6px;
    border:1px solid #1e2d3d;display:flex;justify-content:space-between;
    align-items:center;cursor:pointer;transition:all 0.15s;
}
.profile-item:hover{border-color:#3b82f6;background:#141f2e;}
.profile-item.selected{border-color:#3b82f6;background:#1e3a5f;}
.profile-name{font-weight:500;color:#e2e8f0;}

/* === Slot Cards (Auto-Push) === */
.slot-card{border:2px solid #1e2d3d;transition:border-color 0.15s;}
.slot-card.active{border-color:#3b82f6;background:#111b27;}
.slot-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px;}
.slot-title{display:flex;align-items:center;gap:10px;}
.slot-icon{
    width:36px;height:36px;background:#1e2d3d;border-radius:6px;
    display:flex;align-items:center;justify-content:center;font-size:1.2rem;
}
.active-badge{background:#3b82f6;color:#fff;padding:4px 10px;border-radius:12px;font-size:0.75rem;font-weight:600;}

/* === Quick Push Grid === */
.quick-push{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin-bottom:20px;}
.quick-btn{
    padding:16px;background:#141f2e;border:1px solid #1e2d3d;border-radius:8px;
    cursor:pointer;text-align:center;transition:all 0.15s;
}
.quick-btn:hover{border-color:#3b82f6;transform:translateY(-1px);}
.quick-btn h3{color:#e2e8f0;margin-bottom:4px;}
.quick-btn .quick-profile{color:#64748b;font-size:0.85rem;}

/* === Color Picker Grid === */
.color-grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(140px,1fr));gap:12px;}
.color-item{background:#111b27;border:1px solid #1e2d3d;border-radius:8px;padding:12px;text-align:center;}
.color-item label{margin-bottom:8px;font-size:0.8rem;}
.color-preview{
    width:40px;height:40px;border-radius:6px;margin:8px auto;
    border:2px solid #1e2d3d;
}
input[type="color"]{
    width:100%;height:36px;padding:2px;cursor:pointer;
    background:#0c1520;border:1px solid #1e2d3d;border-radius:4px;
}

/* === Toggle Switch === */
.toggle{
    width:48px;height:26px;background:#1e2d3d;border-radius:13px;
    position:relative;cursor:pointer;transition:background 0.2s;
}
.toggle.on{background:#3b82f6;}
.toggle::after{
    content:'';position:absolute;width:22px;height:22px;
    background:#fff;border-radius:11px;top:2px;left:2px;
    transition:left 0.2s;
}
.toggle.on::after{left:24px;}

/* === Setting Row === */
.setting-row{
    display:flex;justify-content:space-between;align-items:center;
    padding:12px 0;border-bottom:1px solid #1e2d3d;
}
.setting-row:last-child{border-bottom:none;}

/* === Bytes Display === */
.bytes-display{
    font-family:'SF Mono',Monaco,Consolas,monospace;
    background:#0c1520;padding:10px;border-radius:6px;
    font-size:0.8rem;color:#94a3b8;word-break:break-all;
}

/* === Utility === */
.text-center{text-align:center;}
.mt-2{margin-top:8px;}
.mt-4{margin-top:16px;}
.mb-2{margin-bottom:8px;}
.mb-4{margin-bottom:16px;}
.flex{display:flex;}
.gap-2{gap:8px;}
.items-center{align-items:center;}
.justify-between{justify-content:space-between;}
</style>)CSS";
    return css;
}

String WiFiManager::generateProfileOptions(const String& selected) {
    String options;
    auto profiles = v1ProfileManager.listProfiles();
    for (const auto& name : profiles) {
        String escaped = htmlEscape(name);
        options += "<option value=\"" + escaped + "\"";
        if (name == selected) {
            options += " selected";
        }
        options += ">" + escaped + "</option>\n";
    }
    return options;
}

String WiFiManager::generateTopNav(const String& activePath) {
    String nav = "<div class=\"topnav\">\n";

    // Settings (sliders icon)
    nav += String("<a href=\"/settings\"><button class=\"navbtn") + (activePath == "/settings" ? " active" : "") + "\">"
        "<svg viewBox=\"0 0 24 24\"><path d=\"M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z\"/><circle cx=\"12\" cy=\"12\" r=\"3\"/>"
        "</svg>Settings</button></a>\n";

    // Time (clock icon)
    nav += String("<a href=\"/time\"><button class=\"navbtn") + (activePath == "/time" ? " active" : "") + "\">"
        "<svg viewBox=\"0 0 24 24\"><circle cx=\"12\" cy=\"12\" r=\"10\"/><path d=\"M12 6v6l4 2\"/>"
        "</svg>Time</button></a>\n";

    // Logs (file-text icon)
    nav += String("<a href=\"/logs\"><button class=\"navbtn") + (activePath == "/logs" ? " active" : "") + "\">"
        "<svg viewBox=\"0 0 24 24\"><path d=\"M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z\"/><path d=\"M14 2v6h6\"/><path d=\"M16 13H8\"/><path d=\"M16 17H8\"/><path d=\"M10 9H8\"/>"
        "</svg>Logs</button></a>\n";

    // V1 Settings (sliders-horizontal icon)
    nav += String("<a href=\"/v1settings\"><button class=\"navbtn") + (activePath == "/v1settings" ? " active" : "") + "\">"
        "<svg viewBox=\"0 0 24 24\"><path d=\"M21 4H3\"/><path d=\"M21 12H3\"/><path d=\"M21 20H3\"/><circle cx=\"9\" cy=\"4\" r=\"2\"/><circle cx=\"15\" cy=\"12\" r=\"2\"/><circle cx=\"9\" cy=\"20\" r=\"2\"/>"
        "</svg>V1 Profiles</button></a>\n";

    // Auto-Push (zap icon)
    nav += String("<a href=\"/autopush\"><button class=\"navbtn") + (activePath == "/autopush" ? " active" : "") + "\">"
        "<svg viewBox=\"0 0 24 24\"><path d=\"M13 2L3 14h9l-1 8 10-12h-9l1-8z\"/>"
        "</svg>Auto-Push</button></a>\n";

    // Display Colors (palette icon)
    nav += String("<a href=\"/displaycolors\"><button class=\"navbtn") + (activePath == "/displaycolors" ? " active" : "") + "\">"
        "<svg viewBox=\"0 0 24 24\"><circle cx=\"13.5\" cy=\"6.5\" r=\".5\"/><circle cx=\"17.5\" cy=\"10.5\" r=\".5\"/><circle cx=\"8.5\" cy=\"7.5\" r=\".5\"/><circle cx=\"6.5\" cy=\"12.5\" r=\".5\"/><path d=\"M12 2C6.5 2 2 6.5 2 12s4.5 10 10 10c.926 0 1.648-.746 1.648-1.688 0-.437-.18-.835-.437-1.125-.29-.289-.438-.652-.438-1.125a1.64 1.64 0 0 1 1.668-1.668h1.996c3.051 0 5.555-2.503 5.555-5.555C21.965 6.012 17.461 2 12 2z\"/>"
        "</svg>Colors</button></a>\n";

    nav += "</div>";
    return nav;
}

String WiFiManager::wrapWithLayout(const String& title, const String& body, const String& activePath) {
    String html;
    html += R"HTML(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>)HTML" + htmlEscape(title) + R"HTML(</title>
)HTML";
    html += generateStyleSheet();
    html += R"HTML(</head>
<body>
)HTML";
    html += generateTopNav(activePath);
    html += String("<h1 class=\"page-title\">") + htmlEscape(title) + "</h1>";
    html += "<div class=\"container\">";
    html += body;
    html += "</div>";
    html += R"HTML(
</body>
</html>)HTML";
    return html;
}




String WiFiManager::generateSettingsHTML() {
    const V1Settings& settings = settingsManager.get();
    String apSsidEsc = htmlEscape(settings.apSSID);
    // Don't echo back password for security - show placeholder if set
    String apPassEsc = settings.apPassword.length() > 0 ? "********" : "";
    String staSsidEsc = htmlEscape(settings.ssid);
    String staPassEsc = settings.password.length() > 0 ? "********" : "";

    SerialLog.println("=== generateSettingsHTML() ===");
    SerialLog.printf("  brightness: %d\n", settings.brightness);
    SerialLog.printf("  wifiMode: %d\n", settings.wifiMode);
    SerialLog.printf("  apSSID: %s\n", settings.apSSID.c_str());
    SerialLog.printf("  colorTheme: %d (STANDARD=%d, HIGH_CONTRAST=%d, STEALTH=%d)\n",
                  settings.colorTheme, THEME_STANDARD, THEME_HIGH_CONTRAST, THEME_STEALTH);

    String saved = "";
    if (server.hasArg("saved")) {
        if (server.hasArg("wifi")) {
            saved = "<div class='msg success'>Settings saved! Restart to apply WiFi changes.</div>";
        } else {
            saved = "<div class='msg success'>Settings saved!</div>";
        }
    }

    String body;
    body += saved;

    // Time Settings Card
    body += "<div class=\"card\"><h2>Time Settings</h2>";
    body += "<p class=\"muted\">Configure automatic time sync via NTP or set time manually for accurate timestamps.</p>";

    if (timeManager.isTimeValid()) {
        String timeStr = timeManager.getTimestampISO();
        const V1Settings& s = settingsManager.get();
        String syncStatus = s.enableTimesync ? "NTP Sync Enabled" : "Manual Time";
        body += "<div class=\"setting-row\"><span class=\"muted\">" + syncStatus + "</span><span>" + timeStr + " UTC</span></div>";
    } else {
        body += "<div class=\"msg error\">Time Not Set</div>";
    }

    body += "<a class=\"btn primary\" href=\"/time\">Open Time Settings</a></div>";

    // V1 Settings Card
    body += "<div class=\"card\"><h2>V1 Settings</h2>";
    body += "<p class=\"muted\">Pull, edit, and push V1 user settings. Save profiles for quick switching.</p>";
    body += "<a class=\"btn primary\" href=\"/v1settings\">Open V1 Settings</a></div>";

    // Alert Logs Card  
    body += "<div class=\"card\"><h2>Alert Logs</h2>";
    body += "<p class=\"muted\">Alerts are recorded to the SD card. View and clear them from the log page.</p>";
    body += "<a class=\"btn primary\" href=\"/logs\">Open Alert Log</a></div>";

    // WiFi Settings Form
    body += "<form method=\"POST\" action=\"/settings\">";
    body += "<div class=\"card\"><h2>WiFi Mode & Access Point</h2>";
    
    body += "<div class=\"form-group\"><label>WiFi Mode</label>";
    body += "<select name=\"wifi_mode\">";
    body += String("<option value=\"2\"") + (settings.wifiMode == V1_WIFI_AP ? " selected" : "") + ">Access Point Only</option>";
    body += String("<option value=\"1\"") + (settings.wifiMode == V1_WIFI_STA ? " selected" : "") + ">Station Only</option>";
    body += String("<option value=\"3\"") + (settings.wifiMode == V1_WIFI_APSTA ? " selected" : "") + ">AP + Station (NAT passthrough)</option>";
    body += "</select></div>";

    body += "<div class=\"form-group\"><label>AP Network Name</label>";
    body += "<input type=\"text\" name=\"ap_ssid\" value=\"" + apSsidEsc + "\"></div>";

    body += "<div class=\"form-group\"><label>AP Password (min 8 chars)</label>";
    body += "<input type=\"password\" name=\"ap_password\" value=\"" + apPassEsc + "\"></div>";

    body += "<div class=\"form-group\"><label>Upstream WiFi (STA) SSID</label>";
    body += "<input type=\"text\" name=\"ssid\" value=\"" + staSsidEsc + "\" placeholder=\"Home/Hotspot SSID\"></div>";

    body += "<div class=\"form-group\"><label>Upstream WiFi Password</label>";
    body += "<input type=\"password\" name=\"password\" value=\"" + staPassEsc + "\" placeholder=\"Password\"></div>";
    body += "</div>";

    // Display Settings Card
    body += "<div class=\"card\"><h2>Display</h2>";
    body += "<div class=\"form-group\"><label>Brightness (0-255)</label>";
    body += "<input type=\"number\" name=\"brightness\" min=\"0\" max=\"255\" value=\"" + String(settings.brightness) + "\"></div>";
    
    body += "<div class=\"form-group\"><label>Color Theme</label>";
    body += "<select name=\"color_theme\">";
    body += String("<option value=\"0\"") + (settings.colorTheme == THEME_STANDARD ? " selected" : "") + ">Standard</option>";
    body += String("<option value=\"1\"") + (settings.colorTheme == THEME_HIGH_CONTRAST ? " selected" : "") + ">High Contrast</option>";
    body += String("<option value=\"2\"") + (settings.colorTheme == THEME_STEALTH ? " selected" : "") + ">Stealth</option>";
    body += String("<option value=\"3\"") + (settings.colorTheme == THEME_BUSINESS ? " selected" : "") + ">Business</option>";
    body += "</select></div></div>";

    // Quick-Access Profiles Card
    body += "<div class=\"card\"><h2>Quick-Access Profiles</h2>";
    body += "<p class=\"muted\">Configure 3 quick-access profiles for different driving scenarios.</p>";
    body += "<a class=\"btn success\" href=\"/autopush\">Manage Auto-Push Profiles</a></div>";

    // Display Colors Card
    body += "<div class=\"card\"><h2>Display Colors</h2>";
    body += "<p class=\"muted\">Customize bogey, frequency, band, and arrow colors.</p>";
    body += "<a class=\"btn secondary\" href=\"/displaycolors\">Customize Display Colors</a></div>";

    body += "<button type=\"submit\" class=\"btn primary\">Save Settings</button>";
    body += "</form>";

    return wrapWithLayout("V1 Display Settings", body, "/settings");
}

String WiFiManager::generateLogsHTML() {
    const V1Settings& settings = settingsManager.get();
    String statusBox;
    if (alertLogger.isReady()) {
        statusBox = String("<div class='msg success'>") + alertLogger.statusText() + "</div>";
    } else {
        statusBox = "<div class='msg error'>SD card not mounted</div>";
    }

    String body;
    body += statusBox;
    body += R"HTML(
        <div class="actions">
            <button class="btn secondary" onclick="loadLogs()">Refresh</button>
            <button class="btn danger" onclick="showClearModal()">Clear All</button>
            <a class="btn secondary" href="/settings">Back to Settings</a>
        </div>
        <div class="filters" id="filters">
            <span>Filter:</span>
            <button class="filter-btn active" data-band="all">All</button>
        </div>
        <div class="stats" id="stats"></div>
        <div class="card">
            <table>
                <thead>
                    <tr>
                        <th data-col="utc" data-type="num">Time (UTC)</th>
                        <th data-col="event" data-type="str">Event</th>
                        <th data-col="band" data-type="str">Band</th>
                        <th data-col="freq" data-type="num">Freq</th>
                        <th data-col="dir" data-type="str">Dir</th>
                        <th data-col="front" data-type="num">Front</th>
                        <th data-col="rear" data-type="num">Rear</th>
                        <th data-col="count" data-type="num">Count</th>
                        <th data-col="muted" data-type="bool">Muted</th>
                    </tr>
                </thead>
                <tbody id="log-body"></tbody>
            </table>
        </div>
    
    <!-- Clear Confirmation Modal -->
    <div class="modal" id="clearModal">
        <div class="modal-box">
            <h3>Clear All Logs?</h3>
            <p>This will permanently delete all alert log data from the SD card. This action cannot be undone.</p>
            <div class="modal-btns">
                <button class="btn secondary" onclick="hideClearModal()">Cancel</button>
                <button class="btn danger" onclick="confirmClear()">Delete All</button>
            </div>
        </div>
    </div>
    
    <script>
        const bodyEl = document.getElementById('log-body');
        const statsEl = document.getElementById('stats');
        const filtersEl = document.getElementById('filters');
        const modal = document.getElementById('clearModal');
        let logData = [];
        let filteredData = [];
        let sortCol = 'utc';
        let sortDir = 'desc';
        let activeFilter = 'all';

        function formatTime(utc, ts){
            // If we have a real UTC timestamp, show formatted date/time
            if(utc && utc > 1609459200){
                const d = new Date(utc * 1000);
                const pad = n => String(n).padStart(2,'0');
                return `${d.getUTCMonth()+1}/${d.getUTCDate()} ${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}`;
            }
            // Fallback to relative time since boot
            if(!ts || ts <= 0) return '-';
            const sec = ts/1000;
            if(sec < 60) return sec.toFixed(1) + 's';
            if(sec < 3600) return Math.floor(sec/60) + 'm ' + Math.floor(sec%60) + 's';
            return Math.floor(sec/3600) + 'h ' + Math.floor((sec%3600)/60) + 'm';
        }

        function updateFilters(){
            const bands = {};
            logData.forEach(d => { 
                if(d.band && d.band !== 'NONE') bands[d.band] = (bands[d.band]||0)+1; 
            });
            
            // Keep 'All' button, add band buttons (exclude NONE)
            let html = '<span>Filter:</span><button class="filter-btn' + (activeFilter==='all'?' active':'') + '" data-band="all">All<span class="count">(' + logData.length + ')</span></button>';
            Object.keys(bands).sort().forEach(b => {
                const isActive = activeFilter === b ? ' active' : '';
                html += `<button class="filter-btn${isActive}" data-band="${b}">${b}<span class="count">(${bands[b]})</span></button>`;
            });
            filtersEl.innerHTML = html;
            
            // Re-attach click handlers
            filtersEl.querySelectorAll('.filter-btn').forEach(btn => {
                btn.addEventListener('click', () => {
                    activeFilter = btn.dataset.band;
                    applyFilter();
                    updateFilters();
                });
            });
        }

        function applyFilter(){
            if(activeFilter === 'all') filteredData = [...logData];
            else filteredData = logData.filter(d => d.band === activeFilter);
            sortData();
            renderTable();
            updateStats();
        }

        function updateStats(){
            if(!filteredData.length){ 
                statsEl.innerHTML = activeFilter === 'all' ? '' : `<b>0</b> ${activeFilter} alerts`; 
                return; 
            }
            if(activeFilter === 'all'){
                statsEl.innerHTML = `Showing <b>${filteredData.length}</b> alerts`;
            } else {
                statsEl.innerHTML = `Showing <b>${filteredData.length}</b> ${activeFilter} alerts`;
            }
        }

        function sortData(){
            const type = document.querySelector(`th[data-col="${sortCol}"]`)?.dataset.type || 'str';
            filteredData.sort((a,b) => {
                let va, vb;
                // Special handling for time column - prefer utc, fallback to ts
                if(sortCol === 'utc'){
                    va = a.utc || a.ts || 0;
                    vb = b.utc || b.ts || 0;
                } else {
                    va = a[sortCol];
                    vb = b[sortCol];
                }
                if(type === 'num'){ va = Number(va)||0; vb = Number(vb)||0; }
                else if(type === 'bool'){ va = va?1:0; vb = vb?1:0; }
                else { va = String(va||'').toLowerCase(); vb = String(vb||'').toLowerCase(); }
                if(va < vb) return sortDir === 'asc' ? -1 : 1;
                if(va > vb) return sortDir === 'asc' ? 1 : -1;
                return 0;
            });
        }

        function renderTable(){
            document.querySelectorAll('th').forEach(th => {
                th.classList.remove('sort-asc','sort-desc');
                if(th.dataset.col === sortCol) th.classList.add('sort-'+sortDir);
            });
            if(!filteredData.length){
                bodyEl.innerHTML = '<tr><td colspan="9" class="muted">No entries</td></tr>';
                return;
            }
            bodyEl.innerHTML = '';
            filteredData.forEach(item => {
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td>${formatTime(item.utc, item.ts)}</td>
                    <td>${item.event}</td>
                    <td>${item.band || '-'}</td>
                    <td>${item.freq || '-'}</td>
                    <td>${item.dir || '-'}</td>
                    <td>${item.front ?? '-'}</td>
                    <td>${item.rear ?? '-'}</td>
                    <td>${item.count}</td>
                    <td>${item.muted ? 'Yes' : 'No'}</td>`;
                bodyEl.appendChild(tr);
            });
        }

        function loadLogs(){
            bodyEl.innerHTML = '<tr><td colspan="9" class="muted">Loading...</td></tr>';
            fetch('/api/logs').then(r => {
                if(!r.ok) throw new Error('Failed');
                return r.json();
            }).then(data => {
                logData = Array.isArray(data) ? data : [];
                updateFilters();
                applyFilter();
            }).catch(() => {
                bodyEl.innerHTML = '<tr><td colspan="9">Error loading log</td></tr>';
            });
        }

        function showClearModal(){ modal.classList.add('show'); }
        function hideClearModal(){ modal.classList.remove('show'); }
        
        function confirmClear(){
            hideClearModal();
            fetch('/api/logs/clear',{method:'POST'}).then(r=>r.json()).then(d=>{
                activeFilter = 'all';
                loadLogs();
            }).catch(()=>{});
        }

        document.querySelectorAll('th[data-col]').forEach(th => {
            th.addEventListener('click', () => {
                const col = th.dataset.col;
                if(sortCol === col) sortDir = sortDir === 'asc' ? 'desc' : 'asc';
                else { sortCol = col; sortDir = 'desc'; }
                sortData();
                renderTable();
            });
        });

        // Close modal on backdrop click
        modal.addEventListener('click', (e) => { if(e.target === modal) hideClearModal(); });

        window.onload = loadLogs;
    </script>
    )HTML";

    return wrapWithLayout("Alert Log", body, "/logs");
}

String WiFiManager::generateV1SettingsHTML() {
    String currentJson = "null";
    if (v1ProfileManager.hasCurrentSettings()) {
        currentJson = v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings());
    }
    const V1Settings& settings = settingsManager.get();
    String body = R"HTML(
    
    <div id="status" class="status-badge disconnected">
        Checking V1 connection...
    </div>
    
    <div class="card">
        <h2>Current V1 Settings</h2>
        <div id="current-settings">
            <p class="muted">Pull settings from V1 to view</p>
        </div>
        <button class="btn primary" onclick="pullSettings()">Pull from V1</button>
    </div>
    
    <div class="card">
        <h2>Saved Profiles</h2>
        <ul id="profile-list" class="profile-list">
            <li class="muted">Loading profiles...</li>
        </ul>
        <div class="actions">
            <button class="btn primary" onclick="showNewProfileModal()">New Profile</button>
            <button id="btn-edit" class="btn secondary" onclick="editSelectedProfile()" disabled>Edit</button>
            <button id="btn-push" class="btn success" onclick="pushSelectedProfile()" disabled>Push to V1</button>
            <button id="btn-delete" class="btn danger" onclick="deleteSelectedProfile()" disabled>Delete</button>
        </div>
    </div>
    
    <!-- Edit Modal -->
    <div id="edit-modal" class="modal">
        <div class="modal-box">
            <div class="modal-header">
                <h2 id="modal-title">Edit Profile</h2>
                <button class="modal-close" onclick="closeEditModal()">×</button>
            </div>
            <input type="text" id="profile-name" placeholder="Profile Name">
            <div id="settings-editor"></div>
            <div class="modal-btns">
                <button class="btn primary" onclick="saveProfile()">Save</button>
                <button class="btn secondary" onclick="closeEditModal()">Cancel</button>
                <button id="btn-push-after-save" class="btn success" onclick="saveAndPush()">Save & Push</button>
            </div>
        </div>
    </div>
    
    <!-- New Profile Modal -->
    <div id="new-modal" class="modal">
        <div class="modal-box">
            <div class="modal-header">
                <h2>New Profile</h2>
                <button class="modal-close" onclick="closeNewModal()">×</button>
            </div>
            <input type="text" id="new-profile-name" placeholder="Profile Name">
            <p class="muted">Create from:</p>
            <button class="btn primary" onclick="createFromDefaults()">Factory Defaults</button>
            <button class="btn secondary" onclick="createFromCurrent()" id="btn-from-current" disabled>Current V1 Settings</button>
            <button class="btn secondary" onclick="closeNewModal()">Cancel</button>
        </div>
    </div>
    
    <script>
        let profiles = [];
        let selectedProfile = null;
        let currentSettings = )HTML" + currentJson + R"HTML(;
        let editingSettings = null;
        let editingDisplayOn = true;  // V1 display on/off (separate from user bytes)
        
        const settingDefs = [
            {key:'xBand', label:'X Band', type:'toggle'},
            {key:'kBand', label:'K Band', type:'toggle'},
            {key:'kaBand', label:'Ka Band', type:'toggle'},
            {key:'laser', label:'Laser', type:'toggle'},
            {key:'kuBand', label:'Ku Band', type:'toggle'},
            {key:'euro', label:'Euro Mode', type:'toggle'},
            {key:'kVerifier', label:'K Verifier (TMF)', type:'toggle'},
            {key:'laserRear', label:'Laser Rear', type:'toggle'},
            {key:'customFreqs', label:'Custom Frequencies', type:'toggle'},
            {key:'kaAlwaysPriority', label:'Ka Always Priority', type:'toggle'},
            {key:'fastLaserDetect', label:'Fast Laser Detect', type:'toggle'},
            {key:'muteToMuteVolume', label:'Mute to Mute Volume', type:'toggle'},
            {key:'bogeyLockLoud', label:'Bogey Lock Loud', type:'toggle'},
            {key:'muteXKRear', label:'Mute X/K Rear', type:'toggle'},
            {key:'startupSequence', label:'Startup Sequence', type:'toggle'},
            {key:'restingDisplay', label:'Resting Display (ON=Logo when idle, OFF=Blank until alert)', type:'toggle'},
            {key:'bsmPlus', label:'BSM Plus', type:'toggle'},
            {key:'mrct', label:'MRCT', type:'toggle'},
            {key:'driveSafe3D', label:'DriveSafe 3D', type:'toggle'},
            {key:'driveSafe3DHD', label:'DriveSafe 3DHD', type:'toggle'},
            {key:'redflexHalo', label:'Redflex Halo', type:'toggle'},
            {key:'redflexNK7', label:'Redflex NK7', type:'toggle'},
            {key:'ekin', label:'Ekin', type:'toggle'},
            {key:'photoVerifier', label:'Photo Verifier', type:'toggle'},
            {key:'kaSensitivity', label:'Ka Sensitivity', type:'select', options:[[3,'Full'],[2,'Original'],[1,'Relaxed']]},
            {key:'kSensitivity', label:'K Sensitivity', type:'select', options:[[2,'Full'],[3,'Original'],[1,'Relaxed']]},
            {key:'xSensitivity', label:'X Sensitivity', type:'select', options:[[2,'Full'],[3,'Original'],[1,'Relaxed']]},
            {key:'autoMute', label:'Auto Mute', type:'select', options:[[3,'Off'],[1,'On'],[2,'Advanced']]}
        ];
        
        function checkStatus() {
            fetch('/status').then(r=>r.json()).then(d=>{
                const el = document.getElementById('status');
                if(d.v1_connected){
                    el.className = 'status connected';
                    el.textContent = 'V1 Connected';
                } else {
                    el.className = 'status disconnected';
                    el.textContent = 'V1 Disconnected';
                }
            }).catch(()=>{});
        }
        
        function loadProfiles() {
            fetch('/api/v1/profiles').then(r=>r.json()).then(data=>{
                profiles = data;
                renderProfiles();
            }).catch(e=>console.error('Load profiles error:', e));
        }
        
        function renderProfiles() {
            const list = document.getElementById('profile-list');
            if(profiles.length === 0){
                list.innerHTML = '<li class="muted">No saved profiles</li>';
                return;
            }
            list.innerHTML = profiles.map(p => 
                `<li class="profile-item ${selectedProfile===p?'selected':''}" onclick="selectProfile('${p}')">
                    <span class="profile-name">${p}</span>
                </li>`
            ).join('');
            updateButtons();
        }
        
        function selectProfile(name) {
            selectedProfile = selectedProfile === name ? null : name;
            renderProfiles();
        }
        
        function updateButtons() {
            const hasSelection = selectedProfile !== null;
            document.getElementById('btn-edit').disabled = !hasSelection;
            document.getElementById('btn-push').disabled = !hasSelection;
            document.getElementById('btn-delete').disabled = !hasSelection;
            document.getElementById('btn-from-current').disabled = !currentSettings;
        }
        
        function pullSettings() {
            fetch('/api/v1/pull', {method:'POST'}).then(r=>r.json()).then(d=>{
                if(d.success){
                    // Poll for settings to arrive (async BLE response)
                    let attempts = 0;
                    const pollForSettings = () => {
                        fetch('/api/v1/current').then(r=>r.json()).then(data=>{
                            if(data.available){
                                currentSettings = data.settings;
                                renderCurrentSettings();
                                updateButtons();
                                alert('Settings pulled from V1!');
                            } else if(attempts < 10){
                                attempts++;
                                setTimeout(pollForSettings, 300);
                            } else {
                                alert('Timeout waiting for V1 response');
                            }
                        });
                    };
                    setTimeout(pollForSettings, 300);
                } else {
                    alert('Error: ' + (d.error || 'Unknown'));
                }
            }).catch(e=>alert('Error: '+e));
        }
        
        function renderCurrentSettings() {
            const el = document.getElementById('current-settings');
            if(!currentSettings){
                el.innerHTML = '<p class="muted">Pull settings from V1 to view</p>';
                return;
            }
            let html = '<div class="muted" style="font-family:monospace;font-size:0.85rem;margin-bottom:12px;">Bytes: ' + currentSettings.bytes.map(b=>'0x'+b.toString(16).toUpperCase().padStart(2,'0')).join(' ') + '</div>';
            html += '<div class="muted" style="max-height:300px;overflow-y:auto;">';
            
            // Group settings by category
            const bands = settingDefs.filter(s=>['xBand','kBand','kaBand','laser','kuBand'].includes(s.key));
            const sensitivity = settingDefs.filter(s=>s.key.includes('Sensitivity'));
            const features = settingDefs.filter(s=>!bands.includes(s) && !sensitivity.includes(s) && s.type==='toggle');
            const other = settingDefs.filter(s=>s.type==='select' && !sensitivity.includes(s));
            
            const renderGroup = (title, items) => {
                if(items.length === 0) return '';
                let g = '<div style="margin-top:8px;"><strong>'+title+'</strong>';
                items.forEach(s=>{
                    const val = currentSettings[s.key];
                    if(s.type==='toggle'){
                        g += `<div style="margin-left:8px;">${s.label}: ${val?'On':'Off'}</div>`;
                    } else if(s.type==='select'){
                        const opt = s.options.find(o=>o[0]===val);
                        g += `<div style="margin-left:8px;">${s.label}: ${opt?opt[1]:val}</div>`;
                    }
                });
                g += '</div>';
                return g;
            };
            
            html += renderGroup('Bands', bands);
            html += renderGroup('Sensitivity', sensitivity);
            html += renderGroup('Features', features);
            html += renderGroup('Other', other);
            html += '</div>';
            el.innerHTML = html;
            updateButtons();
        }
        
        function showNewProfileModal() {
            document.getElementById('new-profile-name').value = '';
            document.getElementById('new-modal').classList.add('show');
        }
        
        function closeNewModal() {
            document.getElementById('new-modal').classList.remove('show');
        }
        
        function createFromDefaults() {
            const name = document.getElementById('new-profile-name').value.trim();
            if(!name){alert('Please enter a profile name');return;}
            editingSettings = {bytes:[255,255,255,255,255,255]};
            // Set all toggles to true (default)
            settingDefs.forEach(s=>{
                if(s.type==='toggle') editingSettings[s.key] = true;
                else if(s.type==='select') editingSettings[s.key] = s.options[0][0];
            });
            closeNewModal();
            showEditModal(name, true);
        }
        
        function createFromCurrent() {
            const name = document.getElementById('new-profile-name').value.trim();
            if(!name){alert('Please enter a profile name');return;}
            if(!currentSettings){alert('No current settings available');return;}
            editingSettings = JSON.parse(JSON.stringify(currentSettings));
            closeNewModal();
            showEditModal(name, true);
        }
        
        function editSelectedProfile() {
            if(!selectedProfile) return;
            fetch('/api/v1/profile?name='+encodeURIComponent(selectedProfile)).then(r=>r.json()).then(data=>{
                editingSettings = data.settings;
                editingDisplayOn = data.displayOn !== false;  // Default to true
                showEditModal(selectedProfile, false);
            }).catch(e=>alert('Error loading profile: '+e));
        }
        
        function showEditModal(name, isNew) {
            document.getElementById('modal-title').textContent = isNew ? 'New Profile' : 'Edit Profile';
            document.getElementById('profile-name').value = name;
            renderSettingsEditor();
            document.getElementById('edit-modal').classList.add('show');
        }
        
        function closeEditModal() {
            document.getElementById('edit-modal').classList.remove('show');
            editingSettings = null;
        }
        
        function renderSettingsEditor() {
            const el = document.getElementById('settings-editor');
            let html = '';
            
            // V1 Display On/Off (dark mode) - not part of user bytes
            html += `<div class="setting-row" style="background:#1a3a1a;margin:-16px -16px 16px -16px;padding:16px;border-radius:12px 12px 0 0;">
                <span style="font-weight:bold;">V1 Display On</span>
                <div class="toggle ${editingDisplayOn?'on':''}" onclick="toggleDisplayOn()"></div>
            </div>`;
            html += '<p style="color:#888;font-size:0.85rem;margin-bottom:12px;">OFF = Full stealth (no display even with alerts). For blank-until-alert, keep this ON and turn Resting Display OFF.</p>';
            
            settingDefs.forEach(s=>{
                if(s.type==='toggle'){
                    const on = editingSettings[s.key];
                    html += `<div class="setting-row">
                        <span>${s.label}</span>
                        <div class="toggle ${on?'on':''}" onclick="toggleSetting('${s.key}')"></div>
                    </div>`;
                } else if(s.type==='select'){
                    const val = editingSettings[s.key];
                    html += `<div class="setting-row select-row">
                        <span>${s.label}</span>
                        <select onchange="selectSetting('${s.key}',this.value)">
                            ${s.options.map(o=>`<option value="${o[0]}" ${val===o[0]?'selected':''}>${o[1]}</option>`).join('')}
                        </select>
                    </div>`;
                }
            });
            el.innerHTML = html;
        }
        
        function toggleDisplayOn() {
            editingDisplayOn = !editingDisplayOn;
            renderSettingsEditor();
        }
        
        function toggleSetting(key) {
            editingSettings[key] = !editingSettings[key];
            renderSettingsEditor();
        }
        
        function selectSetting(key, val) {
            editingSettings[key] = parseInt(val);
        }
        
        function saveProfile() {
            const name = document.getElementById('profile-name').value.trim();
            if(!name){alert('Please enter a profile name');return;}
            
            // Rebuild bytes from settings
            rebuildBytes();
            
            const payload = {name: name, displayOn: editingDisplayOn, settings: editingSettings};
            fetch('/api/v1/profile', {
                method:'POST',
                headers:{'Content-Type':'application/json'},
                body:JSON.stringify(payload)
            }).then(r=>r.json()).then(d=>{
                if(d.success){
                    closeEditModal();
                    loadProfiles();
                } else {
                    alert('Error: '+(d.error||'Unknown'));
                }
            }).catch(e=>alert('Error: '+e));
        }
        
        function rebuildBytes() {
            // Delete bytes array so server uses individual settings to rebuild
            // The server-side jsonToSettings() will parse individual settings if no bytes array
            delete editingSettings.bytes;
        }
        
        function saveAndPush() {
            const name = document.getElementById('profile-name').value.trim();
            if(!name){alert('Please enter a profile name');return;}
            
            rebuildBytes();
            
            const payload = {name: name, displayOn: editingDisplayOn, settings: editingSettings};
            fetch('/api/v1/profile', {
                method:'POST',
                headers:{'Content-Type':'application/json'},
                body:JSON.stringify(payload)
            }).then(r=>r.json()).then(d=>{
                if(d.success){
                    // Now push to V1 - send settings, server will build bytes
                    return fetch('/api/v1/push', {
                        method:'POST',
                        headers:{'Content-Type':'application/json'},
                        body:JSON.stringify({settings: editingSettings, displayOn: editingDisplayOn})
                    });
                } else {
                    throw new Error(d.error||'Save failed');
                }
            }).then(r=>r.json()).then(d=>{
                if(d.success){
                    closeEditModal();
                    loadProfiles();
                    alert('Settings saved and pushed to V1!');
                } else {
                    alert('Saved but push failed: '+(d.error||'Unknown'));
                }
            }).catch(e=>alert('Error: '+e));
        }
        
        function pushSelectedProfile() {
            if(!selectedProfile) return;
            if(!confirm('Push "'+selectedProfile+'" settings to V1?')) return;
            
            fetch('/api/v1/profile?name='+encodeURIComponent(selectedProfile)).then(r=>r.json()).then(data=>{
                // Send settings, server will build bytes
                return fetch('/api/v1/push', {
                    method:'POST',
                    headers:{'Content-Type':'application/json'},
                    body:JSON.stringify({settings: data.settings, displayOn: data.displayOn})
                });
            }).then(r=>r.json()).then(d=>{
                if(d.success){
                    alert('Settings pushed to V1!');
                } else {
                    alert('Error: '+(d.error||'Unknown'));
                }
            }).catch(e=>alert('Error: '+e));
        }
        
        function deleteSelectedProfile() {
            if(!selectedProfile) return;
            if(!confirm('Delete "'+selectedProfile+'"?')) return;
            
            fetch('/api/v1/profile/delete', {
                method:'POST',
                headers:{'Content-Type':'application/json'},
                body:JSON.stringify({name: selectedProfile})
            }).then(r=>r.json()).then(d=>{
                if(d.success){
                    selectedProfile = null;
                    loadProfiles();
                } else {
                    alert('Error: '+(d.error||'Unknown'));
                }
            }).catch(e=>alert('Error: '+e));
        }
        
        // Init
        checkStatus();
        setInterval(checkStatus, 5000);
        loadProfiles();
        renderCurrentSettings();
    </script>
)HTML";
    return wrapWithLayout("V1 Settings & Profiles", body, "/v1settings");
}


// ============= Auto-Push Handlers =============

String WiFiManager::generateAutoPushSettingsJSON() {
    const V1Settings& s = settingsManager.get();
    String json = "{";
    json += "\"autoPushEnabled\":" + String(s.autoPushEnabled ? "true" : "false") + ",";
    json += "\"activeSlot\":" + String(s.activeSlot) + ",";
    json += "\"slot0_name\":\"" + htmlEscape(s.slot0Name) + "\",";
    json += "\"slot1_name\":\"" + htmlEscape(s.slot1Name) + "\",";
    json += "\"slot2_name\":\"" + htmlEscape(s.slot2Name) + "\",";
    json += "\"slot0_color\":" + String(s.slot0Color) + ",";
    json += "\"slot1_color\":" + String(s.slot1Color) + ",";
    json += "\"slot2_color\":" + String(s.slot2Color) + ",";
    json += "\"slot0_volume\":" + String(s.slot0Volume) + ",";
    json += "\"slot1_volume\":" + String(s.slot1Volume) + ",";
    json += "\"slot2_volume\":" + String(s.slot2Volume) + ",";
    json += "\"slot0_muteVol\":" + String(s.slot0MuteVolume) + ",";
    json += "\"slot1_muteVol\":" + String(s.slot1MuteVolume) + ",";
    json += "\"slot2_muteVol\":" + String(s.slot2MuteVolume) + ",";
    json += "\"slot0_profile\":\"" + htmlEscape(s.slot0_default.profileName) + "\",";
    json += "\"slot0_mode\":" + String((int)s.slot0_default.mode) + ",";
    json += "\"slot1_profile\":\"" + htmlEscape(s.slot1_highway.profileName) + "\",";
    json += "\"slot1_mode\":" + String((int)s.slot1_highway.mode) + ",";
    json += "\"slot2_profile\":\"" + htmlEscape(s.slot2_comfort.profileName) + "\",";
    json += "\"slot2_mode\":" + String((int)s.slot2_comfort.mode);
    json += "}";
    return json;
}

void WiFiManager::handleAutoPush() {
    SerialLog.println("[HTTP] GET /autopush");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Stream HTML in chunks
    streamLayoutHeader("Auto-Push Profiles", "/autopush");
    streamAutoPushBody();
    streamLayoutFooter();
}

void WiFiManager::handleAutoPushSlotSave() {
    if (!server.hasArg("slot") || !server.hasArg("profile") || !server.hasArg("mode")) {
        server.send(400, "application/json", "{\"error\":\"Missing parameters\"}");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    String profile = server.arg("profile");
    int mode = server.arg("mode").toInt();
    String name = server.hasArg("name") ? server.arg("name") : "";
    int color = server.hasArg("color") ? server.arg("color").toInt() : -1;
    int volume = server.hasArg("volume") ? server.arg("volume").toInt() : -1;
    int muteVol = server.hasArg("muteVol") ? server.arg("muteVol").toInt() : -1;
    
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }
    
    // Save slot name if provided (limited to 20 chars by setSlotName)
    if (name.length() > 0) {
        settingsManager.setSlotName(slot, name);
    }
    
    // Save slot color if provided
    if (color >= 0) {
        settingsManager.setSlotColor(slot, static_cast<uint16_t>(color));
    }
    
    // Save slot volumes - preserve existing values if not provided
    uint8_t existingVol = settingsManager.getSlotVolume(slot);
    uint8_t existingMute = settingsManager.getSlotMuteVolume(slot);
    uint8_t vol = (volume >= 0) ? static_cast<uint8_t>(volume) : existingVol;
    uint8_t mute = (muteVol >= 0) ? static_cast<uint8_t>(muteVol) : existingMute;
    
    SerialLog.printf("[SaveSlot] Slot %d - volume: %d (was %d), muteVol: %d (was %d)\n", 
                  slot, vol, existingVol, mute, existingMute);
    
    settingsManager.setSlotVolumes(slot, vol, mute);
    
    settingsManager.setSlot(slot, profile, static_cast<V1Mode>(mode));
    
    // If this is the currently active slot, update the display immediately
    if (slot == settingsManager.get().activeSlot) {
        display.drawProfileIndicator(slot);
    }
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleAutoPushActivate() {
    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    bool enable = server.hasArg("enable") ? (server.arg("enable") == "true") : true;
    
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }
    
    settingsManager.setActiveSlot(slot);
    settingsManager.setAutoPushEnabled(enable);
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleAutoPushPushNow() {
    if (!server.hasArg("slot")) {
        server.send(400, "application/json", "{\"error\":\"Missing slot parameter\"}");
        return;
    }
    
    int slot = server.arg("slot").toInt();
    if (slot < 0 || slot > 2) {
        server.send(400, "application/json", "{\"error\":\"Invalid slot\"}");
        return;
    }
    
    // Check if profile/mode are passed directly (from Push Now button)
    String profileName;
    V1Mode mode = V1_MODE_UNKNOWN;
    
    if (server.hasArg("profile") && server.arg("profile").length() > 0) {
        // Use the form values directly
        profileName = server.arg("profile");
        if (server.hasArg("mode")) {
            mode = static_cast<V1Mode>(server.arg("mode").toInt());
        }
    } else {
        // Fall back to saved slot settings
        const V1Settings& s = settingsManager.get();
        AutoPushSlot pushSlot;
        
        switch (slot) {
            case 0: pushSlot = s.slot0_default; break;
            case 1: pushSlot = s.slot1_highway; break;
            case 2: pushSlot = s.slot2_comfort; break;
        }
        
        profileName = pushSlot.profileName;
        mode = pushSlot.mode;
    }
    
    if (profileName.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"No profile configured for this slot\"}");
        return;
    }
    
    // Load and push the profile
    V1Profile profile;
    if (!v1ProfileManager.loadProfile(profileName, profile)) {
        server.send(500, "application/json", "{\"error\":\"Failed to load profile\"}");
        return;
    }
    
    if (!bleClient.writeUserBytes(profile.settings.bytes)) {
        server.send(500, "application/json", "{\"error\":\"Failed to push settings\"}");
        return;
    }
    
    bleClient.setDisplayOn(profile.displayOn);
    
    if (mode != V1_MODE_UNKNOWN) {
        bleClient.setMode(static_cast<uint8_t>(mode));
    }
    
    // Set volumes if configured (not 0xFF = no change)
    uint8_t mainVol = settingsManager.getSlotVolume(slot);
    uint8_t muteVol = settingsManager.getSlotMuteVolume(slot);
    
    SerialLog.printf("[PushNow] Slot %d volumes - main: %d, mute: %d\n", slot, mainVol, muteVol);
    
    if (mainVol != 0xFF || muteVol != 0xFF) {
        delay(100);
        SerialLog.printf("[PushNow] Setting volume - main: %d, muted: %d\n", mainVol, muteVol);
        bleClient.setVolume(mainVol, muteVol);
    } else {
        SerialLog.println("[PushNow] Volume: No change");
    }
    
    // Update active slot and refresh display profile indicator
    settingsManager.setActiveSlot(slot);
    display.drawProfileIndicator(slot);
    
    server.send(200, "application/json", "{\"success\":true}");
}

String WiFiManager::generateAutoPushHTML() {
    const V1Settings& s = settingsManager.get();
    const V1Settings& settings = settingsManager.get();
    
    String body = R"HTML(
    <p class="muted" style="text-align:center; margin-bottom:20px;">Configure 3 quick-access profiles for different driving scenarios</p>
    
    <div id="message"></div>
    
    <div class="card">
        <h2>Quick Push</h2>
        <p class="muted">Click to activate and push a profile immediately</p>
        <div class="quick-push">
            <div class="quick-btn" id="quick-0" onclick="quickPush(0)">
                <div class="quick-btn-icon">1</div>
                <div class="quick-btn-label">)HTML" + htmlEscape(s.slot0Name) + R"HTML(</div>
                <div class="quick-btn-sub" id="quick-sub-0">Not configured</div>
            </div>
            <div class="quick-btn" id="quick-1" onclick="quickPush(1)">
                <div class="quick-btn-icon">2</div>
                <div class="quick-btn-label">)HTML" + htmlEscape(s.slot1Name) + R"HTML(</div>
                <div class="quick-btn-sub" id="quick-sub-1">Not configured</div>
            </div>
            <div class="quick-btn" id="quick-2" onclick="quickPush(2)">
                <div class="quick-btn-icon">3</div>
                <div class="quick-btn-label">)HTML" + htmlEscape(s.slot2Name) + R"HTML(</div>
                <div class="quick-btn-sub" id="quick-sub-2">Not configured</div>
            </div>
        </div>
        <div class="setting-row">
            <span>Auto-push on V1 connect</span>
            <div class="toggle )HTML" + String(s.autoPushEnabled ? "on" : "") + R"HTML(" id="auto-toggle" onclick="toggleAutoPush()"></div>
        </div>
    </div>
    
    <div class="card slot-card" id="slot-0">
        <div class="slot-header">
            <h2>Slot 1: Default</h2>
            <span class="status-badge" id="badge-0" style="display:none;">ACTIVE</span>
        </div>
        <div class="form-group">
            <label>Display Name</label>
            <input type="text" id="name-0" maxlength="20" placeholder="DEFAULT">
        </div>
        <div class="form-group">
            <label>Display Color</label>
            <input type="color" id="color-0" value="#400050">
        </div>
        <div class="form-group">
            <label>Profile</label>
            <select id="profile-0"><option value="">-- None --</option></select>
        </div>
        <div class="form-group">
            <label>Mode</label>
            <select id="mode-0">
                <option value="0">No Change</option>
                <option value="1">All Bogeys</option>
                <option value="2">Logic</option>
                <option value="3">Advanced Logic</option>
            </select>
        </div>
        <div class="form-group">
            <label>V1 Volume</label>
            <select id="volume-0">
                <option value="255">No Change</option>
                <option value="0">0 (Off)</option>
                <option value="1">1</option><option value="2">2</option><option value="3">3</option>
                <option value="4">4</option><option value="5">5</option><option value="6">6</option>
                <option value="7">7</option><option value="8">8</option><option value="9">9 (Max)</option>
            </select>
        </div>
        <div class="form-group">
            <label>Mute Volume</label>
            <select id="muteVol-0">
                <option value="255">No Change</option>
                <option value="0">0 (Silent)</option>
                <option value="1">1</option><option value="2">2</option><option value="3">3</option>
                <option value="4">4</option><option value="5">5</option><option value="6">6</option>
                <option value="7">7</option><option value="8">8</option><option value="9">9 (Max)</option>
            </select>
        </div>
        <div class="actions">
            <button class="btn primary" onclick="saveSlot(0)">Save</button>
            <button class="btn success" onclick="pushSlot(0)">Push Now</button>
        </div>
    </div>
    
    <div class="card slot-card" id="slot-1">
        <div class="slot-header">
            <h2>Slot 2: Highway</h2>
            <span class="status-badge" id="badge-1" style="display:none;">ACTIVE</span>
        </div>
        <div class="form-group">
            <label>Display Name</label>
            <input type="text" id="name-1" maxlength="20" placeholder="HIGHWAY">
        </div>
        <div class="form-group">
            <label>Display Color</label>
            <input type="color" id="color-1" value="#00fc00">
        </div>
        <div class="form-group">
            <label>Profile</label>
            <select id="profile-1"><option value="">-- None --</option></select>
        </div>
        <div class="form-group">
            <label>Mode</label>
            <select id="mode-1">
                <option value="0">No Change</option>
                <option value="1">All Bogeys</option>
                <option value="2">Logic</option>
                <option value="3">Advanced Logic</option>
            </select>
        </div>
        <div class="form-group">
            <label>V1 Volume</label>
            <select id="volume-1">
                <option value="255">No Change</option>
                <option value="0">0 (Off)</option>
                <option value="1">1</option><option value="2">2</option><option value="3">3</option>
                <option value="4">4</option><option value="5">5</option><option value="6">6</option>
                <option value="7">7</option><option value="8">8</option><option value="9">9 (Max)</option>
            </select>
        </div>
        <div class="form-group">
            <label>Mute Volume</label>
            <select id="muteVol-1">
                <option value="255">No Change</option>
                <option value="0">0 (Silent)</option>
                <option value="1">1</option><option value="2">2</option><option value="3">3</option>
                <option value="4">4</option><option value="5">5</option><option value="6">6</option>
                <option value="7">7</option><option value="8">8</option><option value="9">9 (Max)</option>
            </select>
        </div>
        <div class="actions">
            <button class="btn primary" onclick="saveSlot(1)">Save</button>
            <button class="btn success" onclick="pushSlot(1)">Push Now</button>
        </div>
    </div>
    
    <div class="card slot-card" id="slot-2">
        <div class="slot-header">
            <h2>Slot 3: Comfort</h2>
            <span class="status-badge" id="badge-2" style="display:none;">ACTIVE</span>
        </div>
        <div class="form-group">
            <label>Display Name</label>
            <input type="text" id="name-2" maxlength="20" placeholder="COMFORT">
        </div>
        <div class="form-group">
            <label>Display Color</label>
            <input type="color" id="color-2" value="#808080">
        </div>
        <div class="form-group">
            <label>Profile</label>
            <select id="profile-2"><option value="">-- None --</option></select>
        </div>
        <div class="form-group">
            <label>Mode</label>
            <select id="mode-2">
                <option value="0">No Change</option>
                <option value="1">All Bogeys</option>
                <option value="2">Logic</option>
                <option value="3">Advanced Logic</option>
            </select>
        </div>
        <div class="form-group">
            <label>V1 Volume</label>
            <select id="volume-2">
                <option value="255">No Change</option>
                <option value="0">0 (Off)</option>
                <option value="1">1</option><option value="2">2</option><option value="3">3</option>
                <option value="4">4</option><option value="5">5</option><option value="6">6</option>
                <option value="7">7</option><option value="8">8</option><option value="9">9 (Max)</option>
            </select>
        </div>
        <div class="form-group">
            <label>Mute Volume</label>
            <select id="muteVol-2">
                <option value="255">No Change</option>
                <option value="0">0 (Silent)</option>
                <option value="1">1</option><option value="2">2</option><option value="3">3</option>
                <option value="4">4</option><option value="5">5</option><option value="6">6</option>
                <option value="7">7</option><option value="8">8</option><option value="9">9 (Max)</option>
            </select>
        </div>
        <div class="actions">
            <button class="btn primary" onclick="saveSlot(2)">Save</button>
            <button class="btn success" onclick="pushSlot(2)">Push Now</button>
        </div>
    </div>
    
    <script>
        const settings = )HTML" + generateAutoPushSettingsJSON() + R"HTML(;
        
        function rgb565ToHex(rgb565) {
            const r = ((rgb565 >> 11) & 0x1F) << 3;
            const g = ((rgb565 >> 5) & 0x3F) << 2;
            const b = (rgb565 & 0x1F) << 3;
            return '#' + [r,g,b].map(x => x.toString(16).padStart(2,'0')).join('');
        }
        
        function hexToRgb565(hex) {
            const r = parseInt(hex.substr(1,2), 16) >> 3;
            const g = parseInt(hex.substr(3,2), 16) >> 2;
            const b = parseInt(hex.substr(5,2), 16) >> 3;
            return (r << 11) | (g << 5) | b;
        }
        
        function showMessage(msg, isError) {
            const el = document.getElementById('message');
            el.innerHTML = '<div class="msg '+(isError?'error':'success')+'">'+msg+'</div>';
            setTimeout(() => el.innerHTML = '', 3000);
        }
        
        function loadProfiles() {
            fetch('/api/v1/profiles').then(r=>r.json()).then(profiles => {
                for (let i = 0; i < 3; i++) {
                    const sel = document.getElementById('profile-'+i);
                    sel.innerHTML = '<option value="">-- None --</option>';
                    profiles.forEach(p => {
                        const opt = document.createElement('option');
                        opt.value = p;
                        opt.textContent = p;
                        sel.appendChild(opt);
                    });
                }
                loadSettings();
            });
        }
        
        function loadSettings() {
            document.getElementById('name-0').value = settings.slot0_name || 'DEFAULT';
            document.getElementById('name-1').value = settings.slot1_name || 'HIGHWAY';
            document.getElementById('name-2').value = settings.slot2_name || 'COMFORT';
            
            document.getElementById('color-0').value = rgb565ToHex(settings.slot0_color || 0x400A);
            document.getElementById('color-1').value = rgb565ToHex(settings.slot1_color || 0x07E0);
            document.getElementById('color-2').value = rgb565ToHex(settings.slot2_color || 0x8410);
            
            document.getElementById('profile-0').value = settings.slot0_profile || '';
            document.getElementById('mode-0').value = settings.slot0_mode || 0;
            document.getElementById('volume-0').value = settings.slot0_volume !== undefined ? settings.slot0_volume : 255;
            document.getElementById('muteVol-0').value = settings.slot0_muteVol !== undefined ? settings.slot0_muteVol : 255;
            document.getElementById('profile-1').value = settings.slot1_profile || '';
            document.getElementById('mode-1').value = settings.slot1_mode || 0;
            document.getElementById('volume-1').value = settings.slot1_volume !== undefined ? settings.slot1_volume : 255;
            document.getElementById('muteVol-1').value = settings.slot1_muteVol !== undefined ? settings.slot1_muteVol : 255;
            document.getElementById('profile-2').value = settings.slot2_profile || '';
            document.getElementById('mode-2').value = settings.slot2_mode || 0;
            document.getElementById('volume-2').value = settings.slot2_volume !== undefined ? settings.slot2_volume : 255;
            document.getElementById('muteVol-2').value = settings.slot2_muteVol !== undefined ? settings.slot2_muteVol : 255;
            
            updateActiveDisplay();
            updateQuickButtons();
        }
        
        function updateActiveDisplay() {
            for (let i = 0; i < 3; i++) {
                const card = document.getElementById('slot-'+i);
                const badge = document.getElementById('badge-'+i);
                if (i === settings.activeSlot) {
                    card.classList.add('active');
                    badge.style.display = 'inline';
                } else {
                    card.classList.remove('active');
                    badge.style.display = 'none';
                }
            }
        }
        
        function updateQuickButtons() {
            const slots = [
                {name: settings.slot0_name, prof: settings.slot0_profile},
                {name: settings.slot1_name, prof: settings.slot1_profile},
                {name: settings.slot2_name, prof: settings.slot2_profile}
            ];
            const defaultNames = ['DEFAULT', 'HIGHWAY', 'COMFORT'];
            
            for (let i = 0; i < 3; i++) {
                const btn = document.getElementById('quick-'+i);
                const label = btn.querySelector('.quick-btn-label');
                const sub = document.getElementById('quick-sub-'+i);
                
                if (label) label.textContent = slots[i].name || defaultNames[i];
                
                if (slots[i].prof) {
                    sub.textContent = slots[i].prof;
                    btn.style.opacity = '1';
                } else {
                    sub.textContent = 'Not configured';
                    btn.style.opacity = '0.6';
                }
                
                btn.classList.toggle('active', i === settings.activeSlot);
            }
        }
        
        function toggleAutoPush() {
            settings.autoPushEnabled = !settings.autoPushEnabled;
            document.getElementById('auto-toggle').classList.toggle('on', settings.autoPushEnabled);
            
            const data = new URLSearchParams();
            data.append('slot', settings.activeSlot);
            data.append('enable', settings.autoPushEnabled ? 'true' : 'false');
            
            fetch('/api/autopush/activate', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: data
            }).then(r=>r.json()).then(d => {
                if (d.success) {
                    showMessage('Auto-push '+(settings.autoPushEnabled?'enabled':'disabled'), false);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        function saveSlot(slot) {
            const name = document.getElementById('name-'+slot).value.toUpperCase().substring(0, 20);
            const colorHex = document.getElementById('color-'+slot).value;
            const color = hexToRgb565(colorHex);
            const profile = document.getElementById('profile-'+slot).value;
            const mode = document.getElementById('mode-'+slot).value;
            const volume = document.getElementById('volume-'+slot).value;
            const muteVol = document.getElementById('muteVol-'+slot).value;
            
            const data = new URLSearchParams();
            data.append('slot', slot);
            data.append('name', name);
            data.append('color', color);
            data.append('profile', profile);
            data.append('mode', mode);
            data.append('volume', volume);
            data.append('muteVol', muteVol);
            
            fetch('/api/autopush/slot', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: data
            }).then(r=>r.json()).then(d => {
                if (d.success) {
                    showMessage('Slot ' + (slot+1) + ' saved!', false);
                    if (slot === 0) {
                        settings.slot0_name = name; settings.slot0_color = color;
                        settings.slot0_profile = profile; settings.slot0_mode = parseInt(mode);
                    } else if (slot === 1) {
                        settings.slot1_name = name; settings.slot1_color = color;
                        settings.slot1_profile = profile; settings.slot1_mode = parseInt(mode);
                    } else {
                        settings.slot2_name = name; settings.slot2_color = color;
                        settings.slot2_profile = profile; settings.slot2_mode = parseInt(mode);
                    }
                    updateQuickButtons();
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        function pushSlot(slot) {
            const profile = document.getElementById('profile-'+slot).value;
            const mode = document.getElementById('mode-'+slot).value;
            
            const data = new URLSearchParams();
            data.append('slot', slot);
            data.append('profile', profile);
            data.append('mode', mode);
            
            fetch('/api/autopush/push', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: data
            }).then(r=>r.json()).then(d => {
                if (d.success) showMessage('Profile pushed to V1!', false);
                else showMessage('Error: '+(d.error||'Unknown'), true);
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        function quickPush(slot) {
            const data = new URLSearchParams();
            data.append('slot', slot);
            data.append('enable', 'true');
            
            fetch('/api/autopush/activate', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: data
            }).then(r=>r.json()).then(d => {
                if (d.success) {
                    settings.activeSlot = slot;
                    settings.autoPushEnabled = true;
                    document.getElementById('auto-toggle').classList.add('on');
                    updateActiveDisplay();
                    updateQuickButtons();
                    pushSlot(slot);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        loadProfiles();
    </script>
)HTML";
    return wrapWithLayout("Auto-Push Profiles", body, "/autopush");
}
// ============= Display Colors Handlers =============

void WiFiManager::handleDisplayColors() {
    SerialLog.println("[HTTP] GET /displaycolors");
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    
    // Stream HTML in chunks
    streamLayoutHeader("Display Colors", "/displaycolors");
    streamDisplayColorsBody();
    streamLayoutFooter();
}

void WiFiManager::handleDisplayColorsSave() {
    uint16_t bogey = server.hasArg("bogey") ? server.arg("bogey").toInt() : 0xF800;
    uint16_t freq = server.hasArg("freq") ? server.arg("freq").toInt() : 0xF800;
    uint16_t arrow = server.hasArg("arrow") ? server.arg("arrow").toInt() : 0xF800;
    uint16_t bandL = server.hasArg("bandL") ? server.arg("bandL").toInt() : 0x001F;
    uint16_t bandKa = server.hasArg("bandKa") ? server.arg("bandKa").toInt() : 0xF800;
    uint16_t bandK = server.hasArg("bandK") ? server.arg("bandK").toInt() : 0x001F;
    uint16_t bandX = server.hasArg("bandX") ? server.arg("bandX").toInt() : 0x07E0;
    
    settingsManager.setDisplayColors(bogey, freq, arrow, bandL, bandKa, bandK, bandX);
    
    server.send(200, "application/json", "{\"success\":true}");
}

void WiFiManager::handleDisplayColorsReset() {
    // Reset to default colors: Bogey/Freq/Arrow=Red, L/K=Blue, Ka=Red, X=Green
    settingsManager.setDisplayColors(0xF800, 0xF800, 0xF800, 0x001F, 0xF800, 0x001F, 0x07E0);
    server.send(200, "application/json", "{\"success\":true}");
}

String WiFiManager::generateDisplayColorsHTML() {
    const V1Settings& s = settingsManager.get();
    
    String body = R"HTML(
    <p class="muted" style="text-align:center;margin-bottom:20px;">Customize the colors shown on your V1 display</p>
    
    <div id="message"></div>
    
    <div class="card">
        <h2>Counter & Frequency</h2>
        <div class="color-grid">
            <div class="color-item">
                <label>Bogey Counter</label>
                <div class="color-row">
                    <input type="color" id="color-bogey" onchange="updatePreview('bogey')">
                    <div class="color-preview" id="preview-bogey">1.</div>
                </div>
            </div>
            <div class="color-item">
                <label>Frequency Display</label>
                <div class="color-row">
                    <input type="color" id="color-freq" onchange="updatePreview('freq')">
                    <div class="color-preview" id="preview-freq">35.5</div>
                </div>
            </div>
        </div>
    </div>
    
    <div class="card">
        <h2>Band Indicators</h2>
        <div class="color-grid">
            <div class="color-item">
                <label>Laser (L)</label>
                <div class="color-row">
                    <input type="color" id="color-bandL" onchange="updatePreview('bandL')">
                    <div class="color-preview" id="preview-bandL">L</div>
                </div>
            </div>
            <div class="color-item">
                <label>Ka Band</label>
                <div class="color-row">
                    <input type="color" id="color-bandKa" onchange="updatePreview('bandKa')">
                    <div class="color-preview" id="preview-bandKa">Ka</div>
                </div>
            </div>
            <div class="color-item">
                <label>K Band</label>
                <div class="color-row">
                    <input type="color" id="color-bandK" onchange="updatePreview('bandK')">
                    <div class="color-preview" id="preview-bandK">K</div>
                </div>
            </div>
            <div class="color-item">
                <label>X Band</label>
                <div class="color-row">
                    <input type="color" id="color-bandX" onchange="updatePreview('bandX')">
                    <div class="color-preview" id="preview-bandX">X</div>
                </div>
            </div>
        </div>
    </div>
    
    <div class="card">
        <h2>Direction Arrows</h2>
        <div class="color-item">
            <label>Arrow Color</label>
            <div class="color-row">
                <input type="color" id="color-arrow" onchange="updatePreview('arrow')">
                <div class="color-preview" id="preview-arrow">▲▼</div>
            </div>
        </div>
    </div>
    
    <button class="btn primary" onclick="saveColors()">Save Colors</button>
    <button class="btn secondary" onclick="resetDefaults()">Reset to Defaults</button>
    
    <script>
        const colors = {
            bogey: )HTML" + String(s.colorBogey) + R"HTML(,
            freq: )HTML" + String(s.colorFrequency) + R"HTML(,
            arrow: )HTML" + String(s.colorArrow) + R"HTML(,
            bandL: )HTML" + String(s.colorBandL) + R"HTML(,
            bandKa: )HTML" + String(s.colorBandKa) + R"HTML(,
            bandK: )HTML" + String(s.colorBandK) + R"HTML(,
            bandX: )HTML" + String(s.colorBandX) + R"HTML(
        };
        
        function rgb565ToHex(rgb565) {
            const r = ((rgb565 >> 11) & 0x1F) << 3;
            const g = ((rgb565 >> 5) & 0x3F) << 2;
            const b = (rgb565 & 0x1F) << 3;
            return '#' + [r,g,b].map(x => x.toString(16).padStart(2,'0')).join('');
        }
        
        function hexToRgb565(hex) {
            const r = parseInt(hex.substr(1,2), 16) >> 3;
            const g = parseInt(hex.substr(3,2), 16) >> 2;
            const b = parseInt(hex.substr(5,2), 16) >> 3;
            return (r << 11) | (g << 5) | b;
        }
        
        function updatePreview(id) {
            const input = document.getElementById('color-' + id);
            const preview = document.getElementById('preview-' + id);
            preview.style.color = input.value;
            colors[id] = hexToRgb565(input.value);
        }
        
        function loadColors() {
            for (const [key, value] of Object.entries(colors)) {
                const hex = rgb565ToHex(value);
                document.getElementById('color-' + key).value = hex;
                document.getElementById('preview-' + key).style.color = hex;
            }
        }
        
        function showMessage(msg, isError) {
            const el = document.getElementById('message');
            el.innerHTML = '<div class="msg '+(isError?'error':'success')+'">'+msg+'</div>';
            setTimeout(() => el.innerHTML = '', 3000);
        }
        
        function saveColors() {
            const data = new URLSearchParams();
            data.append('bogey', colors.bogey);
            data.append('freq', colors.freq);
            data.append('arrow', colors.arrow);
            data.append('bandL', colors.bandL);
            data.append('bandKa', colors.bandKa);
            data.append('bandK', colors.bandK);
            data.append('bandX', colors.bandX);
            
            fetch('/api/displaycolors', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: data
            }).then(r=>r.json()).then(d => {
                if (d.success) showMessage('Colors saved! Display will update on next alert.', false);
                else showMessage('Error: '+(d.error||'Unknown'), true);
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        function resetDefaults() {
            if (!confirm('Reset all display colors to defaults?')) return;
            fetch('/api/displaycolors/reset', {method:'POST'})
            .then(r=>r.json()).then(d => {
                if (d.success) {
                    colors.bogey = 0xF800; colors.freq = 0xF800; colors.arrow = 0xF800;
                    colors.bandL = 0x001F; colors.bandKa = 0xF800;
                    colors.bandK = 0x001F; colors.bandX = 0x07E0;
                    loadColors();
                    showMessage('Colors reset to defaults!', false);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        loadColors();
    </script>
)HTML";
    return wrapWithLayout("Display Colors", body, "/displaycolors");
}

String WiFiManager::generateTimeSettingsHTML() {
    const V1Settings& settings = settingsManager.get();
    String currentTime = timeManager.isTimeValid() ? timeManager.getTimestampISO() : "Not Set";
    String checkedTime = settings.enableTimesync ? "checked" : "";
    
    String body;
    
    // Current Time Card
    body += "<div class=\"card\">";
    body += "<h2>Current Time (UTC)</h2>";
    body += "<div class=\"setting-row\"><span class=\"muted\">System Time</span><span style=\"font-family:monospace;\">" + currentTime + "</span></div>";
    body += "<p class=\"muted\">All times displayed and stored in UTC. Configure NTP sync or set manually for accurate timestamps.</p>";
    body += "</div>";
    
    // WiFi Networks Form
    body += "<form action=\"/time\" method=\"POST\">";
    body += "<div class=\"card\">";
    body += "<h2>WiFi Networks for Internet/NTP</h2>";
    body += "<p class=\"muted\">Add up to 3 WiFi networks. The device will automatically connect to whichever is available with the strongest signal.</p>";
    
    body += "<div class=\"setting-row\"><span>Enable WiFi STA mode (for NTP sync &amp; NAT routing)</span>";
    body += "<div class=\"toggle " + String(settings.enableTimesync ? "on" : "") + "\" onclick=\"this.classList.toggle('on');document.getElementById('enableTimesync').checked=this.classList.contains('on');\"></div></div>";
    body += "<input type=\"checkbox\" id=\"enableTimesync\" name=\"enableTimesync\" " + checkedTime + " style=\"display:none;\">";
    
    // WiFi Network 1
    body += "<div class=\"form-group\"><label>WiFi Network 1 - SSID</label>";
    body += "<input type=\"text\" name=\"wifi0ssid\" value=\"" + htmlEscape(settings.wifiNetworks[0].ssid) + "\" placeholder=\"e.g., Home WiFi\"></div>";
    body += "<div class=\"form-group\"><label>Password</label>";
    body += "<input type=\"password\" name=\"wifi0pwd\" value=\"" + (settings.wifiNetworks[0].password.length() > 0 ? String("********") : String("")) + "\" placeholder=\"Password\"></div>";
    
    // WiFi Network 2
    body += "<div class=\"form-group\"><label>WiFi Network 2 - SSID</label>";
    body += "<input type=\"text\" name=\"wifi1ssid\" value=\"" + htmlEscape(settings.wifiNetworks[1].ssid) + "\" placeholder=\"e.g., Car WiFi Hotspot\"></div>";
    body += "<div class=\"form-group\"><label>Password</label>";
    body += "<input type=\"password\" name=\"wifi1pwd\" value=\"" + (settings.wifiNetworks[1].password.length() > 0 ? String("********") : String("")) + "\" placeholder=\"Password\"></div>";
    
    // WiFi Network 3
    body += "<div class=\"form-group\"><label>WiFi Network 3 - SSID</label>";
    body += "<input type=\"text\" name=\"wifi2ssid\" value=\"" + htmlEscape(settings.wifiNetworks[2].ssid) + "\" placeholder=\"e.g., Phone Hotspot\"></div>";
    body += "<div class=\"form-group\"><label>Password</label>";
    body += "<input type=\"password\" name=\"wifi2pwd\" value=\"" + (settings.wifiNetworks[2].password.length() > 0 ? String("********") : String("")) + "\" placeholder=\"Password\"></div>";
    
    body += "<button type=\"submit\" class=\"btn primary\">Save WiFi Settings</button>";
    body += "</div></form>";
    
    // Manual Time Setting Card
    body += "<div class=\"card\">";
    body += "<h2>Manual Time Setting</h2>";
    body += "<p class=\"muted\">Set time manually if you don't want to connect to WiFi. Your device will use this time.</p>";
    body += "<button class=\"btn secondary\" onclick=\"setTimeNow()\">Set Time from This Device</button>";
    body += "</div>";
    
    body += R"HTML(
    <script>
        function setTimeNow() {
            const now = Math.floor(Date.now() / 1000);
            const form = document.createElement('form');
            form.method = 'POST';
            form.action = '/time';
            const input = document.createElement('input');
            input.type = 'hidden';
            input.name = 'timestamp';
            input.value = now;
            form.appendChild(input);
            document.body.appendChild(form);
            form.submit();
        }
    </script>
)HTML";
    
    return wrapWithLayout("Time Settings", body, "/time");
}

// ============================
// Streaming HTML Implementations
// ============================

void WiFiManager::streamLayoutHeader(const String& title, const String& activePath) {
    server.sendContent_P(PSTR("<!DOCTYPE html>\n<html>\n<head>\n"));
    server.sendContent_P(PSTR("  <meta charset=\"UTF-8\">\n"));
    server.sendContent_P(PSTR("  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"));
    server.sendContent("  <title>" + htmlEscape(title) + "</title>\n");
    streamStyleSheet();
    server.sendContent_P(PSTR("</head>\n<body>\n"));
    streamTopNav(activePath);
    server.sendContent(String("<h1 class=\"page-title\">") + htmlEscape(title) + "</h1>\n");
    server.sendContent_P(PSTR("<div class=\"container\">\n"));
}

void WiFiManager::streamLayoutFooter() {
    server.sendContent_P(PSTR("</div>\n</body>\n</html>"));
}

void WiFiManager::streamStyleSheet() {
    // Use existing generateStyleSheet() but stream the result
    String css = generateStyleSheet();
    server.sendContent(css);
}

void WiFiManager::streamTopNav(const String& activePath) {
    // Use existing generateTopNav() but stream the result
    String nav = generateTopNav(activePath);
    server.sendContent(nav);
}

void WiFiManager::streamSettingsBody() {
    // Generate settings HTML (unwrapped body only)
    const V1Settings& settings = settingsManager.get();
    String apSsidEsc = htmlEscape(settings.apSSID);
    String apPassEsc = settings.apPassword.length() > 0 ? "********" : "";
    String staSsidEsc = htmlEscape(settings.ssid);
    String staPassEsc = settings.password.length() > 0 ? "********" : "";

    // Success message if present
    if (server.hasArg("saved")) {
        if (server.hasArg("wifi")) {
            server.sendContent_P(PSTR("<div class='msg success'>Settings saved! Restart to apply WiFi changes.</div>"));
        } else {
            server.sendContent_P(PSTR("<div class='msg success'>Settings saved!</div>"));
        }
    }

    // Time Settings Card
    server.sendContent_P(PSTR("<div class=\"card\"><h2>Time Settings</h2>"));
    server.sendContent_P(PSTR("<p class=\"muted\">Configure automatic time sync via NTP or set time manually for accurate timestamps.</p>"));
    
    if (timeManager.isTimeValid()) {
        String timeStr = timeManager.getTimestampISO();
        const V1Settings& s = settingsManager.get();
        String syncStatus = s.enableTimesync ? "NTP Sync Enabled" : "Manual Time";
        server.sendContent("<div class=\"setting-row\"><span class=\"muted\">" + syncStatus + "</span><span>" + timeStr + " UTC</span></div>");
    } else {
        server.sendContent_P(PSTR("<div class=\"msg error\">Time Not Set</div>"));
    }
    server.sendContent_P(PSTR("<a class=\"btn primary\" href=\"/time\">Open Time Settings</a></div>"));

    // V1 Settings Card
    server.sendContent_P(PSTR("<div class=\"card\"><h2>V1 Settings</h2>"));
    server.sendContent_P(PSTR("<p class=\"muted\">Pull, edit, and push V1 user settings. Save profiles for quick switching.</p>"));
    server.sendContent_P(PSTR("<a class=\"btn primary\" href=\"/v1settings\">Open V1 Settings</a></div>"));

    // Alert Logs Card  
    server.sendContent_P(PSTR("<div class=\"card\"><h2>Alert Logs</h2>"));
    server.sendContent_P(PSTR("<p class=\"muted\">Alerts are recorded to the SD card. View and clear them from the log page.</p>"));
    server.sendContent_P(PSTR("<a class=\"btn primary\" href=\"/logs\">Open Alert Log</a></div>"));

    // WiFi Settings Form
    server.sendContent_P(PSTR("<form method=\"POST\" action=\"/settings\">"));
    server.sendContent_P(PSTR("<div class=\"card\"><h2>WiFi Mode & Access Point</h2>"));
    
    server.sendContent_P(PSTR("<div class=\"form-group\"><label>WiFi Mode</label>"));
    server.sendContent_P(PSTR("<select name=\"wifi_mode\">"));
    server.sendContent(String("<option value=\"2\"") + (settings.wifiMode == V1_WIFI_AP ? " selected" : "") + ">Access Point Only</option>");
    server.sendContent(String("<option value=\"1\"") + (settings.wifiMode == V1_WIFI_STA ? " selected" : "") + ">Station Only</option>");
    server.sendContent(String("<option value=\"3\"") + (settings.wifiMode == V1_WIFI_APSTA ? " selected" : "") + ">AP + Station (NAT passthrough)</option>");
    server.sendContent_P(PSTR("</select></div>"));

    server.sendContent_P(PSTR("<div class=\"form-group\"><label>AP Network Name</label>"));
    server.sendContent("<input type=\"text\" name=\"ap_ssid\" value=\"" + apSsidEsc + "\"></div>");

    server.sendContent_P(PSTR("<div class=\"form-group\"><label>AP Password (min 8 chars)</label>"));
    server.sendContent("<input type=\"password\" name=\"ap_password\" value=\"" + apPassEsc + "\"></div>");

    server.sendContent_P(PSTR("<div class=\"form-group\"><label>Upstream WiFi (STA) SSID</label>"));
    server.sendContent("<input type=\"text\" name=\"ssid\" value=\"" + staSsidEsc + "\" placeholder=\"Home/Hotspot SSID\"></div>");

    server.sendContent_P(PSTR("<div class=\"form-group\"><label>Upstream WiFi Password</label>"));
    server.sendContent("<input type=\"password\" name=\"password\" value=\"" + staPassEsc + "\" placeholder=\"Password\"></div>");
    server.sendContent_P(PSTR("</div>"));

    // Display Settings Card
    server.sendContent_P(PSTR("<div class=\"card\"><h2>Display</h2>"));
    server.sendContent_P(PSTR("<div class=\"form-group\"><label>Brightness (0-255)</label>"));
    server.sendContent("<input type=\"number\" name=\"brightness\" min=\"0\" max=\"255\" value=\"" + String(settings.brightness) + "\"></div>");
    
    server.sendContent_P(PSTR("<div class=\"form-group\"><label>Color Theme</label>"));
    server.sendContent_P(PSTR("<select name=\"color_theme\">"));
    server.sendContent(String("<option value=\"0\"") + (settings.colorTheme == THEME_STANDARD ? " selected" : "") + ">Standard</option>");
    server.sendContent(String("<option value=\"1\"") + (settings.colorTheme == THEME_HIGH_CONTRAST ? " selected" : "") + ">High Contrast</option>");
    server.sendContent(String("<option value=\"2\"") + (settings.colorTheme == THEME_STEALTH ? " selected" : "") + ">Stealth</option>");
    server.sendContent(String("<option value=\"3\"") + (settings.colorTheme == THEME_BUSINESS ? " selected" : "") + ">Business</option>");
    server.sendContent_P(PSTR("</select></div></div>"));

    // Quick-Access Profiles Card
    server.sendContent_P(PSTR("<div class=\"card\"><h2>Quick-Access Profiles</h2>"));
    server.sendContent_P(PSTR("<p class=\"muted\">Configure 3 quick-access profiles for different driving scenarios.</p>"));
    server.sendContent_P(PSTR("<a class=\"btn success\" href=\"/autopush\">Manage Auto-Push Profiles</a></div>"));

    // Display Colors Card
    server.sendContent_P(PSTR("<div class=\"card\"><h2>Display Colors</h2>"));
    server.sendContent_P(PSTR("<p class=\"muted\">Customize bogey, frequency, band, and arrow colors.</p>"));
    server.sendContent_P(PSTR("<a class=\"btn secondary\" href=\"/displaycolors\">Customize Display Colors</a></div>"));

    server.sendContent_P(PSTR("<button type=\"submit\" class=\"btn primary\">Save Settings</button>"));
    server.sendContent_P(PSTR("</form>"));
}

void WiFiManager::streamTimeSettingsBody() {
    // Use existing generateTimeSettingsHTML() body
    String fullHTML = generateTimeSettingsHTML();
    server.sendContent(fullHTML);
}

void WiFiManager::streamLogsBody() {
    const V1Settings& settings = settingsManager.get();
    String statusBox;
    if (alertLogger.isReady()) {
        statusBox = String("<div class='msg success'>") + alertLogger.statusText() + "</div>";
    } else {
        statusBox = "<div class='msg error'>SD card not mounted</div>";
    }
    
    server.sendContent(statusBox);
    server.sendContent_P(PSTR(R"HTML(
        <div class="actions">
            <button class="btn secondary" onclick="loadLogs()">Refresh</button>
            <button class="btn danger" onclick="showClearModal()">Clear All</button>
            <a class="btn secondary" href="/settings">Back to Settings</a>
        </div>
        <div class="filters" id="filters">
            <span>Filter:</span>
            <button class="filter-btn active" data-band="all">All</button>
        </div>
        <div class="stats" id="stats"></div>
        <div class="card">
            <table>
                <thead>
                    <tr>
                        <th data-col="utc" data-type="num">Time (UTC)</th>
                        <th data-col="event" data-type="str">Event</th>
                        <th data-col="band" data-type="str">Band</th>
                        <th data-col="freq" data-type="num">Freq</th>
                        <th data-col="dir" data-type="str">Dir</th>
                        <th data-col="front" data-type="num">Front</th>
                        <th data-col="rear" data-type="num">Rear</th>
                        <th data-col="count" data-type="num">Count</th>
                        <th data-col="muted" data-type="bool">Muted</th>
                    </tr>
                </thead>
                <tbody id="log-body"></tbody>
            </table>
        </div>
    
    <!-- Clear Confirmation Modal -->
    <div class="modal" id="clearModal">
        <div class="modal-box">
            <h3>Clear All Logs?</h3>
            <p>This will permanently delete all alert log data from the SD card. This action cannot be undone.</p>
            <div class="modal-btns">
                <button class="btn secondary" onclick="hideClearModal()">Cancel</button>
                <button class="btn danger" onclick="confirmClear()">Delete All</button>
            </div>
        </div>
    </div>
    
    <script>
        const bodyEl = document.getElementById('log-body');
        const statsEl = document.getElementById('stats');
        const filtersEl = document.getElementById('filters');
        const modal = document.getElementById('clearModal');
        let logData = [];
        let filteredData = [];
        let sortCol = 'utc';
        let sortDir = 'desc';
        let activeFilter = 'all';

        function formatTime(utc, ts){
            if(utc && utc > 1609459200){
                const d = new Date(utc * 1000);
                const pad = n => String(n).padStart(2,'0');
                return `${d.getUTCMonth()+1}/${d.getUTCDate()} ${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}`;
            }
            if(!ts || ts <= 0) return '-';
            const sec = ts/1000;
            if(sec < 60) return sec.toFixed(1) + 's';
            if(sec < 3600) return Math.floor(sec/60) + 'm ' + Math.floor(sec%60) + 's';
            return Math.floor(sec/3600) + 'h ' + Math.floor((sec%3600)/60) + 'm';
        }

        function updateFilters(){
            const bands = {};
            logData.forEach(d => { 
                if(d.band && d.band !== 'NONE') bands[d.band] = (bands[d.band]||0)+1; 
            });
            const keys = Object.keys(bands).sort();
            if(keys.length === 0) return;
            keys.forEach(k => {
                const existing = filtersEl.querySelector(`[data-band="${k}"]`);
                if(existing) {
                    existing.textContent = `${k} (${bands[k]})`;
                    return;
                }
                const btn = document.createElement('button');
                btn.className = 'filter-btn';
                btn.dataset.band = k;
                btn.textContent = `${k} (${bands[k]})`;
                btn.onclick = () => { activeFilter = k; applyFilter(); };
                filtersEl.appendChild(btn);
            });
            filtersEl.querySelectorAll('.filter-btn').forEach(b => {
                b.classList.toggle('active', b.dataset.band === activeFilter);
            });
        }

        function applyFilter(){
            if(activeFilter === 'all') filteredData = logData.slice();
            else filteredData = logData.filter(d => d.band === activeFilter);
            statsEl.textContent = `Showing ${filteredData.length} of ${logData.length} entries`;
            filtersEl.querySelectorAll('.filter-btn').forEach(b => {
                b.classList.toggle('active', b.dataset.band === activeFilter);
            });
            sortData();
            renderTable();
        }

        function sortData(){
            const type = document.querySelector(`th[data-col="${sortCol}"]`)?.dataset.type || 'str';
            filteredData.sort((a,b) => {
                let va = a[sortCol], vb = b[sortCol];
                if(sortCol === 'utc'){
                    va = a.utc > 1609459200 ? a.utc : (a.ts||0)/1000;
                    vb = b.utc > 1609459200 ? b.utc : (b.ts||0)/1000;
                }
                if(type === 'num'){ va = Number(va)||0; vb = Number(vb)||0; }
                else if(type === 'bool'){ va = va?1:0; vb = vb?1:0; }
                else { va = String(va||'').toLowerCase(); vb = String(vb||'').toLowerCase(); }
                if(va < vb) return sortDir === 'asc' ? -1 : 1;
                if(va > vb) return sortDir === 'asc' ? 1 : -1;
                return 0;
            });
        }

        function renderTable(){
            document.querySelectorAll('th').forEach(th => {
                th.classList.remove('sort-asc','sort-desc');
                if(th.dataset.col === sortCol) th.classList.add('sort-'+sortDir);
            });
            if(!filteredData.length){
                bodyEl.innerHTML = '<tr><td colspan="9" class="muted">No entries</td></tr>';
                return;
            }
            bodyEl.innerHTML = '';
            filteredData.forEach(item => {
                const tr = document.createElement('tr');
                tr.innerHTML = `
                    <td>${formatTime(item.utc, item.ts)}</td>
                    <td>${item.event}</td>
                    <td>${item.band || '-'}</td>
                    <td>${item.freq || '-'}</td>
                    <td>${item.dir || '-'}</td>
                    <td>${item.front ?? '-'}</td>
                    <td>${item.rear ?? '-'}</td>
                    <td>${item.count}</td>
                    <td>${item.muted ? 'Yes' : 'No'}</td>`;
                bodyEl.appendChild(tr);
            });
        }

        function loadLogs(){
            bodyEl.innerHTML = '<tr><td colspan="9" class="muted">Loading...</td></tr>';
            fetch('/api/logs').then(r => {
                if(!r.ok) throw new Error('HTTP ' + r.status);
                return r.text();
            }).then(text => {
                console.log('Raw response:', text);
                let data;
                try {
                    data = JSON.parse(text);
                } catch(e) {
                    console.error('JSON parse error:', e, 'Raw:', text);
                    throw new Error('Invalid JSON: ' + e.message);
                }
                if(data.error) {
                    bodyEl.innerHTML = '<tr><td colspan="9">' + data.error + '</td></tr>';
                    return;
                }
                logData = Array.isArray(data) ? data : [];
                updateFilters();
                applyFilter();
            }).catch(e => {
                console.error('Load error:', e);
                bodyEl.innerHTML = '<tr><td colspan="9">Error: ' + e.message + '</td></tr>';
            });
        }

        function showClearModal(){ modal.classList.add('show'); }
        function hideClearModal(){ modal.classList.remove('show'); }
        
        function confirmClear(){
            hideClearModal();
            fetch('/api/logs/clear',{method:'POST'}).then(r=>r.json()).then(d=>{
                activeFilter = 'all';
                loadLogs();
            }).catch(()=>{});
        }

        document.querySelectorAll('th[data-col]').forEach(th => {
            th.addEventListener('click', () => {
                const col = th.dataset.col;
                if(sortCol === col) sortDir = sortDir === 'asc' ? 'desc' : 'asc';
                else { sortCol = col; sortDir = 'desc'; }
                sortData();
                renderTable();
            });
        });

        modal.addEventListener('click', (e) => { if(e.target === modal) hideClearModal(); });

        window.onload = loadLogs;
    </script>
    )HTML"));
}

void WiFiManager::streamV1SettingsBody() {
    String currentJson = "null";
    if (v1ProfileManager.hasCurrentSettings()) {
        currentJson = v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings());
    }
    const V1Settings& settings = settingsManager.get();
    
    server.sendContent_P(PSTR(R"HTML(
    <div id="status" class="status-badge disconnected">
        Checking V1 connection...
    </div>
    
    <div class="card">
        <h2>Current V1 Settings</h2>
        <div id="current-settings">
            <p class="muted">Pull settings from V1 to view</p>
        </div>
        <button class="btn primary" onclick="pullSettings()">Pull from V1</button>
    </div>
    
    <div class="card">
        <h2>Saved Profiles</h2>
        <div id="profiles-list"></div>
        <button class="btn primary" onclick="showSaveModal()">Save Current as Profile</button>
    </div>
    
    <div class="card">
        <h2>Quick Actions</h2>
        <div class="actions">
            <button class="btn secondary" onclick="muteV1()">Mute All</button>
            <button class="btn secondary" onclick="unmuteV1()">Unmute All</button>
        </div>
    </div>
    
    <!-- Save Profile Modal -->
    <div class="modal" id="saveModal">
        <div class="modal-box">
            <h3>Save Profile</h3>
            <label>Profile Name:
                <input type="text" id="profile-name" placeholder="My V1 Profile">
            </label>
            <div class="modal-btns">
                <button class="btn secondary" onclick="hideSaveModal()">Cancel</button>
                <button class="btn primary" onclick="confirmSave()">Save</button>
            </div>
        </div>
    </div>
    
    <script>
        let currentSettings = )HTML"));
    server.sendContent(currentJson);
    server.sendContent_P(PSTR(R"HTML(;
        let profiles = [];
        const statusEl = document.getElementById('status');
        const currentEl = document.getElementById('current-settings');
        const profilesEl = document.getElementById('profiles-list');
        const modal = document.getElementById('saveModal');
        const nameInput = document.getElementById('profile-name');

        function checkStatus(){
            fetch('/api/v1/status').then(r=>r.json()).then(d=>{
                statusEl.className = 'status-badge ' + (d.connected ? 'connected' : 'disconnected');
                statusEl.textContent = d.connected ? 'V1 Connected' : 'V1 Disconnected';
            }).catch(()=>{});
        }

        function pullSettings(){
            currentEl.innerHTML = '<p class="muted">Pulling from V1...</p>';
            fetch('/api/v1settings/pull',{method:'POST'}).then(r=>r.json()).then(d=>{
                if(d.success){
                    currentSettings = d.settings;
                    renderSettings();
                } else {
                    currentEl.innerHTML = '<p class="muted">Failed to pull settings</p>';
                }
            }).catch(()=>{
                currentEl.innerHTML = '<p class="muted">Error pulling settings</p>';
            });
        }

        function renderSettings(){
            if(!currentSettings){
                currentEl.innerHTML = '<p class="muted">No settings loaded</p>';
                return;
            }
            let html = '<table class="settings-table">';
            for(const [k,v] of Object.entries(currentSettings)){
                html += `<tr><td>${k}</td><td>${v}</td></tr>`;
            }
            html += '</table>';
            currentEl.innerHTML = html;
        }

        function loadProfiles(){
            fetch('/api/v1settings/profiles').then(r=>r.json()).then(d=>{
                profiles = d;
                renderProfiles();
            }).catch(()=>{});
        }

        function renderProfiles(){
            if(!profiles.length){
                profilesEl.innerHTML = '<p class="muted">No saved profiles</p>';
                return;
            }
            let html = '<div class="profiles-grid">';
            profiles.forEach(p => {
                html += `<div class="profile-item">
                    <strong>${p}</strong>
                    <div>
                        <button class="btn secondary" onclick="pushProfile('${p}')">Push to V1</button>
                        <button class="btn danger" onclick="deleteProfile('${p}')">Delete</button>
                    </div>
                </div>`;
            });
            html += '</div>';
            profilesEl.innerHTML = html;
        }

        function pushProfile(name){
            fetch(`/api/v1settings/push?profile=${encodeURIComponent(name)}`,{method:'POST'})
                .then(r=>r.json()).then(d=>{
                    alert(d.success ? 'Profile pushed to V1!' : 'Failed to push profile');
                }).catch(()=>{ alert('Error pushing profile'); });
        }

        function deleteProfile(name){
            if(!confirm(`Delete profile "${name}"?`)) return;
            fetch(`/api/v1settings/delete?profile=${encodeURIComponent(name)}`,{method:'POST'})
                .then(r=>r.json()).then(d=>{
                    if(d.success) loadProfiles();
                }).catch(()=>{});
        }

        function showSaveModal(){
            if(!currentSettings){
                alert('Pull settings from V1 first');
                return;
            }
            modal.classList.add('show');
            nameInput.value = '';
            nameInput.focus();
        }

        function hideSaveModal(){ modal.classList.remove('show'); }

        function confirmSave(){
            const name = nameInput.value.trim();
            if(!name){
                alert('Enter a profile name');
                return;
            }
            hideSaveModal();
            fetch('/api/v1settings/save',{
                method:'POST',
                headers:{'Content-Type':'application/json'},
                body:JSON.stringify({name:name})
            }).then(r=>r.json()).then(d=>{
                if(d.success) loadProfiles();
                else alert('Failed to save profile');
            }).catch(()=>{ alert('Error saving profile'); });
        }

        function muteV1(){
            fetch('/api/v1/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:'mute',state:true})})
                .then(r=>r.json()).catch(()=>{});
        }

        function unmuteV1(){
            fetch('/api/v1/command',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({command:'mute',state:false})})
                .then(r=>r.json()).catch(()=>{});
        }

        modal.addEventListener('click', (e) => { if(e.target === modal) hideSaveModal(); });

        window.onload = () => {
            checkStatus();
            loadProfiles();
            if(currentSettings) renderSettings();
            setInterval(checkStatus, 5000);
        };
    </script>
    )HTML"));
}

void WiFiManager::streamAutoPushBody() {
    const V1Settings& s = settingsManager.get();
    
    server.sendContent_P(PSTR("<p class=\"muted\" style=\"text-align:center; margin-bottom:20px;\">Configure 3 quick-access profiles for different driving scenarios</p>\n<div id=\"message\"></div>\n"));
    
    server.sendContent_P(PSTR("<div class=\"card\"><h2>Quick Push</h2><p class=\"muted\">Click to activate and push a profile immediately</p><div class=\"quick-push\">"));
    server.sendContent("<div class=\"quick-btn\" id=\"quick-0\" onclick=\"quickPush(0)\"><div class=\"quick-btn-icon\">1</div><div class=\"quick-btn-label\">" + htmlEscape(s.slot0Name) + "</div><div class=\"quick-btn-sub\" id=\"quick-sub-0\">Not configured</div></div>");
    server.sendContent("<div class=\"quick-btn\" id=\"quick-1\" onclick=\"quickPush(1)\"><div class=\"quick-btn-icon\">2</div><div class=\"quick-btn-label\">" + htmlEscape(s.slot1Name) + "</div><div class=\"quick-btn-sub\" id=\"quick-sub-1\">Not configured</div></div>");
    server.sendContent("<div class=\"quick-btn\" id=\"quick-2\" onclick=\"quickPush(2)\"><div class=\"quick-btn-icon\">3</div><div class=\"quick-btn-label\">" + htmlEscape(s.slot2Name) + "</div><div class=\"quick-btn-sub\" id=\"quick-sub-2\">Not configured</div></div>");
    server.sendContent("</div><div class=\"setting-row\"><span>Auto-push on V1 connect</span><div class=\"toggle " + String(s.autoPushEnabled ? "on" : "") + "\" id=\"auto-toggle\" onclick=\"toggleAutoPush()\"></div></div></div>");
    
    // Slot cards (0-2)
    for (int slot = 0; slot < 3; slot++) {
        String slotName, slotColor;
        if (slot == 0) { slotName = "Default"; slotColor = "#400050"; }
        else if (slot == 1) { slotName = "Highway"; slotColor = "#00fc00"; }
        else { slotName = "Comfort"; slotColor = "#808080"; }
        
        String placeholder = slotName.c_str();
        placeholder.toUpperCase();
        
        server.sendContent("<div class=\"card slot-card\" id=\"slot-" + String(slot) + "\"><div class=\"slot-header\"><h2>Slot " + String(slot+1) + ": " + slotName + "</h2>");
        server.sendContent("<span class=\"status-badge\" id=\"badge-" + String(slot) + "\" style=\"display:none;\">ACTIVE</span></div>");
        server.sendContent("<div class=\"form-group\"><label>Display Name</label><input type=\"text\" id=\"name-" + String(slot) + "\" maxlength=\"20\" placeholder=\"" + placeholder + "\"></div>");
        server.sendContent("<div class=\"form-group\"><label>Display Color</label><input type=\"color\" id=\"color-" + String(slot) + "\" value=\"" + slotColor + "\"></div>");
        server.sendContent("<div class=\"form-group\"><label>Profile</label><select id=\"profile-" + String(slot) + "\"><option value=\"\">-- None --</option></select></div>");
        server.sendContent_P(PSTR("<div class=\"form-group\"><label>Mode</label><select id=\"mode-"));
        server.sendContent(String(slot) + "\"><option value=\"0\">No Change</option><option value=\"1\">All Bogeys</option><option value=\"2\">Logic</option><option value=\"3\">Advanced Logic</option></select></div>");
        server.sendContent_P(PSTR("<div class=\"form-group\"><label>V1 Volume</label><select id=\"volume-"));
        server.sendContent(String(slot) + "\"><option value=\"255\">No Change</option><option value=\"0\">0 (Off)</option><option value=\"1\">1</option><option value=\"2\">2</option><option value=\"3\">3</option><option value=\"4\">4</option><option value=\"5\">5</option><option value=\"6\">6</option><option value=\"7\">7</option><option value=\"8\">8</option><option value=\"9\">9 (Max)</option></select></div>");
        server.sendContent_P(PSTR("<div class=\"form-group\"><label>Mute Volume</label><select id=\"muteVol-"));
        server.sendContent(String(slot) + "\"><option value=\"255\">No Change</option><option value=\"0\">0 (Silent)</option><option value=\"1\">1</option><option value=\"2\">2</option><option value=\"3\">3</option><option value=\"4\">4</option><option value=\"5\">5</option><option value=\"6\">6</option><option value=\"7\">7</option><option value=\"8\">8</option><option value=\"9\">9 (Max)</option></select></div>");
        server.sendContent("<div class=\"actions\"><button class=\"btn primary\" onclick=\"saveSlot(" + String(slot) + ")\">Save</button><button class=\"btn success\" onclick=\"pushSlot(" + String(slot) + ")\">Push Now</button></div></div>");
    }
    
    // Complete function - just send body from generate function (minus the wrapWithLayout)
    String fullHTML = generateAutoPushHTML();
    server.sendContent(fullHTML);
}

void WiFiManager::streamDisplayColorsBody() {
    // Send body from generate function (minus the wrapWithLayout)
    String fullHTML = generateDisplayColorsHTML();
    server.sendContent(fullHTML);
}

