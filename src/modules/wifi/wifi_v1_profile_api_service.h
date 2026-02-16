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

void handleProfilesList(WebServer& server, const Runtime& runtime);

void handleProfileGet(WebServer& server, const Runtime& runtime);

void handleProfileSave(WebServer& server,
                       const Runtime& runtime,
                       const std::function<bool()>& checkRateLimit);

void handleProfileDelete(WebServer& server,
                         const Runtime& runtime,
                         const std::function<bool()>& checkRateLimit);

void handleCurrentSettings(WebServer& server, const Runtime& runtime);

void handleSettingsPull(WebServer& server,
                        const Runtime& runtime,
                        const std::function<bool()>& checkRateLimit);

void handleSettingsPush(WebServer& server,
                        const Runtime& runtime,
                        const std::function<bool()>& checkRateLimit);

inline void handleApiProfilesList(WebServer& server, const Runtime& runtime) {
    handleProfilesList(server, runtime);
}

inline void handleApiProfileGet(WebServer& server, const Runtime& runtime) {
    handleProfileGet(server, runtime);
}

inline void handleApiProfileSave(WebServer& server,
                                 const Runtime& runtime,
                                 const std::function<bool()>& checkRateLimit) {
    handleProfileSave(server, runtime, checkRateLimit);
}

inline void handleApiProfileDelete(WebServer& server,
                                   const Runtime& runtime,
                                   const std::function<bool()>& checkRateLimit) {
    handleProfileDelete(server, runtime, checkRateLimit);
}

inline void handleApiCurrentSettings(WebServer& server, const Runtime& runtime) {
    handleCurrentSettings(server, runtime);
}

inline void handleApiSettingsPull(WebServer& server,
                                  const Runtime& runtime,
                                  const std::function<bool()>& checkRateLimit) {
    handleSettingsPull(server, runtime, checkRateLimit);
}

inline void handleApiSettingsPush(WebServer& server,
                                  const Runtime& runtime,
                                  const std::function<bool()>& checkRateLimit) {
    handleSettingsPush(server, runtime, checkRateLimit);
}

}  // namespace WifiV1ProfileApiService
