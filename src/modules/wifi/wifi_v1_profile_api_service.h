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

// Backward-compatible aliases for non-Api symbol names.
inline void handleProfilesList(WebServer& server, const Runtime& runtime) {
    handleApiProfilesList(server, runtime);
}

inline void handleProfileGet(WebServer& server, const Runtime& runtime) {
    handleApiProfileGet(server, runtime);
}

inline void handleProfileSave(WebServer& server,
                              const Runtime& runtime,
                              const std::function<bool()>& checkRateLimit) {
    handleApiProfileSave(server, runtime, checkRateLimit);
}

inline void handleProfileDelete(WebServer& server,
                                const Runtime& runtime,
                                const std::function<bool()>& checkRateLimit) {
    handleApiProfileDelete(server, runtime, checkRateLimit);
}

inline void handleCurrentSettings(WebServer& server, const Runtime& runtime) {
    handleApiCurrentSettings(server, runtime);
}

inline void handleSettingsPull(WebServer& server,
                               const Runtime& runtime,
                               const std::function<bool()>& checkRateLimit) {
    handleApiSettingsPull(server, runtime, checkRateLimit);
}

inline void handleSettingsPush(WebServer& server,
                               const Runtime& runtime,
                               const std::function<bool()>& checkRateLimit) {
    handleApiSettingsPush(server, runtime, checkRateLimit);
}

}  // namespace WifiV1ProfileApiService
