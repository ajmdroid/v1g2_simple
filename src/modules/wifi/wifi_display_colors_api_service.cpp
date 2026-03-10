#include "wifi_display_colors_api_service.h"

#include <ArduinoJson.h>

#include <algorithm>

#include "../gps/gps_lockout_safety.h"
#include "../../../include/clamp_utils.h"
#include "wifi_api_response.h"

namespace WifiDisplayColorsApiService {

void handleApiSave(WebServer& server,
                   const Runtime& runtime,
                   const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.getMutableSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    Serial.println("[HTTP] POST /api/displaycolors");
#ifndef UNIT_TEST
    Serial.printf("[HTTP] Args count: %d\n", server.args());
    for (int i = 0; i < server.args(); i++) {
        Serial.printf("[HTTP] Arg %s = %s\n", server.argName(i).c_str(), server.arg(i).c_str());
    }
#endif

    V1Settings& s = runtime.getMutableSettings();

    auto argBool = [&server](const char* key, bool fallback) -> bool {
        if (!server.hasArg(key)) return fallback;
        return server.arg(key) == "true" || server.arg(key) == "1";
    };

    // Main display colors
    if (server.hasArg("bogey") || server.hasArg("freq") || server.hasArg("arrowFront") ||
        server.hasArg("arrowSide") || server.hasArg("arrowRear") || server.hasArg("bandL") ||
        server.hasArg("bandKa") || server.hasArg("bandK") || server.hasArg("bandX")) {
        s.colorBogey = server.hasArg("bogey") ? server.arg("bogey").toInt() : s.colorBogey;
        s.colorFrequency = server.hasArg("freq") ? server.arg("freq").toInt() : s.colorFrequency;
        s.colorArrowFront = server.hasArg("arrowFront") ? server.arg("arrowFront").toInt() : s.colorArrowFront;
        s.colorArrowSide = server.hasArg("arrowSide") ? server.arg("arrowSide").toInt() : s.colorArrowSide;
        s.colorArrowRear = server.hasArg("arrowRear") ? server.arg("arrowRear").toInt() : s.colorArrowRear;
        s.colorBandL = server.hasArg("bandL") ? server.arg("bandL").toInt() : s.colorBandL;
        s.colorBandKa = server.hasArg("bandKa") ? server.arg("bandKa").toInt() : s.colorBandKa;
        s.colorBandK = server.hasArg("bandK") ? server.arg("bandK").toInt() : s.colorBandK;
        s.colorBandX = server.hasArg("bandX") ? server.arg("bandX").toInt() : s.colorBandX;

        Serial.printf("[HTTP] Saving colors: bogey=%d freq=%d arrowF=%d arrowS=%d arrowR=%d\n",
                      s.colorBogey,
                      s.colorFrequency,
                      s.colorArrowFront,
                      s.colorArrowSide,
                      s.colorArrowRear);
    }

    // Color groups
    if (server.hasArg("wifiIcon")) s.colorWiFiIcon = server.arg("wifiIcon").toInt();
    if (server.hasArg("wifiConnected")) s.colorWiFiConnected = server.arg("wifiConnected").toInt();
    if (server.hasArg("bleConnected")) s.colorBleConnected = server.arg("bleConnected").toInt();
    if (server.hasArg("bleDisconnected")) s.colorBleDisconnected = server.arg("bleDisconnected").toInt();
    if (server.hasArg("bar1")) s.colorBar1 = server.arg("bar1").toInt();
    if (server.hasArg("bar2")) s.colorBar2 = server.arg("bar2").toInt();
    if (server.hasArg("bar3")) s.colorBar3 = server.arg("bar3").toInt();
    if (server.hasArg("bar4")) s.colorBar4 = server.arg("bar4").toInt();
    if (server.hasArg("bar5")) s.colorBar5 = server.arg("bar5").toInt();
    if (server.hasArg("bar6")) s.colorBar6 = server.arg("bar6").toInt();
    if (server.hasArg("muted")) s.colorMuted = server.arg("muted").toInt();
    if (server.hasArg("bandPhoto")) s.colorBandPhoto = server.arg("bandPhoto").toInt();
    if (server.hasArg("persisted")) s.colorPersisted = server.arg("persisted").toInt();
    if (server.hasArg("volumeMain")) s.colorVolumeMain = server.arg("volumeMain").toInt();
    if (server.hasArg("volumeMute")) s.colorVolumeMute = server.arg("volumeMute").toInt();
    if (server.hasArg("rssiV1")) s.colorRssiV1 = server.arg("rssiV1").toInt();
    if (server.hasArg("rssiProxy")) s.colorRssiProxy = server.arg("rssiProxy").toInt();
    if (server.hasArg("lockout")) s.colorLockout = server.arg("lockout").toInt();
    if (server.hasArg("gps")) s.colorGps = server.arg("gps").toInt();

    // Display toggles
    if (server.hasArg("freqUseBandColor")) s.freqUseBandColor = argBool("freqUseBandColor", s.freqUseBandColor);
    if (server.hasArg("hideWifiIcon")) s.hideWifiIcon = argBool("hideWifiIcon", s.hideWifiIcon);
    if (server.hasArg("hideProfileIndicator")) s.hideProfileIndicator = argBool("hideProfileIndicator", s.hideProfileIndicator);
    if (server.hasArg("hideBatteryIcon")) s.hideBatteryIcon = argBool("hideBatteryIcon", s.hideBatteryIcon);
    if (server.hasArg("showBatteryPercent")) s.showBatteryPercent = argBool("showBatteryPercent", s.showBatteryPercent);
    if (server.hasArg("hideBleIcon")) s.hideBleIcon = argBool("hideBleIcon", s.hideBleIcon);
    if (server.hasArg("hideVolumeIndicator")) s.hideVolumeIndicator = argBool("hideVolumeIndicator", s.hideVolumeIndicator);
    if (server.hasArg("hideRssiIndicator")) s.hideRssiIndicator = argBool("hideRssiIndicator", s.hideRssiIndicator);

    // Development/runtime toggles
    if (server.hasArg("enableWifiAtBoot")) s.enableWifiAtBoot = argBool("enableWifiAtBoot", s.enableWifiAtBoot);
    if (server.hasArg("enableSignalTraceLogging")) {
        s.enableSignalTraceLogging = argBool("enableSignalTraceLogging", s.enableSignalTraceLogging);
    }
    if (server.hasArg("gpsEnabled")) {
        s.gpsEnabled = argBool("gpsEnabled", s.gpsEnabled);
        if (runtime.setGpsRuntimeEnabled) {
            runtime.setGpsRuntimeEnabled(s.gpsEnabled);
        }
        if (runtime.setSpeedSourceGpsEnabled) {
            runtime.setSpeedSourceGpsEnabled(s.gpsEnabled);
        }
    }
    if (server.hasArg("gpsLockoutMode")) {
        s.gpsLockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("gpsLockoutMode"), s.gpsLockoutMode);
    }
    if (server.hasArg("gpsLockoutCoreGuardEnabled")) {
        s.gpsLockoutCoreGuardEnabled =
            argBool("gpsLockoutCoreGuardEnabled", s.gpsLockoutCoreGuardEnabled);
    }
    if (server.hasArg("gpsLockoutMaxQueueDrops")) {
        s.gpsLockoutMaxQueueDrops =
            clamp_utils::clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxPerfDrops")) {
        s.gpsLockoutMaxPerfDrops =
            clamp_utils::clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
    }
    if (server.hasArg("gpsLockoutMaxEventBusDrops")) {
        s.gpsLockoutMaxEventBusDrops =
            clamp_utils::clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
    }

    // Voice settings
    if (server.hasArg("voiceAlertMode")) {
        int mode = server.arg("voiceAlertMode").toInt();
        mode = std::max(0, std::min(mode, 3));
        s.voiceAlertMode = static_cast<VoiceAlertMode>(mode);
    }
    if (server.hasArg("voiceDirectionEnabled")) s.voiceDirectionEnabled = argBool("voiceDirectionEnabled", s.voiceDirectionEnabled);
    if (server.hasArg("announceBogeyCount")) s.announceBogeyCount = argBool("announceBogeyCount", s.announceBogeyCount);
    if (server.hasArg("muteVoiceIfVolZero")) s.muteVoiceIfVolZero = argBool("muteVoiceIfVolZero", s.muteVoiceIfVolZero);

    // Secondary alerts
    if (server.hasArg("announceSecondaryAlerts")) s.announceSecondaryAlerts = argBool("announceSecondaryAlerts", s.announceSecondaryAlerts);
    if (server.hasArg("secondaryLaser")) s.secondaryLaser = argBool("secondaryLaser", s.secondaryLaser);
    if (server.hasArg("secondaryKa")) s.secondaryKa = argBool("secondaryKa", s.secondaryKa);
    if (server.hasArg("secondaryK")) s.secondaryK = argBool("secondaryK", s.secondaryK);
    if (server.hasArg("secondaryX")) s.secondaryX = argBool("secondaryX", s.secondaryX);

    // Volume fade
    if (server.hasArg("alertVolumeFadeEnabled")) s.alertVolumeFadeEnabled = argBool("alertVolumeFadeEnabled", s.alertVolumeFadeEnabled);
    if (server.hasArg("alertVolumeFadeDelaySec")) {
        int val = server.arg("alertVolumeFadeDelaySec").toInt();
        s.alertVolumeFadeDelaySec = static_cast<uint8_t>(std::max(1, std::min(val, 10)));
    }
    if (server.hasArg("alertVolumeFadeVolume")) {
        int val = server.arg("alertVolumeFadeVolume").toInt();
        s.alertVolumeFadeVolume = static_cast<uint8_t>(std::max(0, std::min(val, 9)));
    }

    // Misc sliders
    if (server.hasArg("brightness")) {
        int brightness = server.arg("brightness").toInt();
        brightness = std::max(0, std::min(brightness, 255));
        s.brightness = static_cast<uint8_t>(brightness);
        if (runtime.setDisplayBrightness) {
            runtime.setDisplayBrightness(static_cast<uint8_t>(brightness));
        }
    }
    if (server.hasArg("voiceVolume")) {
        int volume = server.arg("voiceVolume").toInt();
        volume = std::max(0, std::min(volume, 100));
        s.voiceVolume = static_cast<uint8_t>(volume);
        if (runtime.setAudioVolume) {
            runtime.setAudioVolume(static_cast<uint8_t>(volume));
        }
    }

    // Persist all color/visibility changes
    if (runtime.saveSettings) {
        runtime.saveSettings();
    }

    // Trigger immediate display preview to show new colors (skip if requested)
    if (!server.hasArg("skipPreview") ||
        (server.arg("skipPreview") != "true" && server.arg("skipPreview") != "1")) {
        if (runtime.showDisplayDemo) {
            runtime.showDisplayDemo();
        }
        if (runtime.requestColorPreviewHoldMs) {
            runtime.requestColorPreviewHoldMs(5500);  // Hold ~5.5s and cycle bands during preview.
        }
    }

    server.send(200, "application/json", "{\"success\":true}");
}

void handleApiReset(WebServer& server,
                    const Runtime& runtime,
                    const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;

    if (!runtime.getMutableSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    V1Settings& s = runtime.getMutableSettings();

    // Reset to default colors: Bogey/Freq=Red, Front/Side/Rear=Red, L/K=Blue, Ka=Red, X=Green, WiFi=Cyan
    s.colorBogey = 0xF800;
    s.colorFrequency = 0xF800;
    s.colorArrowFront = 0xF800;
    s.colorArrowSide = 0xF800;
    s.colorArrowRear = 0xF800;
    s.colorBandL = 0x001F;
    s.colorBandKa = 0xF800;
    s.colorBandK = 0x001F;
    s.colorBandX = 0x07E0;
    s.colorBandPhoto = 0x780F;
    s.colorWiFiIcon = 0x07FF;
    s.colorWiFiConnected = 0x07E0;
    s.colorBleConnected = 0x07E0;
    s.colorBleDisconnected = 0x001F;
    s.colorBar1 = 0x07E0;
    s.colorBar2 = 0x07E0;
    s.colorBar3 = 0xFFE0;
    s.colorBar4 = 0xFFE0;
    s.colorBar5 = 0xF800;
    s.colorBar6 = 0xF800;
    s.colorMuted = 0x3186;
    s.colorPersisted = 0x18C3;
    s.colorVolumeMain = 0x001F;
    s.colorVolumeMute = 0xFFE0;
    s.colorRssiV1 = 0x07E0;
    s.colorRssiProxy = 0x001F;
    s.colorLockout = 0x07E0;
    s.colorGps = 0x07FF;
    s.freqUseBandColor = false;

    if (runtime.saveSettings) {
        runtime.saveSettings();
    }

    // Trigger immediate display preview to show reset colors.
    if (runtime.showDisplayDemo) {
        runtime.showDisplayDemo();
    }
    if (runtime.requestColorPreviewHoldMs) {
        runtime.requestColorPreviewHoldMs(5500);
    }

    server.send(200, "application/json", "{\"success\":true}");
}

static void handlePreviewImpl(WebServer& server, const Runtime& runtime) {
    const bool previewRunning =
        runtime.isColorPreviewRunning && runtime.isColorPreviewRunning();

    if (previewRunning) {
        Serial.println("[HTTP] POST /api/displaycolors/preview - toggling off");
        if (runtime.cancelColorPreview) {
            runtime.cancelColorPreview();
        }
        // main.cpp loop handles display restore based on V1 connection state
        server.send(200, "application/json", "{\"success\":true,\"active\":false}");
        return;
    }

    Serial.println("[HTTP] POST /api/displaycolors/preview - starting");
    if (runtime.showDisplayDemo) {
        runtime.showDisplayDemo();
    }
    if (runtime.requestColorPreviewHoldMs) {
        runtime.requestColorPreviewHoldMs(5500);
    }
    server.send(200, "application/json", "{\"success\":true,\"active\":true}");
}

void handleApiPreview(WebServer& server,
                      const Runtime& runtime,
                      const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handlePreviewImpl(server, runtime);
}

static void handleClearImpl(WebServer& server, const Runtime& runtime) {
    Serial.println("[HTTP] POST /api/displaycolors/clear - cancelling preview");
    if (runtime.cancelColorPreview) {
        runtime.cancelColorPreview();
    }
    // main.cpp loop handles display restore based on V1 connection state
    server.send(200, "application/json", "{\"success\":true,\"active\":false}");
}

void handleApiClear(WebServer& server,
                    const Runtime& runtime,
                    const std::function<bool()>& checkRateLimit) {
    if (checkRateLimit && !checkRateLimit()) return;
    handleClearImpl(server, runtime);
}

void handleApiGet(WebServer& server, const Runtime& runtime) {
    if (!runtime.getSettings) {
        server.send(500, "application/json", "{\"error\":\"Settings unavailable\"}");
        return;
    }

    const V1Settings& s = runtime.getSettings();

    JsonDocument doc;
    doc["bogey"] = s.colorBogey;
    doc["freq"] = s.colorFrequency;
    doc["arrowFront"] = s.colorArrowFront;
    doc["arrowSide"] = s.colorArrowSide;
    doc["arrowRear"] = s.colorArrowRear;
    doc["bandL"] = s.colorBandL;
    doc["bandKa"] = s.colorBandKa;
    doc["bandK"] = s.colorBandK;
    doc["bandX"] = s.colorBandX;
    doc["bandPhoto"] = s.colorBandPhoto;
    doc["wifiIcon"] = s.colorWiFiIcon;
    doc["wifiConnected"] = s.colorWiFiConnected;
    doc["bleConnected"] = s.colorBleConnected;
    doc["bleDisconnected"] = s.colorBleDisconnected;
    doc["bar1"] = s.colorBar1;
    doc["bar2"] = s.colorBar2;
    doc["bar3"] = s.colorBar3;
    doc["bar4"] = s.colorBar4;
    doc["bar5"] = s.colorBar5;
    doc["bar6"] = s.colorBar6;
    doc["muted"] = s.colorMuted;
    doc["persisted"] = s.colorPersisted;
    doc["volumeMain"] = s.colorVolumeMain;
    doc["volumeMute"] = s.colorVolumeMute;
    doc["rssiV1"] = s.colorRssiV1;
    doc["rssiProxy"] = s.colorRssiProxy;
    doc["lockout"] = s.colorLockout;
    doc["gps"] = s.colorGps;
    doc["freqUseBandColor"] = s.freqUseBandColor;
    doc["hideWifiIcon"] = s.hideWifiIcon;
    doc["hideProfileIndicator"] = s.hideProfileIndicator;
    doc["hideBatteryIcon"] = s.hideBatteryIcon;
    doc["showBatteryPercent"] = s.showBatteryPercent;
    doc["hideBleIcon"] = s.hideBleIcon;
    doc["hideVolumeIndicator"] = s.hideVolumeIndicator;
    doc["hideRssiIndicator"] = s.hideRssiIndicator;
    doc["enableWifiAtBoot"] = s.enableWifiAtBoot;
    doc["enableSignalTraceLogging"] = s.enableSignalTraceLogging;
    doc["voiceAlertMode"] = static_cast<int>(s.voiceAlertMode);
    doc["voiceDirectionEnabled"] = s.voiceDirectionEnabled;
    doc["announceBogeyCount"] = s.announceBogeyCount;
    doc["muteVoiceIfVolZero"] = s.muteVoiceIfVolZero;
    doc["brightness"] = s.brightness;
    doc["voiceVolume"] = s.voiceVolume;
    doc["announceSecondaryAlerts"] = s.announceSecondaryAlerts;
    doc["secondaryLaser"] = s.secondaryLaser;
    doc["secondaryKa"] = s.secondaryKa;
    doc["secondaryK"] = s.secondaryK;
    doc["secondaryX"] = s.secondaryX;
    doc["alertVolumeFadeEnabled"] = s.alertVolumeFadeEnabled;
    doc["alertVolumeFadeDelaySec"] = s.alertVolumeFadeDelaySec;
    doc["alertVolumeFadeVolume"] = s.alertVolumeFadeVolume;
    doc["gpsEnabled"] = s.gpsEnabled;
    doc["gpsLockoutMode"] = static_cast<int>(s.gpsLockoutMode);
    doc["gpsLockoutModeName"] = lockoutRuntimeModeName(s.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = s.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = s.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = s.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = s.gpsLockoutMaxEventBusDrops;
    doc["gpsLockoutKaLearningEnabled"] = s.gpsLockoutKaLearningEnabled;

    WifiApiResponse::sendJsonDocument(server, 200, doc);
}

}  // namespace WifiDisplayColorsApiService
