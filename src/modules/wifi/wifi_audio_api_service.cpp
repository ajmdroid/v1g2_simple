#include "wifi_audio_api_service.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "wifi_api_response.h"

namespace WifiAudioApiService {

void handleApiGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& settings = runtime.getSettings();

    JsonDocument doc;
    doc["voiceAlertMode"] = static_cast<int>(settings.voiceAlertMode);
    doc["voiceDirectionEnabled"] = settings.voiceDirectionEnabled;
    doc["announceBogeyCount"] = settings.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = settings.muteVoiceIfVolZero;
    doc["voiceVolume"] = settings.voiceVolume;
    doc["announceSecondaryAlerts"] = settings.announceSecondaryAlerts;
    doc["secondaryLaser"] = settings.secondaryLaser;
    doc["secondaryKa"] = settings.secondaryKa;
    doc["secondaryK"] = settings.secondaryK;
    doc["secondaryX"] = settings.secondaryX;
    doc["alertVolumeFadeEnabled"] = settings.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = settings.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = settings.alertVolumeFadeVolume;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

void handleApiSave(WebServer& server,
                   const Runtime& runtime,
                   const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.getMutableSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/audio/settings");

    auto argBool = [&server](const char* key, bool fallback) -> bool {
        if (!server.hasArg(key)) return fallback;
        return server.arg(key) == "true" || server.arg(key) == "1";
    };

    V1Settings& settings = runtime.getMutableSettings();

    if (server.hasArg("voiceAlertMode")) {
        int mode = server.arg("voiceAlertMode").toInt();
        mode = std::max(0, std::min(mode, 3));
        settings.voiceAlertMode = static_cast<VoiceAlertMode>(mode);
    }
    if (server.hasArg("voiceDirectionEnabled")) {
        settings.voiceDirectionEnabled =
            argBool("voiceDirectionEnabled", settings.voiceDirectionEnabled);
    }
    if (server.hasArg("announceBogeyCount")) {
        settings.announceBogeyCount =
            argBool("announceBogeyCount", settings.announceBogeyCount);
    }
    if (server.hasArg("muteVoiceIfVolZero")) {
        settings.muteVoiceIfVolZero =
            argBool("muteVoiceIfVolZero", settings.muteVoiceIfVolZero);
    }
    if (server.hasArg("voiceVolume")) {
        int volume = server.arg("voiceVolume").toInt();
        volume = std::max(0, std::min(volume, 100));
        settings.voiceVolume = static_cast<uint8_t>(volume);
        if (runtime.setAudioVolume) {
            runtime.setAudioVolume(static_cast<uint8_t>(volume));
        }
    }
    if (server.hasArg("announceSecondaryAlerts")) {
        settings.announceSecondaryAlerts =
            argBool("announceSecondaryAlerts", settings.announceSecondaryAlerts);
    }
    if (server.hasArg("secondaryLaser")) {
        settings.secondaryLaser = argBool("secondaryLaser", settings.secondaryLaser);
    }
    if (server.hasArg("secondaryKa")) {
        settings.secondaryKa = argBool("secondaryKa", settings.secondaryKa);
    }
    if (server.hasArg("secondaryK")) {
        settings.secondaryK = argBool("secondaryK", settings.secondaryK);
    }
    if (server.hasArg("secondaryX")) {
        settings.secondaryX = argBool("secondaryX", settings.secondaryX);
    }
    if (server.hasArg("alertVolumeFadeEnabled")) {
        settings.alertVolumeFadeEnabled =
            argBool("alertVolumeFadeEnabled", settings.alertVolumeFadeEnabled);
    }
    if (server.hasArg("alertVolumeFadeDelaySec")) {
        int delaySec = server.arg("alertVolumeFadeDelaySec").toInt();
        settings.alertVolumeFadeDelaySec =
            static_cast<uint8_t>(std::max(1, std::min(delaySec, 10)));
    }
    if (server.hasArg("alertVolumeFadeVolume")) {
        int fadeVolume = server.arg("alertVolumeFadeVolume").toInt();
        settings.alertVolumeFadeVolume =
            static_cast<uint8_t>(std::max(0, std::min(fadeVolume, 9)));
    }

    if (runtime.persistSettings) {
        runtime.persistSettings();
    }

    server.send(200, "application/json", "{\"success\":true}");
}

}  // namespace WifiAudioApiService
