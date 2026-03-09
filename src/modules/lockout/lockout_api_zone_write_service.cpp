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

bool parseDirectionModeArg(const JsonVariantConst& value, uint8_t& outMode) {
    if (value.is<int>()) {
        const int raw = value.as<int>();
        if (raw < static_cast<int>(LockoutEntry::DIRECTION_ALL) ||
            raw > static_cast<int>(LockoutEntry::DIRECTION_REVERSE)) {
            return false;
        }
        outMode = static_cast<uint8_t>(raw);
        return true;
    }
    if (value.is<const char*>()) {
        String token = value.as<String>();
        token.toLowerCase();
        token.trim();
        if (token == "all") {
            outMode = LockoutEntry::DIRECTION_ALL;
            return true;
        }
        if (token == "forward") {
            outMode = LockoutEntry::DIRECTION_FORWARD;
            return true;
        }
        if (token == "reverse") {
            outMode = LockoutEntry::DIRECTION_REVERSE;
            return true;
        }
    }
    return false;
}

bool parseBoolArg(const JsonObjectConst& body, const char* key, bool& outValue) {
    if (body[key].isNull()) return false;
    if (!body[key].is<bool>()) return false;
    outValue = body[key].as<bool>();
    return true;
}

bool parseFloatArg(const JsonObjectConst& body, const char* key, float& outValue) {
    if (body[key].isNull()) return false;
    if (!body[key].is<float>() && !body[key].is<double>() && !body[key].is<int>()) {
        return false;
    }
    outValue = body[key].as<float>();
    return true;
}

bool parseIntArg(const JsonObjectConst& body, const char* key, int& outValue) {
    if (body[key].isNull()) return false;
    if (!body[key].is<int>()) return false;
    outValue = body[key].as<int>();
    return true;
}

int32_t degreesToE5(float degrees) {
    return static_cast<int32_t>(lroundf(degrees * 100000.0f));
}

uint16_t clampHeadingTol(int raw) {
    if (raw < 0) return 0;
    if (raw > 90) return 90;
    return static_cast<uint16_t>(raw);
}

bool parseZoneBody(const JsonObjectConst& body,
                   LockoutEntry& entry,
                   bool partialUpdate,
                   String& errorMessage) {
    bool hasLatitude = false;
    bool hasLongitude = false;
    bool hasBandMask = false;

    float latitudeDeg = 0.0f;
    float longitudeDeg = 0.0f;
    int bandMaskRaw = 0;

    if (parseFloatArg(body, "latitude", latitudeDeg) || parseFloatArg(body, "lat", latitudeDeg)) {
        hasLatitude = true;
    }
    if (parseFloatArg(body, "longitude", longitudeDeg) || parseFloatArg(body, "lon", longitudeDeg)) {
        hasLongitude = true;
    }
    if (parseIntArg(body, "bandMask", bandMaskRaw) || parseIntArg(body, "band", bandMaskRaw)) {
        hasBandMask = true;
    }

    if (!partialUpdate) {
        if (!hasLatitude || !hasLongitude) {
            errorMessage = "latitude and longitude are required";
            return false;
        }
        if (!hasBandMask) {
            errorMessage = "bandMask is required";
            return false;
        }
    } else if (hasLatitude != hasLongitude) {
        errorMessage = "latitude and longitude must be provided together";
        return false;
    }

    if (hasLatitude && hasLongitude) {
        if (!std::isfinite(latitudeDeg) || !std::isfinite(longitudeDeg) ||
            latitudeDeg < -90.0f || latitudeDeg > 90.0f ||
            longitudeDeg < -180.0f || longitudeDeg > 180.0f) {
            errorMessage = "latitude/longitude out of range";
            return false;
        }
        entry.latE5 = degreesToE5(latitudeDeg);
        entry.lonE5 = degreesToE5(longitudeDeg);
    }

    if (hasBandMask) {
        const uint8_t sanitized = lockoutSanitizeBandMask(static_cast<uint8_t>(bandMaskRaw));
        if (sanitized == 0) {
            errorMessage = "unsupported band mask";
            return false;
        }
        entry.bandMask = sanitized;
    }

    int radiusE5Raw = 0;
    if (parseIntArg(body, "radiusE5", radiusE5Raw) || parseIntArg(body, "rad", radiusE5Raw)) {
        entry.radiusE5 = clampLockoutLearnerRadiusE5Value(radiusE5Raw);
    }

    int frequencyRaw = 0;
    if (parseIntArg(body, "frequencyMHz", frequencyRaw) || parseIntArg(body, "freq", frequencyRaw)) {
        entry.freqMHz = static_cast<uint16_t>(std::max(0, std::min(65535, frequencyRaw)));
    }

    int freqTolRaw = 0;
    if (parseIntArg(body, "frequencyToleranceMHz", freqTolRaw) ||
        parseIntArg(body, "ftol", freqTolRaw)) {
        entry.freqTolMHz = static_cast<uint16_t>(std::max(0, std::min(65535, freqTolRaw)));
    }

    int confidenceRaw = 0;
    if (parseIntArg(body, "confidence", confidenceRaw) || parseIntArg(body, "conf", confidenceRaw)) {
        entry.confidence = static_cast<uint8_t>(std::max(0, std::min(255, confidenceRaw)));
    }

    bool manualFlag = false;
    if (parseBoolArg(body, "manual", manualFlag)) {
        entry.setManual(manualFlag);
    }

    bool learnedFlag = false;
    if (parseBoolArg(body, "learned", learnedFlag)) {
        entry.setLearned(learnedFlag);
    }

    if (!body["directionMode"].isNull() || !body["dir"].isNull()) {
        const JsonVariantConst value = !body["directionMode"].isNull()
                                           ? body["directionMode"]
                                           : body["dir"];
        uint8_t parsedMode = entry.directionMode;
        if (!parseDirectionModeArg(value, parsedMode)) {
            errorMessage = "invalid directionMode";
            return false;
        }
        entry.directionMode = parsedMode;
    }

    if (!body["headingToleranceDeg"].isNull() || !body["htol"].isNull()) {
        int headingTolRaw = 0;
        const bool hasTol = parseIntArg(body, "headingToleranceDeg", headingTolRaw) ||
                            parseIntArg(body, "htol", headingTolRaw);
        if (!hasTol) {
            errorMessage = "invalid headingToleranceDeg";
            return false;
        }
        entry.headingTolDeg = static_cast<uint8_t>(clampHeadingTol(headingTolRaw));
    }

    if (!body["headingDeg"].isNull() || !body["hdg"].isNull()) {
        const JsonVariantConst value = !body["headingDeg"].isNull()
                                           ? body["headingDeg"]
                                           : body["hdg"];
        if (value.isNull()) {
            entry.headingDeg = LockoutEntry::HEADING_INVALID;
        } else if (value.is<int>()) {
            const int headingRaw = value.as<int>();
            if (headingRaw < 0 || headingRaw >= 360) {
                errorMessage = "headingDeg out of range";
                return false;
            }
            entry.headingDeg = static_cast<uint16_t>(headingRaw);
        } else {
            errorMessage = "invalid headingDeg";
            return false;
        }
    }

    if (entry.directionMode != LockoutEntry::DIRECTION_ALL &&
        entry.headingDeg == LockoutEntry::HEADING_INVALID) {
        errorMessage = "headingDeg is required for directional lockouts";
        return false;
    }

    if (entry.directionMode == LockoutEntry::DIRECTION_ALL &&
        entry.headingDeg == LockoutEntry::HEADING_INVALID) {
        entry.headingTolDeg = 45;
    }

    return true;
}

bool parseRequestJson(WebServer& server, JsonDocument& body) {
    if (!server.hasArg("plain") || server.arg("plain").length() == 0) {
        return false;
    }
    const DeserializationError error = deserializeJson(body, server.arg("plain"));
    return !error;
}

void appendZoneSummary(JsonDocument& doc, int slot, const LockoutEntry& entry) {
    doc["slot"] = slot;
    doc["latitude"] = static_cast<double>(entry.latE5) / 100000.0;
    doc["longitude"] = static_cast<double>(entry.lonE5) / 100000.0;
    doc["radiusE5"] = entry.radiusE5;
    doc["bandMask"] = entry.bandMask;
    doc["frequencyMHz"] = entry.freqMHz;
    doc["frequencyToleranceMHz"] = entry.freqTolMHz;
    doc["confidence"] = entry.confidence;
    doc["manual"] = entry.isManual();
    doc["learned"] = entry.isLearned();
    doc["directionMode"] = lockoutDirectionModeName(entry.directionMode);
    doc["directionModeRaw"] = entry.directionMode;
    if (entry.headingDeg == LockoutEntry::HEADING_INVALID) {
        doc["headingDeg"] = nullptr;
    } else {
        doc["headingDeg"] = entry.headingDeg;
    }
    doc["headingToleranceDeg"] = entry.headingTolDeg;
}


}  // anonymous namespace

namespace LockoutApiService {

void handleApiPendingClear(WebServer& server,
                           LockoutLearner& lockoutLearner,
                           const std::function<bool()>& checkRateLimit,
                           const std::function<void()>& markUiActivity) {
    if (checkRateLimit && !checkRateLimit()) return;
    if (markUiActivity) {
        markUiActivity();
    }
    const uint32_t count = static_cast<uint32_t>(lockoutLearner.activeCandidateCount());
    lockoutLearner.clearCandidates();
    JsonDocument doc;
    doc["success"] = true;
    doc["cleared"] = count;
    sendJsonStream(server, doc);
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
    const bool wasManual = entry->isManual();
    const bool wasLearned = entry->isLearned();

    if (!lockoutIndex.remove(static_cast<size_t>(slot))) {
        server.send(500, "application/json",
                    "{\"success\":false,\"message\":\"failed to delete zone\"}");
        return;
    }
    lockoutStore.markDirty();

    JsonDocument responseDoc;
    responseDoc["success"] = true;
    responseDoc["slot"] = slot;
    responseDoc["manual"] = wasManual;
    responseDoc["learned"] = wasLearned;
    responseDoc["activeCount"] = static_cast<uint32_t>(lockoutIndex.activeCount());
    sendJsonStream(server, responseDoc);
}

void handleZoneCreate(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore) {
    JsonDocument body;
    if (!parseRequestJson(server, body)) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    LockoutEntry entry;
    entry.radiusE5 = LOCKOUT_LEARNER_RADIUS_E5_DEFAULT;
    entry.freqTolMHz = LOCKOUT_LEARNER_FREQ_TOL_DEFAULT;
    entry.confidence = 100;
    entry.flags = LockoutEntry::FLAG_ACTIVE | LockoutEntry::FLAG_MANUAL;
    entry.directionMode = LockoutEntry::DIRECTION_ALL;
    entry.headingDeg = LockoutEntry::HEADING_INVALID;
    entry.headingTolDeg = 45;

    String errorMessage;
    if (!parseZoneBody(body.as<JsonObjectConst>(), entry, false, errorMessage)) {
        JsonDocument responseDoc;
        responseDoc["success"] = false;
        responseDoc["message"] = errorMessage;
        sendJsonStream(server, responseDoc, 400);
        return;
    }

    // Create endpoint is manual-first by design.
    entry.setActive(true);
    entry.setManual(true);
    entry.setLearned(false);

    const int slot = lockoutIndex.addOrUpdate(entry);
    if (slot < 0) {
        server.send(507, "application/json",
                    "{\"success\":false,\"message\":\"lockout index full\"}");
        return;
    }
    lockoutStore.markDirty();

    const LockoutEntry* stored = lockoutIndex.at(static_cast<size_t>(slot));
    JsonDocument responseDoc;
    responseDoc["success"] = true;
    responseDoc["activeCount"] = static_cast<uint32_t>(lockoutIndex.activeCount());
    if (stored) {
        appendZoneSummary(responseDoc, slot, *stored);
    } else {
        responseDoc["slot"] = slot;
    }
    sendJsonStream(server, responseDoc);
}

void handleZoneUpdate(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore) {
    JsonDocument body;
    if (!parseRequestJson(server, body)) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"Invalid JSON\"}");
        return;
    }

    const JsonObjectConst obj = body.as<JsonObjectConst>();
    int slot = -1;
    if (!parseIntArg(obj, "slot", slot)) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"slot is required\"}");
        return;
    }
    if (slot < 0 || slot >= static_cast<int>(lockoutIndex.capacity())) {
        server.send(400, "application/json",
                    "{\"success\":false,\"message\":\"slot out of range\"}");
        return;
    }

    const LockoutEntry* current = lockoutIndex.at(static_cast<size_t>(slot));
    if (!current || !current->isActive()) {
        server.send(404, "application/json",
                    "{\"success\":false,\"message\":\"zone not found\"}");
        return;
    }

    LockoutEntry updated = *current;
    String errorMessage;
    if (!parseZoneBody(obj, updated, true, errorMessage)) {
        JsonDocument responseDoc;
        responseDoc["success"] = false;
        responseDoc["message"] = errorMessage;
        sendJsonStream(server, responseDoc, 400);
        return;
    }
    updated.setActive(true);

    LockoutEntry* slotPtr = lockoutIndex.mutableAt(static_cast<size_t>(slot));
    if (!slotPtr) {
        server.send(500, "application/json",
                    "{\"success\":false,\"message\":\"failed to update slot\"}");
        return;
    }
    *slotPtr = updated;
    lockoutStore.markDirty();

    JsonDocument responseDoc;
    responseDoc["success"] = true;
    responseDoc["activeCount"] = static_cast<uint32_t>(lockoutIndex.activeCount());
    appendZoneSummary(responseDoc, slot, updated);
    sendJsonStream(server, responseDoc);
}

void sendZoneExport(WebServer& server,
                    LockoutStore& lockoutStore) {
    JsonDocument doc;
    lockoutStore.toJson(doc);
    doc["exportedAtMs"] = millis();
    sendJsonStream(server, doc);
}

void handleZoneImport(WebServer& server,
                      LockoutIndex& lockoutIndex,
                      LockoutStore& lockoutStore) {
    // Scope each JsonDocument to minimize peak internal SRAM usage.
    // Previous approach had 4 concurrent documents (~40-80 KiB peak);
    // this sequence limits peak to 2 documents at a time.
    LockoutIndex tempIndex;
    LockoutStore tempStore;
    tempStore.begin(&tempIndex);

    // Phase 1: Parse and validate import payload (importDoc alive).
    {
        JsonDocument importDoc;
        if (!parseRequestJson(server, importDoc)) {
            server.send(400, "application/json",
                        "{\"success\":false,\"message\":\"Invalid JSON\"}");
            return;
        }

        if (!tempStore.fromJson(importDoc)) {
            server.send(400, "application/json",
                        "{\"success\":false,\"message\":\"Invalid lockout import payload\"}");
            return;
        }
    }  // importDoc freed

    // Phase 2: Normalize and build backup (2 docs alive: normalizedDoc + backupDoc).
    JsonDocument normalizedDoc;
    tempStore.toJson(normalizedDoc);

    JsonDocument backupDoc;
    lockoutStore.toJson(backupDoc);
    if (!lockoutStore.fromJson(normalizedDoc)) {
        (void)lockoutStore.fromJson(backupDoc);
        server.send(500, "application/json",
                    "{\"success\":false,\"message\":\"failed to apply imported zones\"}");
        return;
    }
    lockoutStore.markDirty();

    JsonDocument responseDoc;
    responseDoc["success"] = true;
    responseDoc["activeCount"] = static_cast<uint32_t>(lockoutIndex.activeCount());
    responseDoc["entriesImported"] = lockoutStore.stats().entriesLoaded;
    sendJsonStream(server, responseDoc);
}


}  // namespace LockoutApiService
