#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <functional>
#include <vector>

namespace WifiV1ProfileApiService {

struct ProfileSummary {
    String name;
    String description;
    bool displayOn = true;
};

struct Runtime {
    std::function<std::vector<String>()> listProfileNames;
    std::function<bool(const String&, ProfileSummary&)> loadProfileSummary;
    std::function<bool(const String&, String&)> loadProfileJson;
    std::function<bool(const String&, uint8_t outBytes[6], bool& displayOn)> loadProfileSettings;
    std::function<bool(const JsonObject&, uint8_t outBytes[6])> parseSettingsJson;
    std::function<bool(const String&,
                       const String&,
                       bool,
                       const uint8_t inBytes[6],
                       String&)> saveProfile;
    std::function<bool(const String&)> deleteProfile;
    std::function<bool()> requestUserBytes;
    std::function<bool(const uint8_t inBytes[6])> writeUserBytes;
    std::function<void(bool)> setDisplayOn;
    std::function<bool()> hasCurrentSettings;
    std::function<String()> currentSettingsJson;
    std::function<bool()> v1Connected;
    std::function<void()> backupToSd;
};

void handleApiProfilesList(WebServer& server, const Runtime& runtime);

void handleApiProfileGet(WebServer& server, const Runtime& runtime);

void handleApiProfileSave(WebServer& server,
                          const Runtime& runtime,
                          const std::function<bool()>& checkRateLimit);

void handleApiProfileDelete(WebServer& server,
                            const Runtime& runtime,
                            const std::function<bool()>& checkRateLimit);

void handleApiCurrentSettings(WebServer& server, const Runtime& runtime);

void handleApiSettingsPull(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit);

void handleApiSettingsPush(WebServer& server,
                           const Runtime& runtime,
                           const std::function<bool()>& checkRateLimit);

}  // namespace WifiV1ProfileApiService
