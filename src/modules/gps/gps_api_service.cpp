#include "gps_api_service.h"

#include <ArduinoJson.h>
#include <cmath>

#ifndef UNIT_TEST
#include "gps_runtime_module.h"
#include "gps_observation_log.h"
#include "../speed/speed_source_selector.h"
#include "../system/system_event_bus.h"
#include "../../settings.h"
#include "../../perf_metrics.h"
#endif
#include "../../../include/clamp_utils.h"
#include "json_stream_response.h"

namespace GpsApiService {

void sendStatus(WebServer& server,
                GpsRuntimeModule& gpsRuntimeModule,
                SpeedSourceSelector& speedSourceSelector,
                SettingsManager& settingsManager,
                GpsObservationLog& gpsObservationLog) {
    const uint32_t nowMs = millis();
    const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
    const SpeedSelectorStatus speedStatus = speedSourceSelector.snapshot();

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

    sendJsonStream(server, doc);
}

void handleApiStatus(WebServer& server,
                     GpsRuntimeModule& gpsRuntimeModule,
                     SpeedSourceSelector& speedSourceSelector,
                     SettingsManager& settingsManager,
                     GpsObservationLog& gpsObservationLog,
                     void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendStatus(server,
               gpsRuntimeModule,
               speedSourceSelector,
               settingsManager,
               gpsObservationLog);
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

    sendJsonStream(server, doc);
}

void handleApiObservations(WebServer& server,
                           GpsObservationLog& gpsObservationLog,
                           bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                           void (*markUiActivity)(void* ctx), void* uiActivityCtx) {
    if (checkRateLimit && !checkRateLimit(rateLimitCtx)) return;
    if (markUiActivity) {
        markUiActivity(uiActivityCtx);
    }
    sendObservations(server, gpsObservationLog);
}


}  // namespace GpsApiService
