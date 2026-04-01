#include "gps_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

#ifndef UNIT_TEST
#include "gps_runtime_module.h"
#include "gps_observation_log.h"
#include "../speed/speed_source_selector.h"
#include "../../settings.h"
#endif
#include "../../settings_runtime_sync.h"
#include "../../../include/clamp_utils.h"

namespace GpsApiService {

namespace {

void sendConfig(WebServer& server, SettingsManager& settingsManager) {
    const V1Settings& settings = settingsManager.get();

    JsonDocument response;
    response["success"] = true;
    response["enabled"] = settings.gpsEnabled;

    String json;
    serializeJson(response, json);
    server.send(200, "application/json", json);
}

}  // namespace

void handleConfig(WebServer& server,
                  SettingsManager& settingsManager,
                  GpsRuntimeModule& gpsRuntimeModule,
                  SpeedSourceSelector& speedSourceSelector,
                  GpsObservationLog& gpsObservationLog) {
    const V1Settings& currentSettings = settingsManager.get();

    bool hasEnabled = false;
    bool enabled = currentSettings.gpsEnabled;
    bool hasScaffoldSample = false;
    float scaffoldSpeedMph = 0.0f;
    bool scaffoldHasFix = true;
    uint8_t scaffoldSatellites = 0;
    float scaffoldHdop = NAN;
    float scaffoldLatitudeDeg = NAN;
    float scaffoldLongitudeDeg = NAN;

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument body;
        const String plainBody = server.arg("plain");
        DeserializationError error = deserializeJson(body, plainBody);
        if (error) {
            server.send(400, "application/json", "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        if (body["enabled"].is<bool>()) {
            enabled = body["enabled"].as<bool>();
            hasEnabled = true;
        }
        if (body["speedMph"].is<float>() || body["speedMph"].is<double>() || body["speedMph"].is<int>()) {
            scaffoldSpeedMph = body["speedMph"].as<float>();
            hasScaffoldSample = true;
        }
        if (body["hasFix"].is<bool>()) {
            scaffoldHasFix = body["hasFix"].as<bool>();
        }
        if (body["satellites"].is<int>()) {
            int sats = body["satellites"].as<int>();
            scaffoldSatellites = static_cast<uint8_t>(std::max(0, std::min(sats, 99)));
        }
        if (body["hdop"].is<float>() || body["hdop"].is<double>() || body["hdop"].is<int>()) {
            scaffoldHdop = body["hdop"].as<float>();
        }
        if (body["latitude"].is<float>() || body["latitude"].is<double>() || body["latitude"].is<int>()) {
            scaffoldLatitudeDeg = body["latitude"].as<float>();
        }
        if (body["longitude"].is<float>() || body["longitude"].is<double>() || body["longitude"].is<int>()) {
            scaffoldLongitudeDeg = body["longitude"].as<float>();
        }
    }

    if (!hasEnabled && server.hasArg("enabled")) {
        String value = server.arg("enabled");
        value.toLowerCase();
        if (value == "1" || value == "true" || value == "on") {
            enabled = true;
            hasEnabled = true;
        } else if (value == "0" || value == "false" || value == "off") {
            enabled = false;
            hasEnabled = true;
        }
    }

    if (!hasEnabled) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing enabled\"}");
        return;
    }

    if (hasScaffoldSample &&
        (!std::isfinite(scaffoldSpeedMph) ||
         scaffoldSpeedMph < 0.0f ||
         scaffoldSpeedMph > SpeedSourceSelector::MAX_VALID_SPEED_MPH)) {
        server.send(400, "application/json", "{\"success\":false,\"message\":\"speedMph out of range\"}");
        return;
    }
    if (std::isfinite(scaffoldHdop) && scaffoldHdop < 0.0f) {
        scaffoldHdop = 0.0f;
    }
    const bool hasScaffoldLatitude = std::isfinite(scaffoldLatitudeDeg);
    const bool hasScaffoldLongitude = std::isfinite(scaffoldLongitudeDeg);
    if (hasScaffoldLatitude != hasScaffoldLongitude) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"latitude and longitude must be provided together\"}");
        return;
    }
    if (hasScaffoldLatitude &&
        (scaffoldLatitudeDeg < -90.0f || scaffoldLatitudeDeg > 90.0f ||
         scaffoldLongitudeDeg < -180.0f || scaffoldLongitudeDeg > 180.0f)) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"latitude/longitude out of range\"}");
        return;
    }

    GpsSettingsUpdate settingsUpdate;
    settingsUpdate.hasEnabled = hasEnabled;
    settingsUpdate.enabled = enabled;

    settingsManager.applyGpsSettingsUpdate(settingsUpdate, SettingsPersistMode::Deferred);
    if (hasEnabled) {
        SettingsRuntimeSync::syncGpsVehicleRuntimeSettings(settingsManager.get(),
                                                           gpsRuntimeModule,
                                                           speedSourceSelector);
    }

    if (enabled && hasScaffoldSample) {
        gpsRuntimeModule.setScaffoldSample(scaffoldSpeedMph,
                                           scaffoldHasFix,
                                           scaffoldSatellites,
                                           scaffoldHdop,
                                           millis(),
                                           scaffoldLatitudeDeg,
                                           scaffoldLongitudeDeg);
    }

    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot();
    const V1Settings& settings = settingsManager.get();

    JsonDocument response;
    response["success"] = true;
    response["enabled"] = settings.gpsEnabled;
    response["runtimeEnabled"] = gpsStatus.enabled;
    response["sampleValid"] = gpsStatus.sampleValid;
    response["hasFix"] = gpsStatus.hasFix;
    response["injectedSamples"] = gpsStatus.injectedSamples;
    response["speedSource"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    response["locationValid"] = gpsStatus.locationValid;
    if (gpsStatus.locationValid) {
        response["latitude"] = gpsStatus.latitudeDeg;
        response["longitude"] = gpsStatus.longitudeDeg;
    } else {
        response["latitude"] = nullptr;
        response["longitude"] = nullptr;
    }
    response["courseValid"] = gpsStatus.courseValid;
    if (gpsStatus.courseValid) {
        response["courseDeg"] = gpsStatus.courseDeg;
        response["courseSampleTsMs"] = gpsStatus.courseSampleTsMs;
    } else {
        response["courseDeg"] = nullptr;
        response["courseSampleTsMs"] = nullptr;
    }
    if (gpsStatus.courseAgeMs == UINT32_MAX) {
        response["courseAgeMs"] = nullptr;
    } else {
        response["courseAgeMs"] = gpsStatus.courseAgeMs;
    }
    const GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    response["observationSize"] = static_cast<uint32_t>(gpsLogStats.size);
    response["observationDrops"] = gpsLogStats.drops;

    String json;
    serializeJson(response, json);
    server.send(200, "application/json", json);
}

void handleApiConfigGet(WebServer& server,
                        SettingsManager& settingsManager,
                        void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendConfig(server, settingsManager);
}

void handleApiConfig(WebServer& server,
                     SettingsManager& settingsManager,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     GpsObservationLog& gpsObservationLog,
                     bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    handleConfig(server,
                 settingsManager,
                 gpsRuntimeModule,
                 speedSourceSelector,
                 gpsObservationLog);
}

}  // namespace GpsApiService
