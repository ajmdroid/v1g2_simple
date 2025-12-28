/**
 * WiFi Manager for V1 Gen2 Display
 * Handles WiFi AP/STA modes and web server
 */

#include "wifi_manager.h"
#include "settings.h"
#include "display.h"
#include "alert_logger.h"
#include "v1_profiles.h"
#include "ble_client.h"
#include "../include/config.h"
#include "../include/color_themes.h"
#include <algorithm>
#include <ArduinoJson.h>

// External BLE client for V1 commands
extern V1BLEClient bleClient;

namespace {
String htmlEscape(const String& in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); ++i) {
        char c = in.charAt(i);
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '\"': out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;        break;
        }
    }
    return out;
}
} // namespace

// Global instance
WiFiManager wifiManager;

WiFiManager::WiFiManager() : server(80), apActive(false) {
}

bool WiFiManager::begin() {
    const V1Settings& settings = settingsManager.get();
    
    if (!settings.enableWifi) {
        Serial.println("WiFi disabled in settings");
        return false;
    }
    
    Serial.println("Starting WiFi...");

    // Only support AP mode for now; ignore other modes to avoid unstable STA flows
    if (settings.wifiMode != V1_WIFI_AP) {
        Serial.println("WiFiManager: forcing AP mode (STA/dual disabled)");
    }
    setupAP();
    
    setupWebServer();
    server.begin();
    Serial.println("Web server started on port 80");
    
    return true;
}

void WiFiManager::setupAP() {
    const V1Settings& settings = settingsManager.get();
    
    String apSSID = settings.apSSID.length() > 0 ? settings.apSSID : "V1-Display";
    String apPass = settings.apPassword.length() >= 8 ? settings.apPassword : "valentine1";
    
    Serial.printf("Starting AP: %s\n", apSSID.c_str());
    
    if (WiFi.getMode() != WIFI_AP_STA) {
        WiFi.mode(WIFI_AP);
    }
    
    // Configure custom AP IP address
    IPAddress apIP(192, 168, 35, 5);
    IPAddress gateway(192, 168, 35, 1);
    IPAddress subnet(255, 255, 255, 0);
    WiFi.softAPConfig(apIP, gateway, subnet);
    
    WiFi.softAP(apSSID.c_str(), apPass.c_str());
    apActive = true;
    
    IPAddress ip = WiFi.softAPIP();
    Serial.printf("AP IP address: %s\n", ip.toString().c_str());
}

void WiFiManager::setupWebServer() {
    server.on("/", HTTP_GET, [this]() { handleSettings(); });  // Root redirects to settings
    server.on("/status", HTTP_GET, [this]() { handleStatus(); });
    server.on("/settings", HTTP_GET, [this]() { handleSettings(); });
    server.on("/settings", HTTP_POST, [this]() { handleSettingsSave(); });
    server.on("/darkmode", HTTP_POST, [this]() { handleDarkMode(); });
    server.on("/mute", HTTP_POST, [this]() { handleMute(); });
    server.on("/logs", HTTP_GET, [this]() { handleLogs(); });
    server.on("/api/logs", HTTP_GET, [this]() { handleLogsData(); });
    server.on("/api/logs/clear", HTTP_POST, [this]() { handleLogsClear(); });
    
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
    
    // STA/dual modes are disabled; skip reconnection logic
}

void WiFiManager::stop() {
    server.stop();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    apActive = false;
    Serial.println("WiFi stopped");
}

bool WiFiManager::isConnected() const {
    return false;  // STA mode disabled
}

bool WiFiManager::isAPActive() const {
    return apActive;
}

String WiFiManager::getIPAddress() const {
    return "";  // STA mode disabled
}

String WiFiManager::getAPIPAddress() const {
    if (apActive) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

void WiFiManager::handleStatus() {
    String json = "{";
    json += "\"connected\":false,";  // STA mode disabled
    json += "\"ap_active\":" + String(apActive ? "true" : "false") + ",";
    json += "\"ip\":\"\",";  // STA mode disabled
    json += "\"ap_ip\":\"" + getAPIPAddress() + "\"";
    
    if (getStatusJson) {
        json += "," + getStatusJson();
    }
    
    json += "}";
    
    server.send(200, "application/json", json);
}

void WiFiManager::handleSettings() {
    server.send(200, "text/html", generateSettingsHTML());
}

void WiFiManager::handleSettingsSave() {
    Serial.println("=== handleSettingsSave() called ===");
    
    // Log all received args
    Serial.printf("Number of args: %d\n", server.args());
    for (int i = 0; i < server.args(); i++) {
        Serial.printf("  Arg[%d]: %s = %s\n", i, server.argName(i).c_str(), server.arg(i).c_str());
    }
    
    // Track if WiFi settings changed (these require restart)
    bool wifiChanged = false;
    
    // Open preferences directly
    Preferences prefs;
    if (!prefs.begin("v1settings", false)) {
        Serial.println("ERROR: Failed to open preferences!");
        server.send(500, "text/plain", "Failed to save settings");
        return;
    }
    Serial.println("Preferences opened for writing");
    
    // Save each field and check return values
    size_t written;
    
    if (server.hasArg("ssid")) {
        // STA mode disabled for now; keep values but we won't use them
        String ssid = server.arg("ssid");
        String pass = server.arg("password");
        if (ssid != prefs.getString("ssid", "") || pass != prefs.getString("password", "")) {
            wifiChanged = true;
        }
        written = prefs.putString("ssid", ssid);
        Serial.printf("Saved ssid='%s', bytes=%d (STA disabled)\n", ssid.c_str(), written);
        written = prefs.putString("password", pass);
        Serial.printf("Saved password, bytes=%d (STA disabled)\n", written);
    }
    
    if (server.hasArg("ap_ssid")) {
        String apSsid = server.arg("ap_ssid");
        String apPass = server.arg("ap_password");
        if (apSsid.length() == 0 || apPass.length() < 8) {
            prefs.end();
            server.send(400, "text/plain", "AP SSID required and password must be at least 8 characters");
            return;
        }
        if (apSsid != prefs.getString("apSSID", "") || apPass != prefs.getString("apPassword", "")) {
            wifiChanged = true;
        }
        written = prefs.putString("apSSID", apSsid);
        Serial.printf("Saved apSSID='%s', bytes=%d\n", apSsid.c_str(), written);
        written = prefs.putString("apPassword", apPass);
        Serial.printf("Saved apPassword, bytes=%d\n", written);
    }
    
    // Force AP mode regardless of submitted value
    int mode = V1_WIFI_AP;
    if (mode != prefs.getInt("wifiMode", V1_WIFI_AP)) {
        wifiChanged = true;
    }
    written = prefs.putInt("wifiMode", mode);
    Serial.printf("Saved wifiMode=%d (forced AP), bytes=%d\n", mode, written);
    
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        written = prefs.putUChar("brightness", (uint8_t)brightness);
        Serial.printf("Saved brightness=%d, bytes=%d\n", brightness, written);
    }
    
    if (server.hasArg("color_theme")) {
        int theme = server.arg("color_theme").toInt();
        theme = std::max(0, std::min(theme, 2));  // Clamp to valid theme range
        written = prefs.putInt("colorTheme", theme);
        Serial.printf("Saved colorTheme=%d, bytes=%d\n", theme, written);
    }
    
    // Auto-push settings now handled on dedicated /autopush page
    
    // Verify immediately before closing
    Serial.println("--- Verifying before prefs.end() ---");
    Serial.printf("  Read back wifiMode: %d\n", prefs.getInt("wifiMode", -999));
    Serial.printf("  Read back brightness: %d\n", prefs.getUChar("brightness", 255));
    Serial.printf("  Read back colorTheme: %d\n", prefs.getInt("colorTheme", -1));
    Serial.printf("  Read back apSSID: %s\n", prefs.getString("apSSID", "FAIL").c_str());
    
    prefs.end();
    Serial.println("Preferences closed");
    
    // Verify after closing by reopening read-only
    Serial.println("--- Verifying after prefs.end() ---");
    Preferences verify;
    verify.begin("v1settings", true);
    Serial.printf("  Read back wifiMode: %d\n", verify.getInt("wifiMode", -999));
    Serial.printf("  Read back brightness: %d\n", verify.getUChar("brightness", 255));
    Serial.printf("  Read back colorTheme: %d\n", verify.getInt("colorTheme", -1));
    Serial.printf("  Read back apSSID: %s\n", verify.getString("apSSID", "FAIL").c_str());
    verify.end();
    
    // Reload settings into memory
    Serial.println("--- Calling settingsManager.load() ---");
    settingsManager.load();
    
    // Print settings after reload
    const V1Settings& s = settingsManager.get();
    Serial.println("=== Settings after reload ===");
    Serial.printf("  brightness: %d\n", s.brightness);
    Serial.printf("  wifiMode: %d\n", s.wifiMode);
    Serial.printf("  apSSID: %s\n", s.apSSID.c_str());
    Serial.printf("  colorTheme: %d\n", s.colorTheme);
    
    // Update display color theme if it was changed
    if (server.hasArg("color_theme")) {
        display.updateColorTheme();
        Serial.println("Display color theme updated");
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
    
    Serial.printf("Dark mode request: %s, success: %s\n", darkMode ? "ON" : "OFF", success ? "yes" : "no");
    
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
    
    Serial.printf("Mute request: %s, success: %s\n", muted ? "ON" : "OFF", success ? "yes" : "no");
    
    String json = "{\"success\":" + String(success ? "true" : "false") + 
                  ",\"muted\":" + String(muted ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void WiFiManager::handleLogs() {
    server.send(200, "text/html", generateLogsHTML());
}

void WiFiManager::handleLogsData() {
    if (!alertLogger.isReady()) {
        server.send(503, "application/json", "{\"error\":\"SD card not mounted\"}");
        return;
    }

    String json = alertLogger.getRecentJson();
    server.send(200, "application/json", json);
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

void WiFiManager::handleV1Settings() {
    server.send(200, "text/html", generateV1SettingsHTML());
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
    Serial.printf("[V1Settings] Save request body: %s\n", body.c_str());
    
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
    JsonObject settings = doc["settings"];
    if (!settings.isNull()) {
        v1ProfileManager.jsonToSettings(body, profile.settings);
    } else {
        // Direct settings in root
        v1ProfileManager.jsonToSettings(body, profile.settings);
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
    Serial.printf("[V1Settings] Push request: %s\n", body.c_str());
    
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
        Serial.println("[V1Settings] Using raw bytes from request");
    } else {
        // Parse from individual settings
        V1UserSettings settings;
        if (!v1ProfileManager.jsonToSettings(body, settings)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
        memcpy(bytes, settings.bytes, 6);
        Serial.printf("[V1Settings] Built bytes from settings: %02X %02X %02X %02X %02X %02X\n",
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


String WiFiManager::generateSettingsHTML() {
    const V1Settings& settings = settingsManager.get();
    String apSsidEsc = htmlEscape(settings.apSSID);
    String apPassEsc = htmlEscape(settings.apPassword);
    
    Serial.println("=== generateSettingsHTML() ===");
    Serial.printf("  brightness: %d\n", settings.brightness);
    Serial.printf("  wifiMode: %d\n", settings.wifiMode);
    Serial.printf("  apSSID: %s\n", settings.apSSID.c_str());
    Serial.printf("  colorTheme: %d (STANDARD=%d, HIGH_CONTRAST=%d, STEALTH=%d)\n", 
                  settings.colorTheme, THEME_STANDARD, THEME_HIGH_CONTRAST, THEME_STEALTH);
    
    String saved = "";
    if (server.hasArg("saved")) {
        if (server.hasArg("wifi")) {
            saved = "<div class='msg success'>Settings saved! Restart to apply WiFi changes.</div>";
        } else {
            saved = "<div class='msg success'>Settings saved!</div>";
        }
    }
    
    // Generate theme-specific CSS
    String themeCSS;
    switch (settings.colorTheme) {
        case THEME_HIGH_CONTRAST:
            themeCSS = R"CSS(
                body { background: #000; color: #fff; }
                h1 { color: #ffff00; }
                .card { background: #1a1a1a; border: 2px solid #ffff00; }
                .card h2 { color: #ffff00; }
                label { color: #ccc; }
                input, select { background: #2a2a2a; color: #fff; border: 1px solid #555; }
                input:focus, select:focus { outline: 2px solid #ffff00; border-color: #ffff00; }
                button, .btn { background: #ffff00; color: #000; font-weight: bold; }
                button:hover, .btn:hover { background: #ffcc00; }
                .btn-dark { background: #444; color: #fff; }
                .btn-dark.active { background: #ffff00; color: #000; }
                .btn-mute { background: #00ccff; color: #000; }
                .btn-mute.active { background: #ff00ff; color: #fff; }
                .msg.success { background: #00ff00; color: #000; }
            )CSS";
            break;
        case THEME_STEALTH:
            themeCSS = R"CSS(
                body { background: #0a0a0a; color: #666; }
                h1 { color: #444; }
                .card { background: #111; border: 1px solid #222; }
                .card h2 { color: #555; }
                label { color: #444; }
                input, select { background: #151515; color: #666; border: 1px solid #222; }
                input:focus, select:focus { outline: 1px solid #444; border-color: #333; }
                button, .btn { background: #333; color: #666; }
                button:hover, .btn:hover { background: #444; }
                .btn-dark { background: #222; color: #555; }
                .btn-dark.active { background: #444; color: #888; }
                .btn-mute { background: #1a2a3a; color: #555; }
                .btn-mute.active { background: #333; color: #777; }
                .msg.success { background: #1a3a1a; color: #4a8a4a; }
            )CSS";
            break;
        default: // THEME_STANDARD
            themeCSS = R"CSS(
                body { background: #1a1a2e; color: #eee; }
                h1 { color: #e94560; }
                .card { background: #16213e; }
                .card h2 { color: #e94560; }
                label { color: #888; }
                input, select { background: #0f3460; color: #eee; }
                input:focus, select:focus { outline: 2px solid #e94560; }
                button, .btn { background: #e94560; color: #fff; }
                button:hover, .btn:hover { background: #d63050; }
                .btn-dark { background: #333; }
                .btn-dark.active { background: #e94560; }
                .btn-mute { background: #0f4c75; }
                .btn-mute.active { background: #e94560; }
                .msg.success { background: #3a9104; }
            )CSS";
            break;
    }
    
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>V1 Display Settings</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body { 
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 600px; margin: 0 auto; }
        h1 { margin-bottom: 20px; text-align: center; }
        .card {
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 20px;
        }
        .card h2 { margin-bottom: 15px; font-size: 1.2em; }
        .form-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; }
        input, select {
            width: 100%;
            padding: 12px;
            border: none;
            border-radius: 8px;
            font-size: 1em;
        }
        button, .btn {
            padding: 15px;
            border: none;
            border-radius: 8px;
            font-size: 1.1em;
            cursor: pointer;
        }
        .btn-full { width: 100%; margin-top: 10px; }
        .controls { display: flex; gap: 10px; }
        .controls .btn { flex: 1; }
        .msg { padding: 10px; border-radius: 8px; margin-bottom: 20px; text-align: center; }
        .muted { color: #9aa7bd; font-size: 0.95em; }
        
        /* Theme-specific colors */
        )HTML" + themeCSS + R"HTML(
    </style>
</head>
<body>
    <div class="container">
        <h1>V1 Display Settings</h1>
        
        )HTML" + saved + R"HTML(
        
        <div class="card">
            <h2>V1 Settings</h2>
            <p class="muted">Pull, edit, and push V1 user settings. Save profiles for quick switching.</p>
            <a class="btn btn-full" href="/v1settings" style="display:block; text-decoration:none; text-align:center;">Open V1 Settings</a>
        </div>
        
        <div class="card">
            <h2>Alert Logs</h2>
            <p class="muted">Alerts are recorded to the SD card. View and clear them from the log page.</p>
            <a class="btn btn-full" href="/logs" style="display:block; text-decoration:none; text-align:center;">Open Alert Log</a>
        </div>
        
        <form method="POST" action="/settings">
            
            <div class="card">
                <h2>Access Point Settings</h2>
                <div class="form-group">
                    <label>AP Network Name</label>
                    <input type="text" name="ap_ssid" value=")HTML" + apSsidEsc + R"HTML(">
                </div>
                <div class="form-group">
                    <label>AP Password (min 8 chars)</label>
                    <input type="password" name="ap_password" value=")HTML" + apPassEsc + R"HTML(">
                </div>
            </div>
            
            <div class="card">
                <h2>Display</h2>
                <div class="form-group">
                    <label>Brightness (0-255)</label>
                    <input type="number" name="brightness" min="0" max="255" value=")HTML" + String(settings.brightness) + R"HTML(">
                </div>
                <div class="form-group">
                    <label>Color Theme</label>
                    <select name="color_theme">
                        <option value="0")HTML" + String(settings.colorTheme == THEME_STANDARD ? " selected" : "") + R"HTML(">Standard (Red/Pink)</option>
                        <option value="1")HTML" + String(settings.colorTheme == THEME_HIGH_CONTRAST ? " selected" : "") + R"HTML(">High Contrast (Yellow)</option>
                        <option value="2")HTML" + String(settings.colorTheme == THEME_STEALTH ? " selected" : "") + R"HTML(">Stealth (Dark Gray)</option>
                    </select>
                </div>
            </div>
            
            <div class="card" style="text-align:center;">
                <h2>🎯 Quick-Access Profiles</h2>
                <p style="color:#888;margin-bottom:15px;">Configure 3 quick-access profiles for different driving scenarios</p>
                <a href="/autopush" style="text-decoration:none;">
                    <button type="button" class="btn-full" style="background:#3a9104;">Manage Auto-Push Profiles →</button>
                </a>
            </div>
            
            <div class="card" style="text-align:center;">
                <h2>🎨 Display Colors</h2>
                <p style="color:#888;margin-bottom:15px;">Customize bogey, frequency, band, and arrow colors</p>
                <a href="/displaycolors" style="text-decoration:none;">
                    <button type="button" class="btn-full" style="background:#9c27b0;">Customize Display Colors →</button>
                </a>
            </div>
            
            <button type="submit" class="btn-full">Save Settings</button>
        </form>
    </div>
</body>
</html>
)HTML";
    return html;
}

String WiFiManager::generateLogsHTML() {
    String statusBox;
    if (alertLogger.isReady()) {
        statusBox = String("<div class='msg success'>") + alertLogger.statusText() + "</div>";
    } else {
        statusBox = "<div class='msg error'>SD card not mounted</div>";
    }

    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>V1 Alert Logs</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #10121b;
            color: #eaeaea;
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 900px; margin: 0 auto; }
        h1 { color: #e94560; margin-bottom: 10px; text-align: center; }
        .card {
            background: #1a1d2a;
            border-radius: 12px;
            padding: 16px;
            margin-bottom: 20px;
            box-shadow: 0 12px 25px rgba(0,0,0,0.25);
            max-height: 60vh;
            overflow-y: auto;
        }
        .actions { display: flex; gap: 10px; margin-bottom: 12px; flex-wrap: wrap; }
        .btn {
            padding: 12px 14px;
            border: none;
            border-radius: 8px;
            background: #e94560;
            color: #fff;
            font-size: 1em;
            cursor: pointer;
        }
        .btn.secondary { background: #0f4c75; }
        .btn.danger { background: #7a1d2f; }
        .btn:disabled { opacity: 0.6; cursor: not-allowed; }
        .msg { padding: 10px; border-radius: 8px; margin: 10px 0; text-align: center; }
        .msg.success { background: #24572d; }
        .msg.error { background: #7a1d2f; }
        table { width: 100%; border-collapse: collapse; margin-top: 10px; }
        th, td { padding: 10px; text-align: left; }
        th { 
            background: #0f3460; 
            color: #eaeaea; 
            position: sticky; 
            top: 0; 
            cursor: pointer;
            user-select: none;
        }
        th:hover { background: #1a5a9e; }
        th.sort-asc::after { content: ' ▲'; font-size: 0.8em; }
        th.sort-desc::after { content: ' ▼'; font-size: 0.8em; }
        tr:nth-child(odd) { background: #14182b; }
        tr:nth-child(even) { background: #111427; }
        .muted { color: #8fa3c0; }
        .filters { display: flex; gap: 8px; margin-bottom: 12px; flex-wrap: wrap; align-items: center; }
        .filters span { color: #8fa3c0; font-size: 0.9em; }
        .filter-btn {
            padding: 8px 14px;
            border: 2px solid #0f4c75;
            border-radius: 20px;
            background: transparent;
            color: #8fa3c0;
            font-size: 0.9em;
            cursor: pointer;
            transition: all 0.2s;
        }
        .filter-btn:hover { border-color: #e94560; color: #eaeaea; }
        .filter-btn.active { background: #e94560; border-color: #e94560; color: #fff; }
        .filter-btn .count { font-weight: bold; margin-left: 4px; }
        .stats { font-size: 0.9em; color: #8fa3c0; margin-bottom: 8px; }
        .stats b { color: #e94560; }
        /* Modal styles */
        .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.8); z-index: 100; justify-content: center; align-items: center; }
        .modal.show { display: flex; }
        .modal-box { background: #1a1d2a; padding: 24px; border-radius: 12px; max-width: 400px; text-align: center; }
        .modal-box h3 { color: #e94560; margin-bottom: 12px; }
        .modal-box p { margin-bottom: 20px; color: #8fa3c0; }
        .modal-btns { display: flex; gap: 10px; justify-content: center; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Alert Log</h1>
        )HTML" + statusBox + R"HTML(
        <div class="actions">
            <button class="btn" onclick="loadLogs()">Refresh</button>
            <button class="btn danger" onclick="showClearModal()">🗑 Clear All</button>
            <a href="/settings" style="flex:1; text-decoration:none;"><button class="btn" style="width:100%; background:#333;">Back to Settings</button></a>
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
                        <th data-col="ms" data-type="num">Time</th>
                        <th data-col="event" data-type="str">Event</th>
                        <th data-col="band" data-type="str">Band</th>
                        <th data-col="freq" data-type="num">Freq</th>
                        <th data-col="direction" data-type="str">Dir</th>
                        <th data-col="front" data-type="num">Front</th>
                        <th data-col="rear" data-type="num">Rear</th>
                        <th data-col="count" data-type="num">Count</th>
                        <th data-col="muted" data-type="bool">Muted</th>
                    </tr>
                </thead>
                <tbody id="log-body"></tbody>
            </table>
        </div>
    </div>
    
    <!-- Clear Confirmation Modal -->
    <div class="modal" id="clearModal">
        <div class="modal-box">
            <h3>⚠️ Clear All Logs?</h3>
            <p>This will permanently delete all alert log data from the SD card. This action cannot be undone.</p>
            <div class="modal-btns">
                <button class="btn" style="background:#333;" onclick="hideClearModal()">Cancel</button>
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
        let sortCol = 'ms';
        let sortDir = 'desc';
        let activeFilter = 'all';

        function formatTime(ms){
            const sec = ms/1000;
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
                let va = a[sortCol], vb = b[sortCol];
                if(type === 'num'){ va = Number(va)||0; vb = Number(vb)||0; }
                else if(type === 'bool'){ va = va?1:0; vb = vb?1:0; }
                else { va = String(va).toLowerCase(); vb = String(vb).toLowerCase(); }
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
                    <td>${formatTime(item.ms)}</td>
                    <td>${item.event}</td>
                    <td>${item.band}</td>
                    <td>${item.freq}</td>
                    <td>${item.direction}</td>
                    <td>${item.front}</td>
                    <td>${item.rear}</td>
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
</body>
</html>
)HTML";
    return html;
}

String WiFiManager::generateV1SettingsHTML() {
    String currentJson = "null";
    if (v1ProfileManager.hasCurrentSettings()) {
        currentJson = v1ProfileManager.settingsToJson(v1ProfileManager.getCurrentSettings());
    }
    
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>V1 Settings</title>
    <style>
        *{box-sizing:border-box;margin:0;padding:0;}
        body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#1a1a1a;color:#fff;padding:16px;max-width:600px;margin:0 auto;}
        h1{font-size:1.5rem;margin-bottom:16px;color:#fff;}
        h2{font-size:1.1rem;margin-bottom:12px;color:#ccc;}
        .card{background:#252525;border-radius:12px;padding:16px;margin-bottom:16px;}
        .btn{background:#333;color:#fff;border:none;padding:12px 20px;border-radius:8px;cursor:pointer;font-size:1rem;margin:4px;}
        .btn:hover{background:#444;}
        .btn:disabled{opacity:0.5;cursor:not-allowed;}
        .btn-primary{background:#007aff;}
        .btn-primary:hover{background:#0056b3;}
        .btn-danger{background:#ff3b30;}
        .btn-danger:hover{background:#c0392b;}
        .btn-success{background:#34c759;}
        .btn-success:hover{background:#28a745;}
        .btn-full{width:100%;margin:8px 0;}
        .status{padding:12px;border-radius:8px;margin-bottom:16px;}
        .status.connected{background:#1a3a1a;border:1px solid #34c759;}
        .status.disconnected{background:#3a1a1a;border:1px solid #ff3b30;}
        .profile-list{list-style:none;}
        .profile-item{background:#333;padding:12px;margin:8px 0;border-radius:8px;display:flex;justify-content:space-between;align-items:center;cursor:pointer;}
        .profile-item:hover{background:#444;}
        .profile-item.selected{background:#007aff33;border:1px solid #007aff;}
        .profile-name{font-weight:bold;}
        .setting-row{display:flex;justify-content:space-between;align-items:center;padding:10px 0;border-bottom:1px solid #333;}
        .setting-row:last-child{border-bottom:none;}
        .toggle{width:50px;height:28px;background:#555;border-radius:14px;position:relative;cursor:pointer;transition:background 0.2s;}
        .toggle.on{background:#34c759;}
        .toggle::after{content:'';position:absolute;width:24px;height:24px;background:#fff;border-radius:12px;top:2px;left:2px;transition:left 0.2s;}
        .toggle.on::after{left:24px;}
        .select-row select{background:#333;color:#fff;border:1px solid #555;padding:8px 12px;border-radius:6px;font-size:1rem;}
        .modal{display:none;position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.8);z-index:1000;align-items:center;justify-content:center;}
        .modal.show{display:flex;}
        .modal-box{background:#252525;border-radius:12px;padding:24px;max-width:90%;max-height:90%;overflow-y:auto;width:500px;}
        .modal-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:16px;}
        .modal-close{background:none;border:none;color:#fff;font-size:1.5rem;cursor:pointer;}
        input[type="text"]{background:#333;color:#fff;border:1px solid #555;padding:10px;border-radius:6px;width:100%;font-size:1rem;margin-bottom:12px;}
        .msg{padding:12px;border-radius:8px;margin-bottom:12px;}
        .msg.success{background:#1a3a1a;color:#34c759;}
        .msg.error{background:#3a1a1a;color:#ff3b30;}
        .back-link{color:#007aff;text-decoration:none;display:inline-block;margin-bottom:16px;}
        .back-link:hover{text-decoration:underline;}
        .bytes-display{font-family:monospace;background:#333;padding:8px;border-radius:4px;margin:8px 0;}
    </style>
</head>
<body>
    <a href="/settings" class="back-link">← Back to Settings</a>
    <h1>V1 Settings & Profiles</h1>
    
    <div id="status" class="status disconnected">
        Checking V1 connection...
    </div>
    
    <div class="card">
        <h2>Current V1 Settings</h2>
        <div id="current-settings">
            <p style="color:#888;">Pull settings from V1 to view</p>
        </div>
        <button class="btn btn-primary btn-full" onclick="pullSettings()">Pull from V1</button>
    </div>
    
    <div class="card">
        <h2>Saved Profiles</h2>
        <ul id="profile-list" class="profile-list">
            <li style="color:#888;">Loading profiles...</li>
        </ul>
        <div style="margin-top:12px;">
            <button class="btn btn-primary" onclick="showNewProfileModal()">New Profile</button>
            <button id="btn-edit" class="btn" onclick="editSelectedProfile()" disabled>Edit</button>
            <button id="btn-push" class="btn btn-success" onclick="pushSelectedProfile()" disabled>Push to V1</button>
            <button id="btn-delete" class="btn btn-danger" onclick="deleteSelectedProfile()" disabled>Delete</button>
        </div>
    </div>
    
    <!-- Edit Modal -->
    <div id="edit-modal" class="modal">
        <div class="modal-box">
            <div class="modal-header">
                <h2 id="modal-title">Edit Profile</h2>
                <button class="modal-close" onclick="closeEditModal()">&times;</button>
            </div>
            <input type="text" id="profile-name" placeholder="Profile Name">
            <div id="settings-editor"></div>
            <div style="margin-top:16px;">
                <button class="btn btn-primary" onclick="saveProfile()">Save</button>
                <button class="btn" onclick="closeEditModal()">Cancel</button>
                <button id="btn-push-after-save" class="btn btn-success" onclick="saveAndPush()" style="float:right;">Save & Push to V1</button>
            </div>
        </div>
    </div>
    
    <!-- New Profile Modal -->
    <div id="new-modal" class="modal">
        <div class="modal-box">
            <div class="modal-header">
                <h2>New Profile</h2>
                <button class="modal-close" onclick="closeNewModal()">&times;</button>
            </div>
            <input type="text" id="new-profile-name" placeholder="Profile Name">
            <p style="color:#888;margin-bottom:12px;">Create from:</p>
            <button class="btn btn-full" onclick="createFromDefaults()">Factory Defaults</button>
            <button class="btn btn-full" onclick="createFromCurrent()" id="btn-from-current" disabled>Current V1 Settings</button>
            <button class="btn btn-full" onclick="closeNewModal()">Cancel</button>
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
                list.innerHTML = '<li style="color:#888;">No saved profiles</li>';
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
                el.innerHTML = '<p style="color:#888;">Pull settings from V1 to view</p>';
                return;
            }
            let html = '<div class="bytes-display">Bytes: ' + currentSettings.bytes.map(b=>'0x'+b.toString(16).toUpperCase().padStart(2,'0')).join(' ') + '</div>';
            html += '<div style="font-size:0.85rem;color:#aaa;max-height:300px;overflow-y:auto;">';
            
            // Group settings by category
            const bands = settingDefs.filter(s=>['xBand','kBand','kaBand','laser','kuBand'].includes(s.key));
            const sensitivity = settingDefs.filter(s=>s.key.includes('Sensitivity'));
            const features = settingDefs.filter(s=>!bands.includes(s) && !sensitivity.includes(s) && s.type==='toggle');
            const other = settingDefs.filter(s=>s.type==='select' && !sensitivity.includes(s));
            
            const renderGroup = (title, items) => {
                if(items.length === 0) return '';
                let g = '<div style="margin-top:8px;"><strong style="color:#888;">'+title+'</strong>';
                items.forEach(s=>{
                    const val = currentSettings[s.key];
                    if(s.type==='toggle'){
                        g += `<div style="margin-left:8px;">${s.label}: ${val?'✓':'✗'}</div>`;
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
</body>
</html>
)HTML";
    return html;
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
    server.send(200, "text/html", generateAutoPushHTML());
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
    
    Serial.printf("[SaveSlot] Slot %d - volume: %d (was %d), muteVol: %d (was %d)\n", 
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
    
    Serial.printf("[PushNow] Slot %d volumes - main: %d, mute: %d\n", slot, mainVol, muteVol);
    
    if (mainVol != 0xFF || muteVol != 0xFF) {
        delay(100);
        Serial.printf("[PushNow] Setting volume - main: %d, muted: %d\n", mainVol, muteVol);
        bleClient.setVolume(mainVol, muteVol);
    } else {
        Serial.println("[PushNow] Volume: No change");
    }
    
    // Update active slot and refresh display profile indicator
    settingsManager.setActiveSlot(slot);
    display.drawProfileIndicator(slot);
    
    server.send(200, "application/json", "{\"success\":true}");
}

String WiFiManager::generateAutoPushHTML() {
    const V1Settings& s = settingsManager.get();
    
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Auto-Push Profiles</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #1a1a2e;
            color: #eee;
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 800px; margin: 0 auto; }
        h1 { color: #e94560; margin-bottom: 10px; text-align: center; }
        .subtitle { text-align: center; color: #888; margin-bottom: 20px; font-size: 0.9em; }
        .card {
            background: #16213e;
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 20px;
        }
        .card h2 { color: #e94560; margin-bottom: 15px; font-size: 1.2em; }
        .slot-card {
            border: 2px solid #333;
            transition: border-color 0.3s;
        }
        .slot-card.active {
            border-color: #e94560;
            background: #1a2840;
        }
        .slot-header {
            display: flex;
            justify-content: space-between;
            align-items: center;
            margin-bottom: 15px;
        }
        .slot-title {
            display: flex;
            align-items: center;
            gap: 10px;
        }
        .slot-icon {
            width: 40px;
            height: 40px;
            background: #0f3460;
            border-radius: 8px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 1.5em;
        }
        .active-badge {
            background: #e94560;
            color: white;
            padding: 4px 12px;
            border-radius: 20px;
            font-size: 0.85em;
            font-weight: bold;
        }
        .form-group { margin-bottom: 15px; }
        label { display: block; margin-bottom: 5px; color: #888; font-size: 0.9em; }
        select, input {
            width: 100%;
            padding: 12px;
            border: none;
            border-radius: 8px;
            background: #0f3460;
            color: #eee;
            font-size: 1em;
        }
        .btn-group {
            display: flex;
            gap: 10px;
            margin-top: 15px;
        }
        .btn {
            flex: 1;
            padding: 12px;
            border: none;
            border-radius: 8px;
            font-size: 1em;
            cursor: pointer;
            font-weight: 500;
            transition: all 0.3s;
        }
        .btn-primary {
            background: #e94560;
            color: white;
        }
        .btn-primary:hover { background: #d63050; }
        .btn-success {
            background: #3a9104;
            color: white;
        }
        .btn-success:hover { background: #2d7003; }
        .btn-secondary {
            background: #333;
            color: #eee;
        }
        .btn-secondary:hover { background: #444; }
        .msg {
            padding: 12px;
            border-radius: 8px;
            margin-bottom: 20px;
            text-align: center;
        }
        .msg.success { background: #3a9104; color: white; }
        .msg.error { background: #e94560; color: white; }
        .quick-push {
            display: grid;
            grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
            gap: 15px;
            margin-bottom: 20px;
        }
        .quick-btn {
            padding: 20px;
            background: #16213e;
            border: 2px solid #333;
            border-radius: 12px;
            cursor: pointer;
            transition: all 0.3s;
            text-align: center;
        }
        .quick-btn:hover {
            border-color: #e94560;
            transform: translateY(-2px);
        }
        .quick-btn.active {
            border-color: #e94560;
            background: #1a2840;
        }
        .quick-btn-icon {
            font-size: 2em;
            margin-bottom: 10px;
        }
        .quick-btn-label {
            font-weight: bold;
            margin-bottom: 5px;
        }
        .quick-btn-sub {
            font-size: 0.85em;
            color: #888;
        }
        .auto-toggle {
            display: flex;
            align-items: center;
            gap: 10px;
            padding: 15px;
            background: #0f3460;
            border-radius: 8px;
            margin-bottom: 20px;
        }
        .auto-toggle input[type="checkbox"] {
            width: auto;
        }
        .nav-links {
            display: flex;
            gap: 10px;
            margin-bottom: 20px;
        }
        .nav-links a {
            flex: 1;
            padding: 12px;
            background: #333;
            color: #eee;
            text-decoration: none;
            border-radius: 8px;
            text-align: center;
            transition: background 0.3s;
        }
        .nav-links a:hover { background: #444; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎯 Auto-Push Profiles</h1>
        <p class="subtitle">Configure 3 quick-access profiles for different driving scenarios</p>
        
        <div class="nav-links">
            <a href="/">← Home</a>
            <a href="/v1settings">V1 Profiles</a>
            <a href="/settings">Settings</a>
        </div>
        
        <div id="message"></div>
        
        <div class="card">
            <h2>Quick Push</h2>
            <p style="color:#888;font-size:0.9em;margin-bottom:15px;">Click to activate and push a profile immediately</p>
            <div class="quick-push">
                <div class="quick-btn" id="quick-0" onclick="quickPush(0)">
                    <div class="quick-btn-icon">🏠</div>
                    <div class="quick-btn-label">)HTML" + htmlEscape(s.slot0Name) + R"HTML(</div>
                    <div class="quick-btn-sub" id="quick-sub-0">Not configured</div>
                </div>
                <div class="quick-btn" id="quick-1" onclick="quickPush(1)">
                    <div class="quick-btn-icon">🏎️</div>
                    <div class="quick-btn-label">)HTML" + htmlEscape(s.slot1Name) + R"HTML(</div>
                    <div class="quick-btn-sub" id="quick-sub-1">Not configured</div>
                </div>
                <div class="quick-btn" id="quick-2" onclick="quickPush(2)">
                    <div class="quick-btn-icon">👥</div>
                    <div class="quick-btn-label">)HTML" + htmlEscape(s.slot2Name) + R"HTML(</div>
                    <div class="quick-btn-sub" id="quick-sub-2">Not configured</div>
                </div>
            </div>
            <div class="auto-toggle">
                <input type="checkbox" id="auto-enable" )HTML" + String(s.autoPushEnabled ? "checked" : "") + R"HTML(>
                <label for="auto-enable" style="margin:0;flex:1;">Automatically push active profile on V1 connection</label>
            </div>
        </div>
        
        <div class="card slot-card" id="slot-0">
            <div class="slot-header">
                <div class="slot-title">
                    <div class="slot-icon">🏠</div>
                    <h2 style="margin:0;">Default Profile</h2>
                </div>
                <div class="active-badge" id="badge-0" style="display:none;">ACTIVE</div>
            </div>
            <div class="form-group">
                <label>Display Name</label>
                <input type="text" id="name-0" maxlength="20" placeholder="DEFAULT" style="text-transform:uppercase;">
            </div>
            <div class="form-group">
                <label>Display Color</label>
                <input type="color" id="color-0" value="#400050">
            </div>
            <div class="form-group">
                <label>Profile</label>
                <select id="profile-0">
                    <option value="">-- None --</option>
                </select>
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
                    <option value="1">1</option>
                    <option value="2">2</option>
                    <option value="3">3</option>
                    <option value="4">4</option>
                    <option value="5">5</option>
                    <option value="6">6</option>
                    <option value="7">7</option>
                    <option value="8">8</option>
                    <option value="9">9 (Max)</option>
                </select>
            </div>
            <div class="form-group">
                <label>Mute Volume</label>
                <select id="muteVol-0">
                    <option value="255">No Change</option>
                    <option value="0">0 (Silent)</option>
                    <option value="1">1</option>
                    <option value="2">2</option>
                    <option value="3">3</option>
                    <option value="4">4</option>
                    <option value="5">5</option>
                    <option value="6">6</option>
                    <option value="7">7</option>
                    <option value="8">8</option>
                    <option value="9">9 (Max)</option>
                </select>
            </div>
            <div class="btn-group">
                <button class="btn btn-primary" onclick="saveSlot(0)">Save</button>
                <button class="btn btn-success" onclick="pushSlot(0)">Push Now</button>
            </div>
        </div>
        
        <div class="card slot-card" id="slot-1">
            <div class="slot-header">
                <div class="slot-title">
                    <div class="slot-icon">🏎️</div>
                    <h2 style="margin:0;">Highway Profile</h2>
                </div>
                <div class="active-badge" id="badge-1" style="display:none;">ACTIVE</div>
            </div>
            <div class="form-group">
                <label>Display Name</label>
                <input type="text" id="name-1" maxlength="20" placeholder="HIGHWAY" style="text-transform:uppercase;">
            </div>
            <div class="form-group">
                <label>Display Color</label>
                <input type="color" id="color-1" value="#00fc00">
            </div>
            <div class="form-group">
                <label>Profile</label>
                <select id="profile-1">
                    <option value="">-- None --</option>
                </select>
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
                    <option value="1">1</option>
                    <option value="2">2</option>
                    <option value="3">3</option>
                    <option value="4">4</option>
                    <option value="5">5</option>
                    <option value="6">6</option>
                    <option value="7">7</option>
                    <option value="8">8</option>
                    <option value="9">9 (Max)</option>
                </select>
            </div>
            <div class="form-group">
                <label>Mute Volume</label>
                <select id="muteVol-1">
                    <option value="255">No Change</option>
                    <option value="0">0 (Silent)</option>
                    <option value="1">1</option>
                    <option value="2">2</option>
                    <option value="3">3</option>
                    <option value="4">4</option>
                    <option value="5">5</option>
                    <option value="6">6</option>
                    <option value="7">7</option>
                    <option value="8">8</option>
                    <option value="9">9 (Max)</option>
                </select>
            </div>
            <div class="btn-group">
                <button class="btn btn-primary" onclick="saveSlot(1)">Save</button>
                <button class="btn btn-success" onclick="pushSlot(1)">Push Now</button>
            </div>
        </div>
        
        <div class="card slot-card" id="slot-2">
            <div class="slot-header">
                <div class="slot-title">
                    <div class="slot-icon">👥</div>
                    <h2 style="margin:0;">Passenger Comfort Profile</h2>
                </div>
                <div class="active-badge" id="badge-2" style="display:none;">ACTIVE</div>
            </div>
            <div class="form-group">
                <label>Display Name</label>
                <input type="text" id="name-2" maxlength="20" placeholder="COMFORT" style="text-transform:uppercase;">
            </div>
            <div class="form-group">
                <label>Display Color</label>
                <input type="color" id="color-2" value="#808080">
            </div>
            <div class="form-group">
                <label>Profile</label>
                <select id="profile-2">
                    <option value="">-- None --</option>
                </select>
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
                    <option value="1">1</option>
                    <option value="2">2</option>
                    <option value="3">3</option>
                    <option value="4">4</option>
                    <option value="5">5</option>
                    <option value="6">6</option>
                    <option value="7">7</option>
                    <option value="8">8</option>
                    <option value="9">9 (Max)</option>
                </select>
            </div>
            <div class="form-group">
                <label>Mute Volume</label>
                <select id="muteVol-2">
                    <option value="255">No Change</option>
                    <option value="0">0 (Silent)</option>
                    <option value="1">1</option>
                    <option value="2">2</option>
                    <option value="3">3</option>
                    <option value="4">4</option>
                    <option value="5">5</option>
                    <option value="6">6</option>
                    <option value="7">7</option>
                    <option value="8">8</option>
                    <option value="9">9 (Max)</option>
                </select>
            </div>
            <div class="btn-group">
                <button class="btn btn-primary" onclick="saveSlot(2)">Save</button>
                <button class="btn btn-success" onclick="pushSlot(2)">Push Now</button>
            </div>
        </div>
    </div>
    
    <script>
        const settings = )HTML" + generateAutoPushSettingsJSON() + R"HTML(;
        
        // Convert RGB565 (16-bit) to HTML hex color
        function rgb565ToHex(rgb565) {
            const r = ((rgb565 >> 11) & 0x1F) << 3;
            const g = ((rgb565 >> 5) & 0x3F) << 2;
            const b = (rgb565 & 0x1F) << 3;
            return '#' + [r,g,b].map(x => x.toString(16).padStart(2,'0')).join('');
        }
        
        // Convert HTML hex color to RGB565
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
            // Load slot names
            document.getElementById('name-0').value = settings.slot0_name || 'DEFAULT';
            document.getElementById('name-1').value = settings.slot1_name || 'HIGHWAY';
            document.getElementById('name-2').value = settings.slot2_name || 'COMFORT';
            
            // Load slot colors (convert RGB565 to HTML hex)
            document.getElementById('color-0').value = rgb565ToHex(settings.slot0_color || 0x400A);
            document.getElementById('color-1').value = rgb565ToHex(settings.slot1_color || 0x07E0);
            document.getElementById('color-2').value = rgb565ToHex(settings.slot2_color || 0x8410);
            
            // Load slot configurations
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
            
            // Update active state
            updateActiveDisplay();
            updateQuickButtons();
        }
        
        function updateActiveDisplay() {
            for (let i = 0; i < 3; i++) {
                const card = document.getElementById('slot-'+i);
                const badge = document.getElementById('badge-'+i);
                if (i === settings.activeSlot) {
                    card.classList.add('active');
                    badge.style.display = 'block';
                } else {
                    card.classList.remove('active');
                    badge.style.display = 'none';
                }
            }
        }
        
        function updateQuickButtons() {
            const slots = [
                {name: settings.slot0_name, prof: settings.slot0_profile, mode: settings.slot0_mode},
                {name: settings.slot1_name, prof: settings.slot1_profile, mode: settings.slot1_mode},
                {name: settings.slot2_name, prof: settings.slot2_profile, mode: settings.slot2_mode}
            ];
            const defaultNames = ['DEFAULT', 'HIGHWAY', 'COMFORT'];
            
            for (let i = 0; i < 3; i++) {
                const btn = document.getElementById('quick-'+i);
                const label = btn.querySelector('.quick-btn-label');
                const sub = document.getElementById('quick-sub-'+i);
                
                // Update the label with custom name
                if (label) {
                    label.textContent = slots[i].name || defaultNames[i];
                }
                
                if (slots[i].prof) {
                    sub.textContent = slots[i].prof;
                    btn.style.opacity = '1';
                } else {
                    sub.textContent = 'Not configured';
                    btn.style.opacity = '0.5';
                }
                
                if (i === settings.activeSlot) {
                    btn.classList.add('active');
                } else {
                    btn.classList.remove('active');
                }
            }
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
                    showMessage('Slot ' + slot + ' saved!', false);
                    // Update local settings
                    if (slot === 0) {
                        settings.slot0_name = name;
                        settings.slot0_color = color;
                        settings.slot0_profile = profile;
                        settings.slot0_mode = parseInt(mode);
                        settings.slot0_volume = parseInt(volume);
                        settings.slot0_muteVol = parseInt(muteVol);
                    } else if (slot === 1) {
                        settings.slot1_name = name;
                        settings.slot1_color = color;
                        settings.slot1_profile = profile;
                        settings.slot1_mode = parseInt(mode);
                        settings.slot1_volume = parseInt(volume);
                        settings.slot1_muteVol = parseInt(muteVol);
                    } else {
                        settings.slot2_name = name;
                        settings.slot2_color = color;
                        settings.slot2_profile = profile;
                        settings.slot2_mode = parseInt(mode);
                        settings.slot2_volume = parseInt(volume);
                        settings.slot2_muteVol = parseInt(muteVol);
                    }
                    updateQuickButtons();
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        function pushSlot(slot) {
            // Get current form values (use these instead of saved settings)
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
                if (d.success) {
                    showMessage('Profile pushed to V1!', false);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        function quickPush(slot) {
            // Activate this slot and enable auto-push, then push
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
                    document.getElementById('auto-enable').checked = true;
                    updateActiveDisplay();
                    updateQuickButtons();
                    // Now push
                    pushSlot(slot);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        document.getElementById('auto-enable').addEventListener('change', function() {
            const data = new URLSearchParams();
            data.append('slot', settings.activeSlot);
            data.append('enable', this.checked ? 'true' : 'false');
            
            fetch('/api/autopush/activate', {
                method: 'POST',
                headers: {'Content-Type': 'application/x-www-form-urlencoded'},
                body: data
            }).then(r=>r.json()).then(d => {
                if (d.success) {
                    settings.autoPushEnabled = this.checked;
                    showMessage('Auto-push '+(this.checked?'enabled':'disabled'), false);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        });
        
        // Init
        loadProfiles();
    </script>
</body>
</html>
)HTML";
    return html;
}
// ============= Display Colors Handlers =============

void WiFiManager::handleDisplayColors() {
    server.send(200, "text/html", generateDisplayColorsHTML());
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
    
    String html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Display Colors</title>
    <style>
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: #1a1a2e;
            color: #eee;
            min-height: 100vh;
            padding: 20px;
        }
        .container { max-width: 600px; margin: 0 auto; }
        h1 { color: #e94560; margin-bottom: 10px; text-align: center; }
        .subtitle { text-align: center; color: #888; margin-bottom: 20px; font-size: 0.9em; }
        .card {
            background: #16213e;
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 20px;
        }
        .card h2 { color: #e94560; margin-bottom: 15px; font-size: 1.1em; }
        .nav-links {
            display: flex;
            justify-content: center;
            gap: 15px;
            margin-bottom: 20px;
            flex-wrap: wrap;
        }
        .nav-links a {
            color: #e94560;
            text-decoration: none;
            padding: 8px 16px;
            background: #16213e;
            border-radius: 8px;
            font-size: 0.9em;
        }
        .nav-links a:hover { background: #1f2b4a; }
        .form-group {
            margin-bottom: 15px;
        }
        .form-group label {
            display: block;
            margin-bottom: 5px;
            font-weight: 500;
        }
        .color-row {
            display: flex;
            align-items: center;
            gap: 15px;
        }
        .color-row input[type="color"] {
            width: 60px;
            height: 40px;
            border: none;
            border-radius: 8px;
            cursor: pointer;
        }
        .color-row .color-label {
            flex: 1;
            font-size: 1em;
        }
        .color-row .color-preview {
            width: 100px;
            height: 40px;
            border-radius: 8px;
            display: flex;
            align-items: center;
            justify-content: center;
            font-weight: bold;
            font-size: 1.2em;
        }
        .btn {
            width: 100%;
            padding: 15px;
            border: none;
            border-radius: 8px;
            font-size: 1.1em;
            cursor: pointer;
            font-weight: 600;
            transition: all 0.3s;
            margin-top: 10px;
        }
        .btn-primary {
            background: #e94560;
            color: white;
        }
        .btn-primary:hover { background: #d63050; }
        .msg {
            padding: 12px;
            border-radius: 8px;
            margin-bottom: 15px;
            text-align: center;
        }
        .msg.success { background: #0f5132; color: #d1e7dd; }
        .msg.error { background: #842029; color: #f8d7da; }
    </style>
</head>
<body>
    <div class="container">
        <h1>🎨 Display Colors</h1>
        <p class="subtitle">Customize the colors shown on your V1 display</p>
        
        <div class="nav-links">
            <a href="/">← Home</a>
            <a href="/autopush">Auto-Push</a>
            <a href="/v1settings">V1 Profiles</a>
            <a href="/settings">Settings</a>
        </div>
        
        <div id="message"></div>
        
        <div class="card">
            <h2>🔢 Counter & Frequency</h2>
            <div class="form-group">
                <label>Bogey Counter</label>
                <div class="color-row">
                    <input type="color" id="color-bogey" onchange="updatePreview('bogey')">
                    <div class="color-preview" id="preview-bogey">1.</div>
                </div>
            </div>
            <div class="form-group">
                <label>Frequency Display</label>
                <div class="color-row">
                    <input type="color" id="color-freq" onchange="updatePreview('freq')">
                    <div class="color-preview" id="preview-freq">35.5</div>
                </div>
            </div>
        </div>
        
        <div class="card">
            <h2>📡 Band Indicators</h2>
            <div class="form-group">
                <label>Laser (L)</label>
                <div class="color-row">
                    <input type="color" id="color-bandL" onchange="updatePreview('bandL')">
                    <div class="color-preview" id="preview-bandL">L</div>
                </div>
            </div>
            <div class="form-group">
                <label>Ka Band</label>
                <div class="color-row">
                    <input type="color" id="color-bandKa" onchange="updatePreview('bandKa')">
                    <div class="color-preview" id="preview-bandKa">Ka</div>
                </div>
            </div>
            <div class="form-group">
                <label>K Band</label>
                <div class="color-row">
                    <input type="color" id="color-bandK" onchange="updatePreview('bandK')">
                    <div class="color-preview" id="preview-bandK">K</div>
                </div>
            </div>
            <div class="form-group">
                <label>X Band</label>
                <div class="color-row">
                    <input type="color" id="color-bandX" onchange="updatePreview('bandX')">
                    <div class="color-preview" id="preview-bandX">X</div>
                </div>
            </div>
        </div>
        
        <div class="card">
            <h2>➡️ Direction Arrows</h2>
            <div class="form-group">
                <label>Arrow Color</label>
                <div class="color-row">
                    <input type="color" id="color-arrow" onchange="updatePreview('arrow')">
                    <div class="color-preview" id="preview-arrow">▲▼</div>
                </div>
            </div>
        </div>
        
        <button class="btn btn-primary" onclick="saveColors()">Save Colors</button>
        <button class="btn" style="background:#555;margin-top:10px;" onclick="resetDefaults()">Reset to Default Colors</button>
    </div>
    
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
        
        // Convert RGB565 to HTML hex
        function rgb565ToHex(rgb565) {
            const r = ((rgb565 >> 11) & 0x1F) << 3;
            const g = ((rgb565 >> 5) & 0x3F) << 2;
            const b = (rgb565 & 0x1F) << 3;
            return '#' + [r,g,b].map(x => x.toString(16).padStart(2,'0')).join('');
        }
        
        // Convert HTML hex to RGB565
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
                if (d.success) {
                    showMessage('Colors saved! Display will update on next alert.', false);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        function resetDefaults() {
            if (!confirm('Reset all display colors to defaults?')) return;
            fetch('/api/displaycolors/reset', {method:'POST'})
            .then(r=>r.json()).then(d => {
                if (d.success) {
                    colors.bogey = 0xF800;
                    colors.freq = 0xF800;
                    colors.arrow = 0xF800;
                    colors.bandL = 0x001F;
                    colors.bandKa = 0xF800;
                    colors.bandK = 0x001F;
                    colors.bandX = 0x07E0;
                    loadColors();
                    showMessage('Colors reset to defaults!', false);
                } else {
                    showMessage('Error: '+(d.error||'Unknown'), true);
                }
            }).catch(e => showMessage('Error: '+e, true));
        }
        
        // Init
        loadColors();
    </script>
</body>
</html>
)HTML";
    return html;
}