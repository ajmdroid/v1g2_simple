#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstdint>
#include <vector>

namespace WifiV1ProfileApiService {

struct ProfileSummary {
    String name;
    String description;
    bool displayOn = true;
};

struct Runtime {
    std::vector<String> (*listProfileNames)(void* ctx);
    void* listProfileNamesCtx;
    bool (*loadProfileSummary)(const String& name, ProfileSummary& summary, void* ctx);
    void* loadProfileSummaryCtx;
    bool (*loadProfileJson)(const String& name, String& json, void* ctx);
    void* loadProfileJsonCtx;
    bool (*loadProfileSettings)(const String& name, uint8_t outBytes[6], bool& displayOn, void* ctx);
    void* loadProfileSettingsCtx;
    bool (*parseSettingsJson)(const JsonObject& settingsObj, uint8_t outBytes[6], void* ctx);
    void* parseSettingsJsonCtx;
    bool (*saveProfile)(const String& name,
                        const String& description,
                        bool displayOn,
                        const uint8_t inBytes[6],
                        String& error,
                        void* ctx);
    void* saveProfileCtx;
    bool (*deleteProfile)(const String& name, void* ctx);
    void* deleteProfileCtx;
    bool (*requestUserBytes)(void* ctx);
    void* requestUserBytesCtx;
    bool (*writeUserBytes)(const uint8_t inBytes[6], void* ctx);
    void* writeUserBytesCtx;
    void (*setDisplayOn)(bool displayOn, void* ctx);
    void* setDisplayOnCtx;
    bool (*hasCurrentSettings)(void* ctx);
    void* hasCurrentSettingsCtx;
    String (*currentSettingsJson)(void* ctx);
    void* currentSettingsJsonCtx;
    bool (*v1Connected)(void* ctx);
    void* v1ConnectedCtx;
    void (*backupToSd)(void* ctx);
    void* backupToSdCtx;
};

void handleApiProfilesList(WebServer& server, const Runtime& runtime);

void handleApiProfileGet(WebServer& server, const Runtime& runtime);

void handleApiProfileSave(WebServer& server,
                          const Runtime& runtime,
                          bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiProfileDelete(WebServer& server,
                            const Runtime& runtime,
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiCurrentSettings(WebServer& server, const Runtime& runtime);

void handleApiSettingsPull(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiSettingsPush(WebServer& server,
                           const Runtime& runtime,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

}  // namespace WifiV1ProfileApiService
