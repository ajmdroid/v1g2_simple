#include "gps_api_service.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>

#include "gps_runtime_module.h"
#include "gps_lockout_safety.h"
#include "gps_observation_log.h"
#include "../lockout/lockout_learner.h"
#include "../lockout/lockout_band_policy.h"
#include "../speed/speed_source_selector.h"
#include "../system/system_event_bus.h"
#include "../../settings.h"
#include "../../settings_sanitize.h"
#include "../../perf_metrics.h"

namespace GpsApiService {

namespace {

uint16_t clampU16Value(int value, int minVal, int maxVal) {
    return static_cast<uint16_t>(std::max(minVal, std::min(value, maxVal)));
}

}  // namespace

void sendStatus(WebServer& server,
                GpsRuntimeModule& gpsRuntimeModule,
                SpeedSourceSelector& speedSourceSelector,
                SettingsManager& settingsManager,
                GpsObservationLog& gpsObservationLog,
                LockoutLearner& lockoutLearner,
                PerfCounters& perfCounters,
                SystemEventBus& systemEventBus) {
    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot(nowMs);

    JsonDocument doc;
    doc["enabled"] = settingsManager.get().gpsEnabled;
    doc["runtimeEnabled"] = gpsStatus.enabled;
    doc["mode"] = (gpsStatus.parserActive || gpsStatus.moduleDetected || gpsStatus.hardwareSamples > 0)
                      ? "runtime"
                      : "scaffold";
    doc["sampleValid"] = gpsStatus.sampleValid;
    doc["hasFix"] = gpsStatus.hasFix;
    doc["satellites"] = gpsStatus.satellites;
    doc["injectedSamples"] = gpsStatus.injectedSamples;
    doc["moduleDetected"] = gpsStatus.moduleDetected;
    doc["detectionTimedOut"] = gpsStatus.detectionTimedOut;
    doc["parserActive"] = gpsStatus.parserActive;
    doc["hardwareSamples"] = gpsStatus.hardwareSamples;
    doc["bytesRead"] = gpsStatus.bytesRead;
    doc["sentencesSeen"] = gpsStatus.sentencesSeen;
    doc["sentencesParsed"] = gpsStatus.sentencesParsed;
    doc["parseFailures"] = gpsStatus.parseFailures;
    doc["checksumFailures"] = gpsStatus.checksumFailures;
    doc["bufferOverruns"] = gpsStatus.bufferOverruns;

    if (std::isnan(gpsStatus.hdop)) {
        doc["hdop"] = nullptr;
    } else {
        doc["hdop"] = gpsStatus.hdop;
    }
    doc["locationValid"] = gpsStatus.locationValid;
    if (gpsStatus.locationValid) {
        doc["latitude"] = gpsStatus.latitudeDeg;
        doc["longitude"] = gpsStatus.longitudeDeg;
    } else {
        doc["latitude"] = nullptr;
        doc["longitude"] = nullptr;
    }
    doc["courseValid"] = gpsStatus.courseValid;
    if (gpsStatus.courseValid) {
        doc["courseDeg"] = gpsStatus.courseDeg;
        doc["courseSampleTsMs"] = gpsStatus.courseSampleTsMs;
    } else {
        doc["courseDeg"] = nullptr;
        doc["courseSampleTsMs"] = nullptr;
    }

    if (gpsStatus.sampleValid) {
        doc["speedMph"] = gpsStatus.speedMph;
        doc["sampleTsMs"] = gpsStatus.sampleTsMs;
    } else {
        doc["speedMph"] = nullptr;
        doc["sampleTsMs"] = nullptr;
    }

    if (gpsStatus.sampleAgeMs == UINT32_MAX) {
        doc["sampleAgeMs"] = nullptr;
    } else {
        doc["sampleAgeMs"] = gpsStatus.sampleAgeMs;
    }
    if (gpsStatus.courseAgeMs == UINT32_MAX) {
        doc["courseAgeMs"] = nullptr;
    } else {
        doc["courseAgeMs"] = gpsStatus.courseAgeMs;
    }
    if (gpsStatus.lastSentenceTsMs == 0) {
        doc["lastSentenceTsMs"] = nullptr;
    } else {
        doc["lastSentenceTsMs"] = gpsStatus.lastSentenceTsMs;
    }
    const GpsObservationLogStats gpsLogStats = gpsObservationLog.stats();
    JsonObject observationsObj = doc["observations"].to<JsonObject>();
    observationsObj["published"] = gpsLogStats.published;
    observationsObj["drops"] = gpsLogStats.drops;
    observationsObj["size"] = static_cast<uint32_t>(gpsLogStats.size);
    observationsObj["capacity"] = static_cast<uint32_t>(GpsObservationLog::kCapacity);

    JsonObject speedObj = doc["speedSource"].to<JsonObject>();
    speedObj["selected"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    speedObj["gpsEnabled"] = speedStatus.gpsEnabled;
    speedObj["obdConnected"] = speedStatus.obdConnected;
    speedObj["obdFresh"] = speedStatus.obdFresh;
    speedObj["gpsFresh"] = speedStatus.gpsFresh;
    speedObj["sourceSwitches"] = speedStatus.sourceSwitches;
    if (speedStatus.selectedSource == SpeedSource::NONE) {
        speedObj["selectedMph"] = nullptr;
        speedObj["selectedAgeMs"] = nullptr;
    } else {
        speedObj["selectedMph"] = speedStatus.selectedSpeedMph;
        speedObj["selectedAgeMs"] = speedStatus.selectedAgeMs;
    }
    if (speedStatus.obdAgeMs == UINT32_MAX) {
        speedObj["obdAgeMs"] = nullptr;
    } else {
        speedObj["obdAgeMs"] = speedStatus.obdAgeMs;
    }
    if (speedStatus.gpsAgeMs == UINT32_MAX) {
        speedObj["gpsAgeMs"] = nullptr;
    } else {
        speedObj["gpsAgeMs"] = speedStatus.gpsAgeMs;
    }

    const V1Settings& settings = settingsManager.get();
    const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
        settings.gpsLockoutCoreGuardEnabled,
        settings.gpsLockoutMaxQueueDrops,
        settings.gpsLockoutMaxPerfDrops,
        settings.gpsLockoutMaxEventBusDrops,
        perfCounters.queueDrops.load(),
        perfCounters.perfDrop.load(),
        systemEventBus.getDropCount());
    JsonObject lockoutObj = doc["lockout"].to<JsonObject>();
    lockoutObj["mode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    lockoutObj["modeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    lockoutObj["coreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    lockoutObj["maxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    lockoutObj["maxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    lockoutObj["maxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    lockoutObj["coreGuardTripped"] = lockoutGuard.tripped;
    lockoutObj["coreGuardReason"] = lockoutGuard.reason;
    lockoutObj["learnerPromotionHits"] = static_cast<uint32_t>(lockoutLearner.promotionHits());
    lockoutObj["learnerRadiusE5"] = static_cast<uint32_t>(lockoutLearner.radiusE5());
    lockoutObj["learnerFreqToleranceMHz"] = static_cast<uint32_t>(lockoutLearner.freqToleranceMHz());
    lockoutObj["learnerLearnIntervalHours"] = static_cast<uint32_t>(lockoutLearner.learnIntervalHours());
    lockoutObj["learnerUnlearnIntervalHours"] = static_cast<uint32_t>(settings.gpsLockoutLearnerUnlearnIntervalHours);
    lockoutObj["learnerUnlearnCount"] = static_cast<uint32_t>(settings.gpsLockoutLearnerUnlearnCount);
    lockoutObj["manualDemotionMissCount"] = static_cast<uint32_t>(settings.gpsLockoutManualDemotionMissCount);
    lockoutObj["kaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    lockoutObj["enforceAllowed"] = (settings.gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE) &&
                                   !lockoutGuard.tripped;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiStatus(WebServer& server,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     SettingsManager& settingsManager,
                     GpsObservationLog& gpsObservationLog,
                     LockoutLearner& lockoutLearner,
                     PerfCounters& perfCounters,
                     SystemEventBus& systemEventBus,
                     const std::function<void()>& markUiActivity) {
    if (markUiActivity) {
        markUiActivity();
    }
    sendStatus(server,
               gpsRuntimeModule,
               speedSourceSelector,
               settingsManager,
               gpsObservationLog,
               lockoutLearner,
               perfCounters,
               systemEventBus);
}

void sendObservations(WebServer& server,
                      GpsObservationLog& gpsObservationLog) {
    uint16_t limit = 16;
    if (server.hasArg("limit")) {
        limit = clampU16Value(server.arg("limit").toInt(), 1, 32);
    }

    GpsObservation samples[32] = {};
    const size_t count = gpsObservationLog.copyRecent(samples, limit);
    const GpsObservationLogStats stats = gpsObservationLog.stats();

    JsonDocument doc;
    doc["success"] = true;
    doc["count"] = static_cast<uint32_t>(count);
    doc["published"] = stats.published;
    doc["drops"] = stats.drops;
    doc["size"] = static_cast<uint32_t>(stats.size);
    doc["capacity"] = static_cast<uint32_t>(GpsObservationLog::kCapacity);

    JsonArray samplesArray = doc["samples"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
        const GpsObservation& sample = samples[i];
        JsonObject entry = samplesArray.add<JsonObject>();
        entry["tsMs"] = sample.tsMs;
        entry["hasFix"] = sample.hasFix;
        entry["satellites"] = sample.satellites;
        if (std::isnan(sample.hdop)) {
            entry["hdop"] = nullptr;
        } else {
            entry["hdop"] = sample.hdop;
        }
        if (sample.speedValid) {
            entry["speedMph"] = sample.speedMph;
        } else {
            entry["speedMph"] = nullptr;
        }
        if (sample.locationValid) {
            entry["latitude"] = sample.latitudeDeg;
            entry["longitude"] = sample.longitudeDeg;
        } else {
            entry["latitude"] = nullptr;
            entry["longitude"] = nullptr;
        }
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiObservations(WebServer& server,
                           GpsObservationLog& gpsObservationLog,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendObservations(server, gpsObservationLog);
}

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
    bool hasPreQuiet = false;
    bool preQuiet = currentSettings.gpsLockoutPreQuiet;

    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument body;
        DeserializationError error = deserializeJson(body, server.arg("plain"));
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
            lockoutMode = gpsLockoutParseRuntimeModeArg(body["lockoutMode"].as<String>(), lockoutMode);
            hasLockoutMode = true;
        } else if (body["gpsLockoutMode"].is<int>()) {
            lockoutMode = clampLockoutRuntimeModeValue(body["gpsLockoutMode"].as<int>());
            hasLockoutMode = true;
        } else if (body["gpsLockoutMode"].is<const char*>()) {
            lockoutMode = gpsLockoutParseRuntimeModeArg(body["gpsLockoutMode"].as<String>(), lockoutMode);
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
            maxQueueDrops = clampU16Value(body["lockoutMaxQueueDrops"].as<int>(), 0, 65535);
            hasMaxQueueDrops = true;
        } else if (body["gpsLockoutMaxQueueDrops"].is<int>()) {
            maxQueueDrops = clampU16Value(body["gpsLockoutMaxQueueDrops"].as<int>(), 0, 65535);
            hasMaxQueueDrops = true;
        }
        if (body["lockoutMaxPerfDrops"].is<int>()) {
            maxPerfDrops = clampU16Value(body["lockoutMaxPerfDrops"].as<int>(), 0, 65535);
            hasMaxPerfDrops = true;
        } else if (body["gpsLockoutMaxPerfDrops"].is<int>()) {
            maxPerfDrops = clampU16Value(body["gpsLockoutMaxPerfDrops"].as<int>(), 0, 65535);
            hasMaxPerfDrops = true;
        }
        if (body["lockoutMaxEventBusDrops"].is<int>()) {
            maxEventBusDrops = clampU16Value(body["lockoutMaxEventBusDrops"].as<int>(), 0, 65535);
            hasMaxEventBusDrops = true;
        } else if (body["gpsLockoutMaxEventBusDrops"].is<int>()) {
            maxEventBusDrops = clampU16Value(body["gpsLockoutMaxEventBusDrops"].as<int>(), 0, 65535);
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
        if (body["lockoutPreQuiet"].is<bool>()) {
            preQuiet = body["lockoutPreQuiet"].as<bool>();
            hasPreQuiet = true;
        } else if (body["gpsLockoutPreQuiet"].is<bool>()) {
            preQuiet = body["gpsLockoutPreQuiet"].as<bool>();
            hasPreQuiet = true;
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
        maxQueueDrops = clampU16Value(server.arg("lockoutMaxQueueDrops").toInt(), 0, 65535);
        hasMaxQueueDrops = true;
    }
    if (!hasMaxQueueDrops && server.hasArg("gpsLockoutMaxQueueDrops")) {
        maxQueueDrops = clampU16Value(server.arg("gpsLockoutMaxQueueDrops").toInt(), 0, 65535);
        hasMaxQueueDrops = true;
    }
    if (!hasMaxPerfDrops && server.hasArg("lockoutMaxPerfDrops")) {
        maxPerfDrops = clampU16Value(server.arg("lockoutMaxPerfDrops").toInt(), 0, 65535);
        hasMaxPerfDrops = true;
    }
    if (!hasMaxPerfDrops && server.hasArg("gpsLockoutMaxPerfDrops")) {
        maxPerfDrops = clampU16Value(server.arg("gpsLockoutMaxPerfDrops").toInt(), 0, 65535);
        hasMaxPerfDrops = true;
    }
    if (!hasMaxEventBusDrops && server.hasArg("lockoutMaxEventBusDrops")) {
        maxEventBusDrops = clampU16Value(server.arg("lockoutMaxEventBusDrops").toInt(), 0, 65535);
        hasMaxEventBusDrops = true;
    }
    if (!hasMaxEventBusDrops && server.hasArg("gpsLockoutMaxEventBusDrops")) {
        maxEventBusDrops = clampU16Value(server.arg("gpsLockoutMaxEventBusDrops").toInt(), 0, 65535);
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

    if (!hasEnabled) {
        bool hasLockoutUpdate = hasLockoutMode || hasCoreGuardEnabled ||
                                hasMaxQueueDrops || hasMaxPerfDrops || hasMaxEventBusDrops ||
                                hasLearnerPromotionHits || hasLearnerRadiusE5 ||
                                hasLearnerFreqToleranceMHz || hasLearnerLearnIntervalHours ||
                                hasLearnerUnlearnIntervalHours || hasLearnerUnlearnCount ||
                                hasManualDemotionMissCount || hasKaLearningEnabled ||
                                hasPreQuiet;
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
        speedSourceSelector.setGpsEnabled(enabled);
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
    if (hasPreQuiet &&
        mutableSettings.gpsLockoutPreQuiet != preQuiet) {
        mutableSettings.gpsLockoutPreQuiet = preQuiet;
        lockoutSettingsChanged = true;
    }
    if (hasKaLearningEnabled) {
        lockoutSetKaLearningEnabled(mutableSettings.gpsLockoutKaLearningEnabled);
    }
    if (learnerTuningChanged) {
        lockoutLearner.setTuning(mutableSettings.gpsLockoutLearnerPromotionHits,
                                 mutableSettings.gpsLockoutLearnerRadiusE5,
                                 mutableSettings.gpsLockoutLearnerFreqToleranceMHz,
                                 mutableSettings.gpsLockoutLearnerLearnIntervalHours);
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
    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot(nowMs);
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
    response["enabled"] = settingsManager.get().gpsEnabled;
    response["runtimeEnabled"] = gpsStatus.enabled;
    response["sampleValid"] = gpsStatus.sampleValid;
    response["hasFix"] = gpsStatus.hasFix;
    response["injectedSamples"] = gpsStatus.injectedSamples;
    response["speedSource"] = SpeedSourceSelector::sourceName(speedStatus.selectedSource);
    response["lockoutMode"] = lockoutRuntimeModeName(settings.gpsLockoutMode);
    response["lockoutModeRaw"] = static_cast<int>(settings.gpsLockoutMode);
    response["lockoutCoreGuardEnabled"] = settings.gpsLockoutCoreGuardEnabled;
    response["lockoutMaxQueueDrops"] = settings.gpsLockoutMaxQueueDrops;
    response["lockoutMaxPerfDrops"] = settings.gpsLockoutMaxPerfDrops;
    response["lockoutMaxEventBusDrops"] = settings.gpsLockoutMaxEventBusDrops;
    response["lockoutLearnerPromotionHits"] = settings.gpsLockoutLearnerPromotionHits;
    response["lockoutLearnerRadiusE5"] = settings.gpsLockoutLearnerRadiusE5;
    response["lockoutLearnerFreqToleranceMHz"] = settings.gpsLockoutLearnerFreqToleranceMHz;
    response["lockoutLearnerLearnIntervalHours"] = settings.gpsLockoutLearnerLearnIntervalHours;
    response["lockoutLearnerUnlearnIntervalHours"] = settings.gpsLockoutLearnerUnlearnIntervalHours;
    response["lockoutLearnerUnlearnCount"] = settings.gpsLockoutLearnerUnlearnCount;
    response["lockoutManualDemotionMissCount"] = settings.gpsLockoutManualDemotionMissCount;
    response["lockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    response["lockoutPreQuiet"] = settings.gpsLockoutPreQuiet;
    response["gpsLockoutLearnerPromotionHits"] = settings.gpsLockoutLearnerPromotionHits;
    response["gpsLockoutLearnerRadiusE5"] = settings.gpsLockoutLearnerRadiusE5;
    response["gpsLockoutLearnerFreqToleranceMHz"] = settings.gpsLockoutLearnerFreqToleranceMHz;
    response["gpsLockoutLearnerLearnIntervalHours"] = settings.gpsLockoutLearnerLearnIntervalHours;
    response["gpsLockoutLearnerUnlearnIntervalHours"] = settings.gpsLockoutLearnerUnlearnIntervalHours;
    response["gpsLockoutLearnerUnlearnCount"] = settings.gpsLockoutLearnerUnlearnCount;
    response["gpsLockoutManualDemotionMissCount"] = settings.gpsLockoutManualDemotionMissCount;
    response["gpsLockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    response["gpsLockoutPreQuiet"] = settings.gpsLockoutPreQuiet;
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

}  // namespace GpsApiService
