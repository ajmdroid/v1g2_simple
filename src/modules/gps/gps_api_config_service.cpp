#include "gps_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

#ifndef UNIT_TEST
#include "gps_runtime_module.h"
#include "gps_lockout_safety.h"
#include "gps_observation_log.h"
#include "../lockout/lockout_learner.h"
#include "../lockout/lockout_band_policy.h"
#include "../speed/speed_source_selector.h"
#include "../system/system_event_bus.h"
#include "../../settings.h"
#include "../../perf_metrics.h"
#endif
#include "../../../include/clamp_utils.h"

namespace GpsApiService {

namespace {

void appendLockoutConfig(JsonDocument& doc, const V1Settings& settings) {
    JsonObject lockoutObj = doc["lockout"].to<JsonObject>();
    lockoutObj["mode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    lockoutObj["modeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    lockoutObj["coreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    lockoutObj["maxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    lockoutObj["maxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    lockoutObj["maxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    lockoutObj["learnerPromotionHits"] = settings.gpsLockoutLearnerPromotionHits;
    lockoutObj["learnerRadiusE5"] = settings.gpsLockoutLearnerRadiusE5;
    lockoutObj["learnerFreqToleranceMHz"] = settings.gpsLockoutLearnerFreqToleranceMHz;
    lockoutObj["learnerLearnIntervalHours"] = settings.gpsLockoutLearnerLearnIntervalHours;
    lockoutObj["learnerUnlearnIntervalHours"] = settings.gpsLockoutLearnerUnlearnIntervalHours;
    lockoutObj["learnerUnlearnCount"] = settings.gpsLockoutLearnerUnlearnCount;
    lockoutObj["manualDemotionMissCount"] = settings.gpsLockoutManualDemotionMissCount;
    lockoutObj["kaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    lockoutObj["kLearningEnabled"] = settings.gpsLockoutKLearningEnabled;
    lockoutObj["xLearningEnabled"] = settings.gpsLockoutXLearningEnabled;
    lockoutObj["preQuiet"] = settings.gpsLockoutPreQuiet;
    lockoutObj["preQuietBufferE5"] = settings.gpsLockoutPreQuietBufferE5;
    lockoutObj["maxHdopX10"] = settings.gpsLockoutMaxHdopX10;
    lockoutObj["minLearnerSpeedMph"] = settings.gpsLockoutMinLearnerSpeedMph;
    lockoutObj["minSatellites"] = LOCKOUT_GPS_MIN_SATELLITES;

    doc["gpsEnabled"] = settings.gpsEnabled;
    doc["lockoutMode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    doc["lockoutModeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    doc["lockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    doc["lockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    doc["lockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    doc["lockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    doc["lockoutLearnerPromotionHits"] = settings.gpsLockoutLearnerPromotionHits;
    doc["lockoutLearnerRadiusE5"] = settings.gpsLockoutLearnerRadiusE5;
    doc["lockoutLearnerFreqToleranceMHz"] = settings.gpsLockoutLearnerFreqToleranceMHz;
    doc["lockoutLearnerLearnIntervalHours"] = settings.gpsLockoutLearnerLearnIntervalHours;
    doc["lockoutLearnerUnlearnIntervalHours"] = settings.gpsLockoutLearnerUnlearnIntervalHours;
    doc["lockoutLearnerUnlearnCount"] = settings.gpsLockoutLearnerUnlearnCount;
    doc["lockoutManualDemotionMissCount"] = settings.gpsLockoutManualDemotionMissCount;
    doc["lockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    doc["lockoutKLearningEnabled"] = settings.gpsLockoutKLearningEnabled;
    doc["lockoutXLearningEnabled"] = settings.gpsLockoutXLearningEnabled;
    doc["lockoutPreQuiet"] = settings.gpsLockoutPreQuiet;
    doc["lockoutPreQuietBufferE5"] = settings.gpsLockoutPreQuietBufferE5;
    doc["lockoutMaxHdopX10"] = settings.gpsLockoutMaxHdopX10;
    doc["lockoutMinLearnerSpeedMph"] = settings.gpsLockoutMinLearnerSpeedMph;
    doc["lockoutMinSatellites"] = LOCKOUT_GPS_MIN_SATELLITES;
    doc["gpsLockoutMode"] = static_cast<int>(settings.gpsLockoutMode);
    doc["gpsLockoutModeName"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    doc["gpsLockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    doc["gpsLockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    doc["gpsLockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    doc["gpsLockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    doc["gpsLockoutLearnerPromotionHits"] = settings.gpsLockoutLearnerPromotionHits;
    doc["gpsLockoutLearnerRadiusE5"] = settings.gpsLockoutLearnerRadiusE5;
    doc["gpsLockoutLearnerFreqToleranceMHz"] = settings.gpsLockoutLearnerFreqToleranceMHz;
    doc["gpsLockoutLearnerLearnIntervalHours"] = settings.gpsLockoutLearnerLearnIntervalHours;
    doc["gpsLockoutLearnerUnlearnIntervalHours"] = settings.gpsLockoutLearnerUnlearnIntervalHours;
    doc["gpsLockoutLearnerUnlearnCount"] = settings.gpsLockoutLearnerUnlearnCount;
    doc["gpsLockoutManualDemotionMissCount"] = settings.gpsLockoutManualDemotionMissCount;
    doc["gpsLockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    doc["gpsLockoutKLearningEnabled"] = settings.gpsLockoutKLearningEnabled;
    doc["gpsLockoutXLearningEnabled"] = settings.gpsLockoutXLearningEnabled;
    doc["gpsLockoutPreQuiet"] = settings.gpsLockoutPreQuiet;
    doc["gpsLockoutPreQuietBufferE5"] = settings.gpsLockoutPreQuietBufferE5;
    doc["gpsLockoutMaxHdopX10"] = settings.gpsLockoutMaxHdopX10;
    doc["gpsLockoutMinLearnerSpeedMph"] = settings.gpsLockoutMinLearnerSpeedMph;
}

void sendConfig(WebServer& server, SettingsManager& settingsManager) {
    const V1Settings& settings = settingsManager.get();

    JsonDocument response;
    response["success"] = true;
    response["enabled"] = settings.gpsEnabled;
    appendLockoutConfig(response, settings);

    String json;
    serializeJson(response, json);
    server.send(200, "application/json", json);
}

}  // namespace

void handleConfig(WebServer& server,
                  SettingsManager& settingsManager,
                  GpsRuntimeModule& gpsRuntimeModule,
                  SpeedSourceSelector& speedSourceSelector,
                  LockoutLearner& lockoutLearner,
                  GpsObservationLog& gpsObservationLog,
                  PerfCounters& perfCounters,
                  SystemEventBus& systemEventBus) {
    V1Settings& mutableSettings = settingsManager.mutableSettings();
    const V1Settings& currentSettings = mutableSettings;

    bool hasEnabled = false;
    bool enabled = currentSettings.gpsEnabled;
    bool hasScaffoldSample = false;
    float scaffoldSpeedMph = 0.0f;
    bool scaffoldHasFix = true;
    uint8_t scaffoldSatellites = 0;
    float scaffoldHdop = NAN;
    float scaffoldLatitudeDeg = NAN;
    float scaffoldLongitudeDeg = NAN;
    bool hasLockoutMode = false;
    LockoutRuntimeMode lockoutMode = currentSettings.gpsLockoutMode;
    bool hasCoreGuardEnabled = false;
    bool coreGuardEnabled = currentSettings.gpsLockoutCoreGuardEnabled;
    bool hasMaxQueueDrops = false;
    uint16_t maxQueueDrops = currentSettings.gpsLockoutMaxQueueDrops;
    bool hasMaxPerfDrops = false;
    uint16_t maxPerfDrops = currentSettings.gpsLockoutMaxPerfDrops;
    bool hasMaxEventBusDrops = false;
    uint16_t maxEventBusDrops = currentSettings.gpsLockoutMaxEventBusDrops;
    bool hasLearnerPromotionHits = false;
    uint8_t learnerPromotionHits = currentSettings.gpsLockoutLearnerPromotionHits;
    bool hasLearnerRadiusE5 = false;
    uint16_t learnerRadiusE5 = currentSettings.gpsLockoutLearnerRadiusE5;
    bool hasLearnerFreqToleranceMHz = false;
    uint16_t learnerFreqToleranceMHz = currentSettings.gpsLockoutLearnerFreqToleranceMHz;
    bool hasLearnerLearnIntervalHours = false;
    uint8_t learnerLearnIntervalHours = currentSettings.gpsLockoutLearnerLearnIntervalHours;
    bool hasLearnerUnlearnIntervalHours = false;
    uint8_t learnerUnlearnIntervalHours = currentSettings.gpsLockoutLearnerUnlearnIntervalHours;
    bool hasLearnerUnlearnCount = false;
    uint8_t learnerUnlearnCount = currentSettings.gpsLockoutLearnerUnlearnCount;
    bool hasManualDemotionMissCount = false;
    uint8_t manualDemotionMissCount = currentSettings.gpsLockoutManualDemotionMissCount;
    bool hasKaLearningEnabled = false;
    bool kaLearningEnabled = currentSettings.gpsLockoutKaLearningEnabled;
    bool hasKLearningEnabled = false;
    bool kLearningEnabled = currentSettings.gpsLockoutKLearningEnabled;
    bool hasXLearningEnabled = false;
    bool xLearningEnabled = currentSettings.gpsLockoutXLearningEnabled;
    bool hasPreQuiet = false;
    bool preQuiet = currentSettings.gpsLockoutPreQuiet;
    bool hasPreQuietBufferE5 = false;
    uint16_t preQuietBufferE5 = currentSettings.gpsLockoutPreQuietBufferE5;
    bool hasMaxHdopX10 = false;
    uint16_t maxHdopX10 = currentSettings.gpsLockoutMaxHdopX10;
    bool hasMinLearnerSpeedMph = false;
    uint8_t minLearnerSpeedMph = currentSettings.gpsLockoutMinLearnerSpeedMph;

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
        if (body["lockoutMode"].is<int>()) {
            lockoutMode = clampLockoutRuntimeModeValue(body["lockoutMode"].as<int>());
            hasLockoutMode = true;
        } else if (body["lockoutMode"].is<const char*>()) {
            lockoutMode = gpsLockoutParseRuntimeModeArg(String(body["lockoutMode"].as<const char*>()), lockoutMode);
            hasLockoutMode = true;
        } else if (body["gpsLockoutMode"].is<int>()) {
            lockoutMode = clampLockoutRuntimeModeValue(body["gpsLockoutMode"].as<int>());
            hasLockoutMode = true;
        } else if (body["gpsLockoutMode"].is<const char*>()) {
            lockoutMode = gpsLockoutParseRuntimeModeArg(String(body["gpsLockoutMode"].as<const char*>()), lockoutMode);
            hasLockoutMode = true;
        }
        if (body["lockoutCoreGuardEnabled"].is<bool>()) {
            coreGuardEnabled = body["lockoutCoreGuardEnabled"].as<bool>();
            hasCoreGuardEnabled = true;
        } else if (body["gpsLockoutCoreGuardEnabled"].is<bool>()) {
            coreGuardEnabled = body["gpsLockoutCoreGuardEnabled"].as<bool>();
            hasCoreGuardEnabled = true;
        }
        if (body["lockoutMaxQueueDrops"].is<int>()) {
            maxQueueDrops = clamp_utils::clampU16Value(body["lockoutMaxQueueDrops"].as<int>(), 0, 65535);
            hasMaxQueueDrops = true;
        } else if (body["gpsLockoutMaxQueueDrops"].is<int>()) {
            maxQueueDrops = clamp_utils::clampU16Value(body["gpsLockoutMaxQueueDrops"].as<int>(), 0, 65535);
            hasMaxQueueDrops = true;
        }
        if (body["lockoutMaxPerfDrops"].is<int>()) {
            maxPerfDrops = clamp_utils::clampU16Value(body["lockoutMaxPerfDrops"].as<int>(), 0, 65535);
            hasMaxPerfDrops = true;
        } else if (body["gpsLockoutMaxPerfDrops"].is<int>()) {
            maxPerfDrops = clamp_utils::clampU16Value(body["gpsLockoutMaxPerfDrops"].as<int>(), 0, 65535);
            hasMaxPerfDrops = true;
        }
        if (body["lockoutMaxEventBusDrops"].is<int>()) {
            maxEventBusDrops = clamp_utils::clampU16Value(body["lockoutMaxEventBusDrops"].as<int>(), 0, 65535);
            hasMaxEventBusDrops = true;
        } else if (body["gpsLockoutMaxEventBusDrops"].is<int>()) {
            maxEventBusDrops = clamp_utils::clampU16Value(body["gpsLockoutMaxEventBusDrops"].as<int>(), 0, 65535);
            hasMaxEventBusDrops = true;
        }
        if (body["lockoutLearnerPromotionHits"].is<int>()) {
            learnerPromotionHits = clampLockoutLearnerHitsValue(body["lockoutLearnerPromotionHits"].as<int>());
            hasLearnerPromotionHits = true;
        } else if (body["gpsLockoutLearnerPromotionHits"].is<int>()) {
            learnerPromotionHits = clampLockoutLearnerHitsValue(body["gpsLockoutLearnerPromotionHits"].as<int>());
            hasLearnerPromotionHits = true;
        }
        if (body["lockoutLearnerRadiusE5"].is<int>()) {
            learnerRadiusE5 = clampLockoutLearnerRadiusE5Value(body["lockoutLearnerRadiusE5"].as<int>());
            hasLearnerRadiusE5 = true;
        } else if (body["gpsLockoutLearnerRadiusE5"].is<int>()) {
            learnerRadiusE5 = clampLockoutLearnerRadiusE5Value(body["gpsLockoutLearnerRadiusE5"].as<int>());
            hasLearnerRadiusE5 = true;
        }
        if (body["lockoutLearnerFreqToleranceMHz"].is<int>()) {
            learnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
                body["lockoutLearnerFreqToleranceMHz"].as<int>());
            hasLearnerFreqToleranceMHz = true;
        } else if (body["gpsLockoutLearnerFreqToleranceMHz"].is<int>()) {
            learnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
                body["gpsLockoutLearnerFreqToleranceMHz"].as<int>());
            hasLearnerFreqToleranceMHz = true;
        }
        if (body["lockoutLearnerLearnIntervalHours"].is<int>()) {
            learnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
                body["lockoutLearnerLearnIntervalHours"].as<int>());
            hasLearnerLearnIntervalHours = true;
        } else if (body["gpsLockoutLearnerLearnIntervalHours"].is<int>()) {
            learnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
                body["gpsLockoutLearnerLearnIntervalHours"].as<int>());
            hasLearnerLearnIntervalHours = true;
        }
        if (body["lockoutLearnerUnlearnIntervalHours"].is<int>()) {
            learnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
                body["lockoutLearnerUnlearnIntervalHours"].as<int>());
            hasLearnerUnlearnIntervalHours = true;
        } else if (body["gpsLockoutLearnerUnlearnIntervalHours"].is<int>()) {
            learnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
                body["gpsLockoutLearnerUnlearnIntervalHours"].as<int>());
            hasLearnerUnlearnIntervalHours = true;
        }
        if (body["lockoutLearnerUnlearnCount"].is<int>()) {
            learnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
                body["lockoutLearnerUnlearnCount"].as<int>());
            hasLearnerUnlearnCount = true;
        } else if (body["gpsLockoutLearnerUnlearnCount"].is<int>()) {
            learnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
                body["gpsLockoutLearnerUnlearnCount"].as<int>());
            hasLearnerUnlearnCount = true;
        }
        if (body["lockoutManualDemotionMissCount"].is<int>()) {
            manualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
                body["lockoutManualDemotionMissCount"].as<int>());
            hasManualDemotionMissCount = true;
        } else if (body["gpsLockoutManualDemotionMissCount"].is<int>()) {
            manualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
                body["gpsLockoutManualDemotionMissCount"].as<int>());
            hasManualDemotionMissCount = true;
        } else if (body["lockoutLearnerManualDemotionMissCount"].is<int>()) {
            manualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
                body["lockoutLearnerManualDemotionMissCount"].as<int>());
            hasManualDemotionMissCount = true;
        }
        if (body["lockoutKaLearningEnabled"].is<bool>()) {
            kaLearningEnabled = body["lockoutKaLearningEnabled"].as<bool>();
            hasKaLearningEnabled = true;
        } else if (body["gpsLockoutKaLearningEnabled"].is<bool>()) {
            kaLearningEnabled = body["gpsLockoutKaLearningEnabled"].as<bool>();
            hasKaLearningEnabled = true;
        }
        if (body["lockoutKLearningEnabled"].is<bool>()) {
            kLearningEnabled = body["lockoutKLearningEnabled"].as<bool>();
            hasKLearningEnabled = true;
        } else if (body["gpsLockoutKLearningEnabled"].is<bool>()) {
            kLearningEnabled = body["gpsLockoutKLearningEnabled"].as<bool>();
            hasKLearningEnabled = true;
        }
        if (body["lockoutXLearningEnabled"].is<bool>()) {
            xLearningEnabled = body["lockoutXLearningEnabled"].as<bool>();
            hasXLearningEnabled = true;
        } else if (body["gpsLockoutXLearningEnabled"].is<bool>()) {
            xLearningEnabled = body["gpsLockoutXLearningEnabled"].as<bool>();
            hasXLearningEnabled = true;
        }
        if (body["lockoutPreQuiet"].is<bool>()) {
            preQuiet = body["lockoutPreQuiet"].as<bool>();
            hasPreQuiet = true;
        } else if (body["gpsLockoutPreQuiet"].is<bool>()) {
            preQuiet = body["gpsLockoutPreQuiet"].as<bool>();
            hasPreQuiet = true;
        }
        if (body["lockoutPreQuietBufferE5"].is<int>()) {
            preQuietBufferE5 = clampLockoutPreQuietBufferE5Value(body["lockoutPreQuietBufferE5"].as<int>());
            hasPreQuietBufferE5 = true;
        } else if (body["gpsLockoutPreQuietBufferE5"].is<int>()) {
            preQuietBufferE5 = clampLockoutPreQuietBufferE5Value(body["gpsLockoutPreQuietBufferE5"].as<int>());
            hasPreQuietBufferE5 = true;
        }
        if (body["lockoutMaxHdopX10"].is<int>()) {
            maxHdopX10 = clampLockoutGpsMaxHdopX10Value(body["lockoutMaxHdopX10"].as<int>());
            hasMaxHdopX10 = true;
        } else if (body["gpsLockoutMaxHdopX10"].is<int>()) {
            maxHdopX10 = clampLockoutGpsMaxHdopX10Value(body["gpsLockoutMaxHdopX10"].as<int>());
            hasMaxHdopX10 = true;
        }
        if (body["lockoutMinLearnerSpeedMph"].is<int>()) {
            minLearnerSpeedMph = clampLockoutGpsMinLearnerSpeedMphValue(body["lockoutMinLearnerSpeedMph"].as<int>());
            hasMinLearnerSpeedMph = true;
        } else if (body["gpsLockoutMinLearnerSpeedMph"].is<int>()) {
            minLearnerSpeedMph = clampLockoutGpsMinLearnerSpeedMphValue(body["gpsLockoutMinLearnerSpeedMph"].as<int>());
            hasMinLearnerSpeedMph = true;
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
    if (!hasLockoutMode && server.hasArg("lockoutMode")) {
        lockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("lockoutMode"), lockoutMode);
        hasLockoutMode = true;
    }
    if (!hasLockoutMode && server.hasArg("gpsLockoutMode")) {
        lockoutMode = gpsLockoutParseRuntimeModeArg(server.arg("gpsLockoutMode"), lockoutMode);
        hasLockoutMode = true;
    }
    if (!hasCoreGuardEnabled && server.hasArg("lockoutCoreGuardEnabled")) {
        String value = server.arg("lockoutCoreGuardEnabled");
        value.toLowerCase();
        coreGuardEnabled = (value == "1" || value == "true" || value == "on");
        hasCoreGuardEnabled = true;
    }
    if (!hasCoreGuardEnabled && server.hasArg("gpsLockoutCoreGuardEnabled")) {
        String value = server.arg("gpsLockoutCoreGuardEnabled");
        value.toLowerCase();
        coreGuardEnabled = (value == "1" || value == "true" || value == "on");
        hasCoreGuardEnabled = true;
    }
    if (!hasMaxQueueDrops && server.hasArg("lockoutMaxQueueDrops")) {
        maxQueueDrops = clamp_utils::clampU16Value(server.arg("lockoutMaxQueueDrops").toInt(), 0, 65535);
        hasMaxQueueDrops = true;
    }
    if (!hasMaxQueueDrops && server.hasArg("gpsLockoutMaxQueueDrops")) {
        maxQueueDrops = clamp_utils::clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
        hasMaxQueueDrops = true;
    }
    if (!hasMaxPerfDrops && server.hasArg("lockoutMaxPerfDrops")) {
        maxPerfDrops = clamp_utils::clampU16Value(server.arg("lockoutMaxPerfDrops").toInt(), 0, 65535);
        hasMaxPerfDrops = true;
    }
    if (!hasMaxPerfDrops && server.hasArg("gpsLockoutMaxPerfDrops")) {
        maxPerfDrops = clamp_utils::clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
        hasMaxPerfDrops = true;
    }
    if (!hasMaxEventBusDrops && server.hasArg("lockoutMaxEventBusDrops")) {
        maxEventBusDrops = clamp_utils::clampU16Value(server.arg("lockoutMaxEventBusDrops").toInt(), 0, 65535);
        hasMaxEventBusDrops = true;
    }
    if (!hasMaxEventBusDrops && server.hasArg("gpsLockoutMaxEventBusDrops")) {
        maxEventBusDrops = clamp_utils::clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
        hasMaxEventBusDrops = true;
    }
    if (!hasLearnerPromotionHits && server.hasArg("lockoutLearnerPromotionHits")) {
        learnerPromotionHits = clampLockoutLearnerHitsValue(server.arg("lockoutLearnerPromotionHits").toInt());
        hasLearnerPromotionHits = true;
    }
    if (!hasLearnerPromotionHits && server.hasArg("gpsLockoutLearnerPromotionHits")) {
        learnerPromotionHits = clampLockoutLearnerHitsValue(server.arg("gpsLockoutLearnerPromotionHits").toInt());
        hasLearnerPromotionHits = true;
    }
    if (!hasLearnerRadiusE5 && server.hasArg("lockoutLearnerRadiusE5")) {
        learnerRadiusE5 = clampLockoutLearnerRadiusE5Value(server.arg("lockoutLearnerRadiusE5").toInt());
        hasLearnerRadiusE5 = true;
    }
    if (!hasLearnerRadiusE5 && server.hasArg("gpsLockoutLearnerRadiusE5")) {
        learnerRadiusE5 = clampLockoutLearnerRadiusE5Value(server.arg("gpsLockoutLearnerRadiusE5").toInt());
        hasLearnerRadiusE5 = true;
    }
    if (!hasLearnerFreqToleranceMHz && server.hasArg("lockoutLearnerFreqToleranceMHz")) {
        learnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
            server.arg("lockoutLearnerFreqToleranceMHz").toInt());
        hasLearnerFreqToleranceMHz = true;
    }
    if (!hasLearnerFreqToleranceMHz && server.hasArg("gpsLockoutLearnerFreqToleranceMHz")) {
        learnerFreqToleranceMHz = clampLockoutLearnerFreqTolValue(
            server.arg("gpsLockoutLearnerFreqToleranceMHz").toInt());
        hasLearnerFreqToleranceMHz = true;
    }
    if (!hasLearnerLearnIntervalHours && server.hasArg("lockoutLearnerLearnIntervalHours")) {
        learnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            server.arg("lockoutLearnerLearnIntervalHours").toInt());
        hasLearnerLearnIntervalHours = true;
    }
    if (!hasLearnerLearnIntervalHours && server.hasArg("gpsLockoutLearnerLearnIntervalHours")) {
        learnerLearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            server.arg("gpsLockoutLearnerLearnIntervalHours").toInt());
        hasLearnerLearnIntervalHours = true;
    }
    if (!hasLearnerUnlearnIntervalHours && server.hasArg("lockoutLearnerUnlearnIntervalHours")) {
        learnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            server.arg("lockoutLearnerUnlearnIntervalHours").toInt());
        hasLearnerUnlearnIntervalHours = true;
    }
    if (!hasLearnerUnlearnIntervalHours && server.hasArg("gpsLockoutLearnerUnlearnIntervalHours")) {
        learnerUnlearnIntervalHours = clampLockoutLearnerIntervalHoursValue(
            server.arg("gpsLockoutLearnerUnlearnIntervalHours").toInt());
        hasLearnerUnlearnIntervalHours = true;
    }
    if (!hasLearnerUnlearnCount && server.hasArg("lockoutLearnerUnlearnCount")) {
        learnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
            server.arg("lockoutLearnerUnlearnCount").toInt());
        hasLearnerUnlearnCount = true;
    }
    if (!hasLearnerUnlearnCount && server.hasArg("gpsLockoutLearnerUnlearnCount")) {
        learnerUnlearnCount = clampLockoutLearnerUnlearnCountValue(
            server.arg("gpsLockoutLearnerUnlearnCount").toInt());
        hasLearnerUnlearnCount = true;
    }
    if (!hasManualDemotionMissCount && server.hasArg("lockoutManualDemotionMissCount")) {
        manualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
            server.arg("lockoutManualDemotionMissCount").toInt());
        hasManualDemotionMissCount = true;
    }
    if (!hasManualDemotionMissCount && server.hasArg("gpsLockoutManualDemotionMissCount")) {
        manualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
            server.arg("gpsLockoutManualDemotionMissCount").toInt());
        hasManualDemotionMissCount = true;
    }
    if (!hasManualDemotionMissCount && server.hasArg("lockoutLearnerManualDemotionMissCount")) {
        manualDemotionMissCount = clampLockoutManualDemotionMissCountValue(
            server.arg("lockoutLearnerManualDemotionMissCount").toInt());
        hasManualDemotionMissCount = true;
    }
    if (!hasKaLearningEnabled && server.hasArg("lockoutKaLearningEnabled")) {
        String value = server.arg("lockoutKaLearningEnabled");
        value.toLowerCase();
        kaLearningEnabled = (value == "1" || value == "true" || value == "on");
        hasKaLearningEnabled = true;
    }
    if (!hasKaLearningEnabled && server.hasArg("gpsLockoutKaLearningEnabled")) {
        String value = server.arg("gpsLockoutKaLearningEnabled");
        value.toLowerCase();
        kaLearningEnabled = (value == "1" || value == "true" || value == "on");
        hasKaLearningEnabled = true;
    }
    if (!hasKLearningEnabled && server.hasArg("lockoutKLearningEnabled")) {
        String value = server.arg("lockoutKLearningEnabled");
        value.toLowerCase();
        kLearningEnabled = (value == "1" || value == "true" || value == "on");
        hasKLearningEnabled = true;
    }
    if (!hasKLearningEnabled && server.hasArg("gpsLockoutKLearningEnabled")) {
        String value = server.arg("gpsLockoutKLearningEnabled");
        value.toLowerCase();
        kLearningEnabled = (value == "1" || value == "true" || value == "on");
        hasKLearningEnabled = true;
    }
    if (!hasXLearningEnabled && server.hasArg("lockoutXLearningEnabled")) {
        String value = server.arg("lockoutXLearningEnabled");
        value.toLowerCase();
        xLearningEnabled = (value == "1" || value == "true" || value == "on");
        hasXLearningEnabled = true;
    }
    if (!hasXLearningEnabled && server.hasArg("gpsLockoutXLearningEnabled")) {
        String value = server.arg("gpsLockoutXLearningEnabled");
        value.toLowerCase();
        xLearningEnabled = (value == "1" || value == "true" || value == "on");
        hasXLearningEnabled = true;
    }
    if (!hasPreQuiet && server.hasArg("lockoutPreQuiet")) {
        String value = server.arg("lockoutPreQuiet");
        value.toLowerCase();
        preQuiet = (value == "1" || value == "true" || value == "on");
        hasPreQuiet = true;
    }
    if (!hasPreQuiet && server.hasArg("gpsLockoutPreQuiet")) {
        String value = server.arg("gpsLockoutPreQuiet");
        value.toLowerCase();
        preQuiet = (value == "1" || value == "true" || value == "on");
        hasPreQuiet = true;
    }
    if (!hasPreQuietBufferE5 && server.hasArg("lockoutPreQuietBufferE5")) {
        preQuietBufferE5 = clampLockoutPreQuietBufferE5Value(server.arg("lockoutPreQuietBufferE5").toInt());
        hasPreQuietBufferE5 = true;
    }
    if (!hasPreQuietBufferE5 && server.hasArg("gpsLockoutPreQuietBufferE5")) {
        preQuietBufferE5 = clampLockoutPreQuietBufferE5Value(server.arg("gpsLockoutPreQuietBufferE5").toInt());
        hasPreQuietBufferE5 = true;
    }
    if (!hasMaxHdopX10 && server.hasArg("lockoutMaxHdopX10")) {
        maxHdopX10 = clampLockoutGpsMaxHdopX10Value(server.arg("lockoutMaxHdopX10").toInt());
        hasMaxHdopX10 = true;
    }
    if (!hasMaxHdopX10 && server.hasArg("gpsLockoutMaxHdopX10")) {
        maxHdopX10 = clampLockoutGpsMaxHdopX10Value(server.arg("gpsLockoutMaxHdopX10").toInt());
        hasMaxHdopX10 = true;
    }
    if (!hasMinLearnerSpeedMph && server.hasArg("lockoutMinLearnerSpeedMph")) {
        minLearnerSpeedMph = clampLockoutGpsMinLearnerSpeedMphValue(server.arg("lockoutMinLearnerSpeedMph").toInt());
        hasMinLearnerSpeedMph = true;
    }
    if (!hasMinLearnerSpeedMph && server.hasArg("gpsLockoutMinLearnerSpeedMph")) {
        minLearnerSpeedMph = clampLockoutGpsMinLearnerSpeedMphValue(server.arg("gpsLockoutMinLearnerSpeedMph").toInt());
        hasMinLearnerSpeedMph = true;
    }

    if (!hasEnabled) {
        bool hasLockoutUpdate = hasLockoutMode || hasCoreGuardEnabled ||
                                hasMaxQueueDrops || hasMaxPerfDrops || hasMaxEventBusDrops ||
                                hasLearnerPromotionHits || hasLearnerRadiusE5 ||
                                hasLearnerFreqToleranceMHz || hasLearnerLearnIntervalHours ||
                                hasLearnerUnlearnIntervalHours || hasLearnerUnlearnCount ||
                                hasManualDemotionMissCount || hasKaLearningEnabled ||
                                hasKLearningEnabled || hasXLearningEnabled ||
                                hasPreQuiet || hasPreQuietBufferE5 ||
                                hasMaxHdopX10 || hasMinLearnerSpeedMph;
        if (!hasLockoutUpdate) {
            server.send(400, "application/json", "{\"success\":false,\"message\":\"Missing enabled or lockout settings\"}");
            return;
        }
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

    if (hasEnabled) {
        settingsManager.setGpsEnabled(enabled);
        gpsRuntimeModule.setEnabled(enabled);
        speedSourceSelector.syncEnabledInputs(settingsManager.get().gpsEnabled,
                                             settingsManager.get().obdEnabled);
    }

    bool lockoutSettingsChanged = false;
    bool learnerTuningChanged = false;
    if (hasLockoutMode && mutableSettings.gpsLockoutMode != lockoutMode) {
        mutableSettings.gpsLockoutMode = lockoutMode;
        lockoutSettingsChanged = true;
    }
    if (hasCoreGuardEnabled && mutableSettings.gpsLockoutCoreGuardEnabled != coreGuardEnabled) {
        mutableSettings.gpsLockoutCoreGuardEnabled = coreGuardEnabled;
        lockoutSettingsChanged = true;
    }
    if (hasMaxQueueDrops && mutableSettings.gpsLockoutMaxQueueDrops != maxQueueDrops) {
        mutableSettings.gpsLockoutMaxQueueDrops = maxQueueDrops;
        lockoutSettingsChanged = true;
    }
    if (hasMaxPerfDrops && mutableSettings.gpsLockoutMaxPerfDrops != maxPerfDrops) {
        mutableSettings.gpsLockoutMaxPerfDrops = maxPerfDrops;
        lockoutSettingsChanged = true;
    }
    if (hasMaxEventBusDrops && mutableSettings.gpsLockoutMaxEventBusDrops != maxEventBusDrops) {
        mutableSettings.gpsLockoutMaxEventBusDrops = maxEventBusDrops;
        lockoutSettingsChanged = true;
    }
    if (hasLearnerPromotionHits &&
        mutableSettings.gpsLockoutLearnerPromotionHits != learnerPromotionHits) {
        mutableSettings.gpsLockoutLearnerPromotionHits = learnerPromotionHits;
        lockoutSettingsChanged = true;
        learnerTuningChanged = true;
    }
    if (hasLearnerRadiusE5 &&
        mutableSettings.gpsLockoutLearnerRadiusE5 != learnerRadiusE5) {
        mutableSettings.gpsLockoutLearnerRadiusE5 = learnerRadiusE5;
        lockoutSettingsChanged = true;
        learnerTuningChanged = true;
    }
    if (hasLearnerFreqToleranceMHz &&
        mutableSettings.gpsLockoutLearnerFreqToleranceMHz != learnerFreqToleranceMHz) {
        mutableSettings.gpsLockoutLearnerFreqToleranceMHz = learnerFreqToleranceMHz;
        lockoutSettingsChanged = true;
        learnerTuningChanged = true;
    }
    if (hasLearnerLearnIntervalHours &&
        mutableSettings.gpsLockoutLearnerLearnIntervalHours != learnerLearnIntervalHours) {
        mutableSettings.gpsLockoutLearnerLearnIntervalHours = learnerLearnIntervalHours;
        lockoutSettingsChanged = true;
        learnerTuningChanged = true;
    }
    if (hasLearnerUnlearnIntervalHours &&
        mutableSettings.gpsLockoutLearnerUnlearnIntervalHours != learnerUnlearnIntervalHours) {
        mutableSettings.gpsLockoutLearnerUnlearnIntervalHours = learnerUnlearnIntervalHours;
        lockoutSettingsChanged = true;
    }
    if (hasLearnerUnlearnCount &&
        mutableSettings.gpsLockoutLearnerUnlearnCount != learnerUnlearnCount) {
        mutableSettings.gpsLockoutLearnerUnlearnCount = learnerUnlearnCount;
        lockoutSettingsChanged = true;
    }
    if (hasManualDemotionMissCount &&
        mutableSettings.gpsLockoutManualDemotionMissCount != manualDemotionMissCount) {
        mutableSettings.gpsLockoutManualDemotionMissCount = manualDemotionMissCount;
        lockoutSettingsChanged = true;
    }
    if (hasKaLearningEnabled &&
        mutableSettings.gpsLockoutKaLearningEnabled != kaLearningEnabled) {
        mutableSettings.gpsLockoutKaLearningEnabled = kaLearningEnabled;
        lockoutSettingsChanged = true;
    }
    if (hasKLearningEnabled &&
        mutableSettings.gpsLockoutKLearningEnabled != kLearningEnabled) {
        mutableSettings.gpsLockoutKLearningEnabled = kLearningEnabled;
        lockoutSettingsChanged = true;
    }
    if (hasXLearningEnabled &&
        mutableSettings.gpsLockoutXLearningEnabled != xLearningEnabled) {
        mutableSettings.gpsLockoutXLearningEnabled = xLearningEnabled;
        lockoutSettingsChanged = true;
    }
    if (hasPreQuiet &&
        mutableSettings.gpsLockoutPreQuiet != preQuiet) {
        mutableSettings.gpsLockoutPreQuiet = preQuiet;
        lockoutSettingsChanged = true;
    }
    if (hasPreQuietBufferE5 &&
        mutableSettings.gpsLockoutPreQuietBufferE5 != preQuietBufferE5) {
        mutableSettings.gpsLockoutPreQuietBufferE5 = preQuietBufferE5;
        lockoutSettingsChanged = true;
    }
    if (hasMaxHdopX10 &&
        mutableSettings.gpsLockoutMaxHdopX10 != maxHdopX10) {
        mutableSettings.gpsLockoutMaxHdopX10 = maxHdopX10;
        lockoutSettingsChanged = true;
        learnerTuningChanged = true;
    }
    if (hasMinLearnerSpeedMph &&
        mutableSettings.gpsLockoutMinLearnerSpeedMph != minLearnerSpeedMph) {
        mutableSettings.gpsLockoutMinLearnerSpeedMph = minLearnerSpeedMph;
        lockoutSettingsChanged = true;
        learnerTuningChanged = true;
    }
    if (hasKaLearningEnabled) {
        lockoutSetKaLearningEnabled(mutableSettings.gpsLockoutKaLearningEnabled);
    }
    if (hasKLearningEnabled) {
        lockoutSetKLearningEnabled(mutableSettings.gpsLockoutKLearningEnabled);
    }
    if (hasXLearningEnabled) {
        lockoutSetXLearningEnabled(mutableSettings.gpsLockoutXLearningEnabled);
    }
    if (learnerTuningChanged) {
        lockoutLearner.setTuning(mutableSettings.gpsLockoutLearnerPromotionHits,
                                 mutableSettings.gpsLockoutLearnerRadiusE5,
                                 mutableSettings.gpsLockoutLearnerFreqToleranceMHz,
                                 mutableSettings.gpsLockoutLearnerLearnIntervalHours,
                                 mutableSettings.gpsLockoutMaxHdopX10,
                                 mutableSettings.gpsLockoutMinLearnerSpeedMph);
    }
    if (lockoutSettingsChanged) {
        settingsManager.save();
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
    const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
        settings.gpsLockoutCoreGuardEnabled,
        settings.gpsLockoutMaxQueueDrops,
        settings.gpsLockoutMaxPerfDrops,
        settings.gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(),
        perfCounters.perfDrop.load(),
        systemEventBus.getDropCount());

    JsonDocument response;
    response["success"] = true;
    response["enabled"] = settings.gpsEnabled;
    response["runtimeEnabled"] = gpsStatus.enabled;
    response["sampleValid"] = gpsStatus.sampleValid;
    response["hasFix"] = gpsStatus.hasFix;
    response["injectedSamples"] = gpsStatus.injectedSamples;
    response["speedSource"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    appendLockoutConfig(response, settings);
    response["lockoutCoreGuardTripped"] = lockoutGuard.tripped;
    response["lockoutCoreGuardReason"] = lockoutGuard.reason;
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
                        const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendConfig(server, settingsManager);
}

void handleApiConfig(WebServer& server,
                     SettingsManager& settingsManager,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     LockoutLearner& lockoutLearner,
                     GpsObservationLog& gpsObservationLog,
                     PerfCounters& perfCounters,
                     SystemEventBus& systemEventBus,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleConfig(server,
                 settingsManager,
                 gpsRuntimeModule,
                 speedSourceSelector,
                 lockoutLearner,
                 gpsObservationLog,
                 perfCounters,
                 systemEventBus);
}

}  // namespace GpsApiService
