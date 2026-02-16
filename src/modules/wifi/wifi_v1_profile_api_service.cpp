#include "wifi_v1_profile_api_service.h"

#include <ArduinoJson.h>
#include <cstring>

#ifdef UNIT_TEST
#include <string>
#endif

namespace WifiV1ProfileApiService {

namespace {

void sendJsonDocument(WebServer& server, int statusCode, const JsonDocument& doc) {
#ifdef UNIT_TEST
    std::string response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response.c_str());
#else
    String response;
    serializeJson(doc, response);
    server.send(statusCode, "application/json", response);
#endif
}

}  // namespace

void handleProfilesList(WebServer& server, const Runtime& runtime) {
    std::vector<String> profileNames;
    if (runtime.listProfileNames) {
        profileNames = runtime.listProfileNames();
    }
    Serial.printf("[V1Profiles] Listing %d profiles\n", profileNames.size());

    JsonDocument doc;
    JsonArray array = doc["profiles"].to<JsonArray>();

    for (const String& name : profileNames) {
        ProfileSummary profile;
        if (runtime.loadProfileSummary && runtime.loadProfileSummary(name, profile)) {
            JsonObject obj = array.add<JsonObject>();
            obj["name"] = profile.name;
            obj["description"] = profile.description;
            obj["displayOn"] = profile.displayOn;
            Serial.printf("[V1Profiles]   - %s: %s\n", profile.name.c_str(), profile.description.c_str());
        }
    }

    sendJsonDocument(server, 200, doc);
}

void handleProfileGet(WebServer& server, const Runtime& runtime) {
    if (!server.hasArg("name")) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    String name = server.arg("name");
    String profileJson;
    if (!runtime.loadProfileJson || !runtime.loadProfileJson(name, profileJson)) {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
        return;
    }

    server.send(200, "application/json", profileJson);
}

void handleProfileSave(WebServer& server,
                       const Runtime& runtime,
                       const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    String body = server.arg("plain");
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }
    Serial.printf("[V1Settings] Save request body: %s\n", body.c_str());

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.c_str());

    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    if (!runtime.parseSettingsJson || !runtime.saveProfile) {
        server.send(500, "application/json", "{\"error\":\"Profile persistence unavailable\"}");
        return;
    }

    const String description = doc["description"] | "";
    const bool displayOn = doc["displayOn"] | true;  // Default to on
    uint8_t settingsBytes[6];
    memset(settingsBytes, 0xFF, sizeof(settingsBytes));

    // Parse settings from JSON
    JsonObject settingsObj = doc["settings"];
    if (!settingsObj.isNull()) {
        if (!runtime.parseSettingsJson(settingsObj, settingsBytes)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    } else {
        // Direct settings in root
        JsonObject rootObj = doc.as<JsonObject>();
        if (!runtime.parseSettingsJson(rootObj, settingsBytes)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
    }

    String saveError;
    if (runtime.saveProfile(name, description, displayOn, settingsBytes, saveError)) {
        if (runtime.backupToSd) {
            runtime.backupToSd();
        }
        Serial.printf("[V1Profiles] Profile '%s' saved successfully\n", name.c_str());
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        Serial.printf("[V1Profiles] Failed to save profile '%s': %s\n", name.c_str(), saveError.c_str());
        String errorJson = String("{\"error\":\"") + saveError + "\"}";
        server.send(500, "application/json", errorJson);
    }
}

void handleProfileDelete(WebServer& server,
                         const Runtime& runtime,
                         const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

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
    DeserializationError err = deserializeJson(doc, body.c_str());
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String name = doc["name"] | "";
    if (name.isEmpty()) {
        server.send(400, "application/json", "{\"error\":\"Missing profile name\"}");
        return;
    }

    if (!runtime.deleteProfile) {
        server.send(500, "application/json", "{\"error\":\"Profile persistence unavailable\"}");
        return;
    }

    if (runtime.deleteProfile(name)) {
        if (runtime.backupToSd) {
            runtime.backupToSd();
        }
        server.send(200, "application/json", "{\"success\":true}");
    } else {
        server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
    }
}

void handleCurrentSettings(WebServer& server, const Runtime& runtime) {
    JsonDocument doc;
    doc["connected"] = runtime.v1Connected ? runtime.v1Connected() : false;

    if (!runtime.hasCurrentSettings || !runtime.hasCurrentSettings()) {
        doc["available"] = false;
        sendJsonDocument(server, 200, doc);
        return;
    }

    doc["available"] = true;
    // Parse existing settings JSON and embed it
    if (runtime.currentSettingsJson) {
        JsonDocument settingsDoc;
        String settingsJson = runtime.currentSettingsJson();
        deserializeJson(settingsDoc, settingsJson.c_str());
        doc["settings"] = settingsDoc;
    }

    sendJsonDocument(server, 200, doc);
}

void handleSettingsPull(WebServer& server,
                        const Runtime& runtime,
                        const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.v1Connected || !runtime.v1Connected()) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }

    bool requested = false;
    if (runtime.requestUserBytes) {
        requested = runtime.requestUserBytes();
    }
    if (requested) {
        // Response will come async via BLE callback
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Request sent. Check current settings.\"}");
    } else {
        server.send(500, "application/json", "{\"error\":\"Failed to send request\"}");
    }
}

void handleSettingsPush(WebServer& server,
                        const Runtime& runtime,
                        const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.v1Connected || !runtime.v1Connected()) {
        server.send(503, "application/json", "{\"error\":\"V1 not connected\"}");
        return;
    }

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"Missing request body\"}");
        return;
    }

    String body = server.arg("plain");
    Serial.printf("[V1Settings] Push request: %s\n", body.c_str());
    if (body.length() > 4096) {
        server.send(400, "application/json", "{\"error\":\"Payload too large\"}");
        return;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body.c_str());
    if (err) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    uint8_t bytes[6];
    bool displayOn = true;

    // Check if pushing a profile by name
    String profileName = doc["name"] | "";
    if (!profileName.isEmpty()) {
        if (!runtime.loadProfileSettings ||
            !runtime.loadProfileSettings(profileName, bytes, displayOn)) {
            server.send(404, "application/json", "{\"error\":\"Profile not found\"}");
            return;
        }
        Serial.printf("[V1Settings] Pushing profile '%s': %02X %02X %02X %02X %02X %02X\n",
                      profileName.c_str(),
                      bytes[0],
                      bytes[1],
                      bytes[2],
                      bytes[3],
                      bytes[4],
                      bytes[5]);
    }
    // Check for bytes array
    else if (doc["bytes"].is<JsonArray>()) {
        JsonArray bytesArray = doc["bytes"];
        if (bytesArray.size() != 6) {
            server.send(400, "application/json", "{\"error\":\"Invalid bytes array\"}");
            return;
        }
        for (int i = 0; i < 6; i++) {
            bytes[i] = bytesArray[i].as<uint8_t>();
        }
        displayOn = doc["displayOn"] | true;
        Serial.println("[V1Settings] Using raw bytes from request");
    }
    // Parse from individual settings
    else {
        JsonObject settingsObj = doc["settings"].as<JsonObject>();
        if (settingsObj.isNull()) {
            settingsObj = doc.as<JsonObject>();
        }
        if (!runtime.parseSettingsJson || !runtime.parseSettingsJson(settingsObj, bytes)) {
            server.send(400, "application/json", "{\"error\":\"Invalid settings\"}");
            return;
        }
        displayOn = doc["displayOn"] | true;
        Serial.printf("[V1Settings] Built bytes from settings: %02X %02X %02X %02X %02X %02X\n",
                      bytes[0],
                      bytes[1],
                      bytes[2],
                      bytes[3],
                      bytes[4],
                      bytes[5]);
    }

    bool writeOk = false;
    if (runtime.writeUserBytes) {
        writeOk = runtime.writeUserBytes(bytes);
    }
    if (writeOk) {
        Serial.println("[V1Settings] Push sent successfully");
        if (runtime.setDisplayOn) {
            runtime.setDisplayOn(displayOn);
        }
        server.send(200, "application/json", "{\"success\":true,\"message\":\"Settings sent to V1\"}");
    } else {
        Serial.println("[V1Settings] Push FAILED - write command rejected");
        server.send(500, "application/json", "{\"error\":\"Write command failed - check V1 connection\"}");
    }
}

}  // namespace WifiV1ProfileApiService
