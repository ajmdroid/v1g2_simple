#include "lockout_api_service.h"

#include <ArduinoJson.h>
#include <cmath>
#include "json_stream_response.h"
#include <esp_heap_caps.h>

#include "lockout_index.h"
#include "lockout_learner.h"
#include "lockout_store.h"
#include "lockout_band_policy.h"
#include "signal_observation_log.h"
#include "signal_observation_sd_logger.h"
#include "../../settings.h"
#include "../../../include/band_utils.h"
#include "../../../include/clamp_utils.h"

namespace {

const char* lockoutDirectionModeName(uint8_t mode) {
    switch (mode) {
        case LockoutEntry::DIRECTION_FORWARD:
            return "forward";
        case LockoutEntry::DIRECTION_REVERSE:
            return "reverse";
        case LockoutEntry::DIRECTION_ALL:
        default:
            return "all";
    }
}

// Shared helper: append signal-observation log stats + SD-logger stats to a JSON doc.
void appendSignalObsStats(JsonDocument& doc,
                          const SignalObservationLogStats& stats,
                          const SignalObservationSdStats& sdStats,
                          const String& sdPath) {
    doc["published"] = stats.published;
    doc["drops"] = stats.drops;
    doc["size"] = static_cast<uint32_t>(stats.size);
    doc["capacity"] = static_cast<uint32_t>(SignalObservationLog::kCapacity);
    JsonObject sdObj = doc["sd"].to<JsonObject>();
    sdObj["enabled"] = sdStats.enabled;
    if (sdStats.enabled) {
        sdObj["path"] = sdPath;
    } else {
        sdObj["path"] = nullptr;
    }
    sdObj["enqueued"] = sdStats.enqueued;
    sdObj["queueDrops"] = sdStats.queueDrops;
    sdObj["deduped"] = sdStats.deduped;
    sdObj["written"] = sdStats.written;
    sdObj["writeFail"] = sdStats.writeFail;
    sdObj["rotations"] = sdStats.rotations;
}

}  // anonymous namespace

namespace LockoutApiService {

void handleZoneDelete(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);
void handleZoneCreate(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);
void handleZoneUpdate(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);
void sendZoneExport(WebServer& server,
                    LockoutStore& lockoutStore);
void handleZoneImport(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore);

void sendSummary(WebServer& server,
                 SignalObservationLog& signalObservationLog,
                 SignalObservationSdLogger& signalObservationSdLogger) {
    const SignalObservationLogStats stats = signalObservationLog.stats();
    SignalObservation latest[1] = {};
    const size_t latestCount = signalObservationLog.copyRecent(latest, 1);

    JsonDocument doc;
    doc["success"] = true;
    appendSignalObsStats(doc, stats, signalObservationSdLogger.stats(),
                         signalObservationSdLogger.csvPath());

    if (latestCount == 1) {
        const SignalObservation& sample = latest[0];
        JsonObject latestObj = doc["latest"].to<JsonObject>();
        latestObj["tsMs"] = sample.tsMs;
        latestObj["band"] = bandName(static_cast<Band>(sample.bandRaw));
        latestObj["bandRaw"] = sample.bandRaw;
        latestObj["frequencyMHz"] = sample.frequencyMHz;
        latestObj["strength"] = sample.strength;
        latestObj["hasFix"] = sample.hasFix;
        if (sample.fixAgeMs == UINT32_MAX) {
            latestObj["fixAgeMs"] = nullptr;
        } else {
            latestObj["fixAgeMs"] = sample.fixAgeMs;
        }
        latestObj["satellites"] = sample.satellites;
        latestObj["locationValid"] = sample.locationValid;
        if (sample.hdopX10 == SignalObservation::HDOP_X10_INVALID) {
            latestObj["hdop"] = nullptr;
        } else {
            latestObj["hdop"] = static_cast<float>(sample.hdopX10) / 10.0f;
        }
        if (sample.locationValid) {
            latestObj["latitude"] = static_cast<double>(sample.latitudeE5) / 100000.0;
            latestObj["longitude"] = static_cast<double>(sample.longitudeE5) / 100000.0;
        } else {
            latestObj["latitude"] = nullptr;
            latestObj["longitude"] = nullptr;
        }
    } else {
        doc["latest"] = nullptr;
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiSummary(WebServer& server,
                      SignalObservationLog& signalObservationLog,
                      SignalObservationSdLogger& signalObservationSdLogger,
                      const std::function<bool()>& checkRateLimit,
                      const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendSummary(server, signalObservationLog, signalObservationSdLogger);
}

void sendEvents(WebServer& server,
                SignalObservationLog& signalObservationLog,
                SignalObservationSdLogger& signalObservationSdLogger) {
    uint16_t limit = 24;
    if (server.hasArg("limit")) {
        limit = clamp_utils::clampU16Value(server.arg("limit").toInt(), 1, 96);
    }

    // Scratch buffer in PSRAM — saves ~5.6 KiB internal .bss.
    // Allocated once on first call; never freed (HTTP handlers are single-threaded).
    static SignalObservation* samples = nullptr;
    if (!samples) {
        samples = static_cast<SignalObservation*>(
            heap_caps_malloc(128 * sizeof(SignalObservation),
                            MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    }
    if (!samples) {
        server.send(500, "application/json",
                    "{\"success\":false,\"error\":\"PSRAM alloc failed\"}");
        return;
    }
    const size_t count = signalObservationLog.copyRecent(samples, limit);
    const SignalObservationLogStats stats = signalObservationLog.stats();

    JsonDocument doc;
    doc["success"] = true;
    doc["count"] = static_cast<uint32_t>(count);
    appendSignalObsStats(doc, stats, signalObservationSdLogger.stats(),
                         signalObservationSdLogger.csvPath());

    JsonArray events = doc["events"].to<JsonArray>();
    for (size_t i = 0; i < count; ++i) {
        const SignalObservation& sample = samples[i];
        JsonObject entry = events.add<JsonObject>();
        entry["tsMs"] = sample.tsMs;
        entry["band"] = bandName(static_cast<Band>(sample.bandRaw));
        entry["frequencyMHz"] = sample.frequencyMHz;
        entry["strength"] = sample.strength;
        if (sample.fixAgeMs == UINT32_MAX) {
            entry["fixAgeMs"] = nullptr;
        } else {
            entry["fixAgeMs"] = sample.fixAgeMs;
        }
        entry["satellites"] = sample.satellites;
        if (sample.hdopX10 == SignalObservation::HDOP_X10_INVALID) {
            entry["hdop"] = nullptr;
        } else {
            entry["hdop"] = static_cast<float>(sample.hdopX10) / 10.0f;
        }
        entry["locationValid"] = sample.locationValid;
        if (sample.locationValid) {
            entry["latitude"] = static_cast<double>(sample.latitudeE5) / 100000.0;
            entry["longitude"] = static_cast<double>(sample.longitudeE5) / 100000.0;
        } else {
            entry["latitude"] = nullptr;
            entry["longitude"] = nullptr;
        }
    }

    sendJsonStream(server, doc);
}

void handleApiEvents(WebServer& server,
                     SignalObservationLog& signalObservationLog,
                     SignalObservationSdLogger& signalObservationSdLogger,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendEvents(server, signalObservationLog, signalObservationSdLogger);
}

void sendZones(WebServer& server,
               LockoutIndex& lockoutIndex,
               LockoutLearner& lockoutLearner,
               SettingsManager& settingsManager) {
    // Bound endpoint cost (JSON size + serialization time) for embedded web UI.
    uint16_t activeLimit = 24;
    uint16_t pendingLimit = 24;
    uint16_t activeOffset = 0;
    uint16_t pendingOffset = 0;
    if (server.hasArg("activeLimit")) {
        activeLimit = clamp_utils::clampU16Value(server.arg("activeLimit").toInt(), 1, 96);
    }
    if (server.hasArg("pendingLimit")) {
        pendingLimit = clamp_utils::clampU16Value(server.arg("pendingLimit").toInt(), 1, 48);
    }

    const int activeOffsetMax =
        (lockoutIndex.capacity() > 0) ? static_cast<int>(lockoutIndex.capacity() - 1) : 0;
    const int pendingOffsetMax =
        (LockoutLearner::kCandidateCapacity > 0) ? static_cast<int>(LockoutLearner::kCandidateCapacity - 1) : 0;
    if (server.hasArg("activeOffset")) {
        activeOffset = clamp_utils::clampU16Value(server.arg("activeOffset").toInt(), 0, activeOffsetMax);
    }
    if (server.hasArg("pendingOffset")) {
        pendingOffset = clamp_utils::clampU16Value(server.arg("pendingOffset").toInt(), 0, pendingOffsetMax);
    }

    JsonDocument doc;
    const uint8_t promotionHits = lockoutLearner.promotionHits();
    const V1Settings& settings = settingsManager.get();
    const uint8_t learnIntervalHours = lockoutLearner.learnIntervalHours();
    const uint8_t unlearnIntervalHours = settings.gpsLockoutLearnerUnlearnIntervalHours;
    const uint8_t unlearnCount = settings.gpsLockoutLearnerUnlearnCount;
    const uint8_t manualDemotionMissCount = settings.gpsLockoutManualDemotionMissCount;
    const int64_t learnIntervalMs = (learnIntervalHours > 0)
                                        ? static_cast<int64_t>(learnIntervalHours) * 3600LL * 1000LL
                                        : 0;
    doc["success"] = true;
    const uint32_t activeCount = static_cast<uint32_t>(lockoutIndex.activeCount());
    const uint32_t pendingCount = static_cast<uint32_t>(lockoutLearner.activeCandidateCount());
    doc["activeCount"] = activeCount;
    doc["activeCapacity"] = static_cast<uint32_t>(lockoutIndex.capacity());
    doc["pendingCount"] = pendingCount;
    doc["pendingCapacity"] = static_cast<uint32_t>(LockoutLearner::kCandidateCapacity);
    doc["promotionHits"] = static_cast<uint32_t>(promotionHits);
    doc["promotionRadiusE5"] = static_cast<uint32_t>(lockoutLearner.radiusE5());
    doc["promotionFreqToleranceMHz"] = static_cast<uint32_t>(lockoutLearner.freqToleranceMHz());
    doc["learnIntervalHours"] = static_cast<uint32_t>(learnIntervalHours);
    doc["unlearnIntervalHours"] = static_cast<uint32_t>(unlearnIntervalHours);
    doc["unlearnCount"] = static_cast<uint32_t>(unlearnCount);
    doc["manualDemotionMissCount"] = static_cast<uint32_t>(manualDemotionMissCount);
    doc["kaLearningEnabled"] = settings.gpsLockoutKaLearningEnabled;
    doc["activeLimit"] = activeLimit;
    doc["pendingLimit"] = pendingLimit;
    doc["activeOffset"] = activeOffset;
    doc["pendingOffset"] = pendingOffset;

    const String detailsArg = server.arg("details");
    const bool includeDetails = detailsArg == "1" || detailsArg == "true";

    JsonArray activeZones = doc["activeZones"].to<JsonArray>();
    uint16_t activeSeen = 0;
    uint16_t activeReturned = 0;
    for (size_t i = 0; i < lockoutIndex.capacity(); ++i) {
        const LockoutEntry* entry = lockoutIndex.at(i);
        if (!entry || !entry->isActive()) {
            continue;
        }
        if (activeSeen < activeOffset) {
            ++activeSeen;
            continue;
        }
        if (activeReturned >= activeLimit) {
            break;
        }
        JsonObject zone = activeZones.add<JsonObject>();
        zone["slot"] = static_cast<uint32_t>(i);
        zone["latitude"] = static_cast<double>(entry->latE5) / 100000.0;
        zone["longitude"] = static_cast<double>(entry->lonE5) / 100000.0;
        zone["radiusE5"] = entry->radiusE5;
        zone["radiusM"] = static_cast<float>(entry->radiusE5) * 1.11f;
        zone["bandMask"] = entry->bandMask;
        zone["frequencyMHz"] = entry->freqMHz;
        zone["frequencyToleranceMHz"] = entry->freqTolMHz;
        zone["confidence"] = entry->confidence;
        zone["manual"] = entry->isManual();
        zone["learned"] = entry->isLearned();
        zone["directionModeRaw"] = entry->directionMode;
        zone["directionMode"] = lockoutDirectionModeName(entry->directionMode);
        if (entry->headingDeg == LockoutEntry::HEADING_INVALID) {
            zone["headingDeg"] = nullptr;
        } else {
            zone["headingDeg"] = entry->headingDeg;
        }
        zone["headingToleranceDeg"] = entry->headingTolDeg;
        zone["missCount"] = entry->missCount;
        const uint8_t demotionThreshold = entry->isManual() ? manualDemotionMissCount : unlearnCount;
        if (demotionThreshold > 0) {
            zone["demotionMissThreshold"] = demotionThreshold;
            zone["demotionMissesRemaining"] = static_cast<uint8_t>(
                (entry->missCount >= demotionThreshold) ? 0 : (demotionThreshold - entry->missCount));
        } else {
            zone["demotionMissThreshold"] = nullptr;
            zone["demotionMissesRemaining"] = nullptr;
        }
        if (includeDetails) {
            zone["firstSeenMs"] = entry->firstSeenMs;
            zone["lastSeenMs"] = entry->lastSeenMs;
            zone["lastPassMs"] = entry->lastPassMs;
            zone["lastCountedMissMs"] = entry->lastCountedMissMs;
        }
        ++activeSeen;
        ++activeReturned;
    }
    doc["activeReturned"] = activeReturned;
    if (static_cast<uint32_t>(activeOffset) + activeReturned < activeCount) {
        doc["activeNextOffset"] = static_cast<uint32_t>(activeOffset) + activeReturned;
    } else {
        doc["activeNextOffset"] = nullptr;
    }

    JsonArray pendingZones = doc["pendingZones"].to<JsonArray>();
    uint16_t pendingSeen = 0;
    uint16_t pendingReturned = 0;
    for (size_t i = 0; i < LockoutLearner::kCandidateCapacity; ++i) {
        const LearnerCandidate* candidate = lockoutLearner.candidateAt(i);
        if (!candidate || !candidate->active) {
            continue;
        }
        if (pendingSeen < pendingOffset) {
            ++pendingSeen;
            continue;
        }
        if (pendingReturned >= pendingLimit) {
            break;
        }
        JsonObject pending = pendingZones.add<JsonObject>();
        pending["slot"] = static_cast<uint32_t>(i);
        pending["latitude"] = static_cast<double>(candidate->latE5) / 100000.0;
        pending["longitude"] = static_cast<double>(candidate->lonE5) / 100000.0;
        pending["bandRaw"] = candidate->band;
        pending["band"] = bandName(static_cast<Band>(candidate->band));
        pending["frequencyMHz"] = candidate->freqMHz;
        pending["hitCount"] = candidate->hitCount;
        pending["hitsRemaining"] = static_cast<uint8_t>(
            (candidate->hitCount >= promotionHits)
                ? 0
                : (promotionHits - candidate->hitCount));
        pending["firstSeenMs"] = candidate->firstSeenMs;
        pending["lastSeenMs"] = candidate->lastSeenMs;
        pending["lastCountedHitMs"] = candidate->lastCountedHitMs;
        if (learnIntervalMs > 0 && candidate->lastCountedHitMs > 0) {
            pending["nextEligibleHitMs"] = candidate->lastCountedHitMs + learnIntervalMs;
        } else {
            pending["nextEligibleHitMs"] = nullptr;
        }
        ++pendingSeen;
        ++pendingReturned;
    }
    doc["pendingReturned"] = pendingReturned;
    if (static_cast<uint32_t>(pendingOffset) + pendingReturned < pendingCount) {
        doc["pendingNextOffset"] = static_cast<uint32_t>(pendingOffset) + pendingReturned;
    } else {
        doc["pendingNextOffset"] = nullptr;
    }

    sendJsonStream(server, doc);
}

void handleApiZones(WebServer& server,
                    LockoutIndex& lockoutIndex,
                    LockoutLearner& lockoutLearner,
                    SettingsManager& settingsManager,
                    const std::function<bool()>& checkRateLimit,
                    const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendZones(server, lockoutIndex, lockoutLearner, settingsManager);
}

void handleApiZoneDelete(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleZoneDelete(server, lockoutIndex, lockoutStore);
}

void handleApiZoneCreate(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleZoneCreate(server, lockoutIndex, lockoutStore);
}

void handleApiZoneUpdate(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleZoneUpdate(server, lockoutIndex, lockoutStore);
}

void handleApiZoneExport(WebServer& server,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendZoneExport(server, lockoutStore);
}

void handleApiZoneImport(WebServer& server,
                         LockoutIndex& lockoutIndex,
                         LockoutStore& lockoutStore,
                         const std::function<bool()>& checkRateLimit,
                         const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleZoneImport(server, lockoutIndex, lockoutStore);
}


}  // namespace LockoutApiService
