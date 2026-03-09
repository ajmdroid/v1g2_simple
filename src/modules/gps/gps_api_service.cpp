#include "gps_api_service.h"

#include <ArduinoJson.h>
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
#include "../../../include/clamp_utils.h"

namespace GpsApiService {

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
    doc["stableHasFix"] = gpsStatus.stableHasFix;
    doc["satellites"] = gpsStatus.satellites;
    doc["stableSatellites"] = gpsStatus.stableSatellites;
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
    if (gpsStatus.stableFixAgeMs == UINT32_MAX) {
        doc["stableFixAgeMs"] = nullptr;
    } else {
        doc["stableFixAgeMs"] = gpsStatus.stableFixAgeMs;
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
    speedObj["gpsFresh"] = speedStatus.gpsFresh;
    speedObj["sourceSwitches"] = speedStatus.sourceSwitches;
    if (speedStatus.selectedSource == SpeedSource::NONE) {
        speedObj["selectedMph"] = nullptr;
        speedObj["selectedAgeMs"] = nullptr;
    } else {
        speedObj["selectedMph"] = speedStatus.selectedSpeedMph;
        speedObj["selectedAgeMs"] = speedStatus.selectedAgeMs;
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
    lockoutObj["kLearningEnabled"] = settings.gpsLockoutKLearningEnabled;
    lockoutObj["xLearningEnabled"] = settings.gpsLockoutXLearningEnabled;
    lockoutObj["preQuiet"] = settings.gpsLockoutPreQuiet;
    lockoutObj["preQuietBufferE5"] = settings.gpsLockoutPreQuietBufferE5;
    lockoutObj["maxHdopX10"] = settings.gpsLockoutMaxHdopX10;
    lockoutObj["minLearnerSpeedMph"] = settings.gpsLockoutMinLearnerSpeedMph;
    lockoutObj["minSatellites"] = LOCKOUT_GPS_MIN_SATELLITES;
    lockoutObj["enforceAllowed"] = (settings.gpsLockoutMode == LOCKOUT_RUNTIME_ENFORCE) &&
                                   !lockoutGuard.tripped;
    // Backward-compatible top-level aliases used by older web clients.
    doc["gpsLockoutKaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    doc["gpsLockoutPreQuiet"] = settings.gpsLockoutPreQuiet;

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
        limit = clamp_utils::clampU16Value(server.arg("limit").toInt(), 1, 32);
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


}  // namespace GpsApiService
