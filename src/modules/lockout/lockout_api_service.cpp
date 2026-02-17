#include "lockout_api_service.h"

#include <ArduinoJson.h>

#include "lockout_index.h"
#include "lockout_learner.h"
#include "lockout_store.h"
#include "signal_observation_log.h"
#include "signal_observation_sd_logger.h"
#include "../../settings.h"
#include "../../../include/band_utils.h"

namespace {

uint16_t clampU16Value(int value, int minVal, int maxVal) {
    if (value < minVal) return static_cast<uint16_t>(minVal);
    if (value > maxVal) return static_cast<uint16_t>(maxVal);
    return static_cast<uint16_t>(value);
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
                      const std::function<void()>& markUiActivity,
                      const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    sendSummary(server, signalObservationLog, signalObservationSdLogger);
}

void sendEvents(WebServer& server,
                SignalObservationLog& signalObservationLog,
                SignalObservationSdLogger& signalObservationSdLogger) {
    uint16_t limit = 32;
    if (server.hasArg("limit")) {
        limit = clampU16Value(server.arg("limit").toInt(), 1, 128);
    }

    // Keep this large scratch buffer off the loop-task stack.
    static SignalObservation samples[128];
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
        entry["bandRaw"] = sample.bandRaw;
        entry["frequencyMHz"] = sample.frequencyMHz;
        entry["strength"] = sample.strength;
        entry["hasFix"] = sample.hasFix;
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

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiEvents(WebServer& server,
                     SignalObservationLog& signalObservationLog,
                     SignalObservationSdLogger& signalObservationSdLogger,
                     const std::function<bool()>& checkRateLimit,
                     const std::function<void()>& markUiActivity,
                     const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
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
    // Bound endpoint cost (JSON size + serialization time) for lower-tier web UI.
    uint16_t activeLimit = 64;
    uint16_t pendingLimit = 64;
    if (server.hasArg("activeLimit")) {
        activeLimit = clampU16Value(server.arg("activeLimit").toInt(), 1, 200);
    }
    if (server.hasArg("pendingLimit")) {
        pendingLimit = clampU16Value(server.arg("pendingLimit").toInt(), 1, 64);
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
    doc["activeCount"] = static_cast<uint32_t>(lockoutIndex.activeCount());
    doc["activeCapacity"] = static_cast<uint32_t>(lockoutIndex.capacity());
    doc["pendingCount"] = static_cast<uint32_t>(lockoutLearner.activeCandidateCount());
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

    JsonArray activeZones = doc["activeZones"].to<JsonArray>();
    uint16_t activeReturned = 0;
    for (size_t i = 0; i < lockoutIndex.capacity() && activeReturned < activeLimit; ++i) {
        const LockoutEntry* entry = lockoutIndex.at(i);
        if (!entry || !entry->isActive()) {
            continue;
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
        zone["firstSeenMs"] = entry->firstSeenMs;
        zone["lastSeenMs"] = entry->lastSeenMs;
        zone["lastPassMs"] = entry->lastPassMs;
        zone["missCount"] = entry->missCount;
        zone["lastCountedMissMs"] = entry->lastCountedMissMs;
        const uint8_t demotionThreshold = entry->isManual() ? manualDemotionMissCount : unlearnCount;
        if (demotionThreshold > 0) {
            zone["demotionMissThreshold"] = demotionThreshold;
            zone["demotionMissesRemaining"] = static_cast<uint8_t>(
                (entry->missCount >= demotionThreshold) ? 0 : (demotionThreshold - entry->missCount));
        } else {
            zone["demotionMissThreshold"] = nullptr;
            zone["demotionMissesRemaining"] = nullptr;
        }
        ++activeReturned;
    }
    doc["activeReturned"] = activeReturned;

    JsonArray pendingZones = doc["pendingZones"].to<JsonArray>();
    uint16_t pendingReturned = 0;
    for (size_t i = 0; i < LockoutLearner::kCandidateCapacity && pendingReturned < pendingLimit; ++i) {
        const LearnerCandidate* candidate = lockoutLearner.candidateAt(i);
        if (!candidate || !candidate->active) {
            continue;
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
        ++pendingReturned;
    }
    doc["pendingReturned"] = pendingReturned;

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiZones(WebServer& server,
                    LockoutIndex& lockoutIndex,
                    LockoutLearner& lockoutLearner,
                    SettingsManager& settingsManager,
                    const std::function<bool()>& checkRateLimit,
                    const std::function<void()>& markUiActivity,
                    const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
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
                         const std::function<void()>& markUiActivity,
                         const std::function<void()>& sendDeprecatedHeader) {
    if (sendDeprecatedHeader) {
        sendDeprecatedHeader();
    }
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    handleZoneDelete(server, lockoutIndex, lockoutStore);
}

void handleZoneDelete(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore) {
    int slot = -1;
    if (server.hasArg("plain") && server.arg("plain").length() > 0) {
        JsonDocument body;
        const DeserializationError error = deserializeJson(body, server.arg("plain"));
        if (error) {
            server.send(400, "application/json",
                        "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }
        if (body["slot"].is<int>()) {
            slot = body["slot"].as<int>();
        }
    }
    if (slot < 0 && server.hasArg("slot")) {
        slot = server.arg("slot").toInt();
    }
    if (slot < 0 || slot >= static_cast<int>(lockoutIndex.capacity())) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"slot out of range\"}");
        return;
    }

    const LockoutEntry* entry = lockoutIndex.at(static_cast<size_t>(slot));
    if (!entry || !entry->isActive()) {
        server.send(404, "application/json",
                    "{\"success\":false,\"message\":\"zone not found\"}");
        return;
    }
    if (!entry->isLearned()) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"only learned zones are deletable\"}");
        return;
    }

    if (!lockoutIndex.remove(static_cast<size_t>(slot))) {
        server.send(500, "application/json",
                    "{\"success\":false,\"message\":\"failed to delete zone\"}");
        return;
    }
    lockoutStore.markDirty();

    JsonDocument responseDoc;
    responseDoc["success"] = true;
    responseDoc["slot"] = slot;
    responseDoc["activeCount"] = static_cast<uint32_t>(lockoutIndex.activeCount());
    String response;
    serializeJson(responseDoc, response);
    server.send(200, "application/json", response);
}

}  // namespace LockoutApiService
