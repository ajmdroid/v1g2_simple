#include "lockout_store.h"
#include "lockout_band_policy.h"
#include "lockout_entry.h"
#include "lockout_index.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

#include <algorithm>
#include <cstring>

LockoutStore lockoutStore;

namespace {

uint8_t clampDirectionMode(int raw) {
    if (raw <= static_cast<int>(LockoutEntry::DIRECTION_ALL)) {
        return LockoutEntry::DIRECTION_ALL;
    }
    if (raw >= static_cast<int>(LockoutEntry::DIRECTION_REVERSE)) {
        return LockoutEntry::DIRECTION_REVERSE;
    }
    return static_cast<uint8_t>(raw);
}

uint8_t clampHeadingTolerance(int raw) {
    if (raw < 0) return 0;
    if (raw > 90) return 90;
    return static_cast<uint8_t>(raw);
}

uint16_t clampHeading(int raw) {
    if (raw < 0 || raw >= 360) {
        return LockoutEntry::HEADING_INVALID;
    }
    return static_cast<uint16_t>(raw);
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void LockoutStore::begin(LockoutIndex* index) {
    index_ = index;
    dirty_ = false;
    stats_ = Stats{};
}

// ---------------------------------------------------------------------------
// Serialization: index → JSON
// ---------------------------------------------------------------------------

void LockoutStore::toJson(JsonDocument& doc) const {
    if (!index_) return;

    doc["_type"]    = kTypeTag;
    doc["_version"] = kVersion;

    JsonArray zones = doc["zones"].to<JsonArray>();
    uint32_t count = 0;

    for (size_t i = 0; i < index_->capacity(); ++i) {
        const LockoutEntry* e = index_->at(i);
        if (!e || !e->isActive()) continue;
        const uint8_t bandMask = lockoutSanitizeBandMask(e->bandMask);
        if (bandMask == 0) continue;

        JsonObject z = zones.add<JsonObject>();
        z["lat"]   = e->latE5;
        z["lon"]   = e->lonE5;
        z["rad"]   = e->radiusE5;
        z["band"]  = bandMask;
        z["freq"]  = e->freqMHz;
        z["ftol"]  = e->freqTolMHz;
        z["conf"]  = e->confidence;
        z["flags"] = e->flags;
        z["dir"]   = clampDirectionMode(e->directionMode);
        z["hdg"]   = (e->headingDeg == LockoutEntry::HEADING_INVALID) ? -1 : e->headingDeg;
        z["htol"]  = clampHeadingTolerance(e->headingTolDeg);
        z["miss"]  = e->missCount;
        z["first"] = e->firstSeenMs;
        z["last"]  = e->lastSeenMs;
        z["pass"]  = e->lastPassMs;
        z["mms"]   = e->lastCountedMissMs;
        ++count;
    }

    stats_.entriesSaved = count;
}

// ---------------------------------------------------------------------------
// Deserialization: JSON → index
// ---------------------------------------------------------------------------

bool LockoutStore::fromJson(JsonDocument& doc) {
    if (!index_) {
        Serial.println("[LockoutStore] fromJson: no index wired");
        ++stats_.loadErrors;
        return false;
    }

    // Validate type tag.
    const char* type = doc["_type"];
    if (!type || strcmp(type, kTypeTag) != 0) {
        Serial.printf("[LockoutStore] Invalid type: %s\n", type ? type : "(null)");
        ++stats_.loadErrors;
        return false;
    }

    // Validate version.
    uint8_t version = doc["_version"] | (uint8_t)0;
    if (version != kVersion) {
        Serial.printf("[LockoutStore] Unknown version: %u\n", version);
        ++stats_.loadErrors;
        return false;
    }

    // Zones array.
    JsonArray zones = doc["zones"];
    if (zones.isNull()) {
        Serial.println("[LockoutStore] Missing zones array");
        ++stats_.loadErrors;
        return false;
    }

    // Clear the index before populating.
    index_->clear();

    uint32_t loaded  = 0;
    uint32_t skipped = 0;

    for (JsonObject z : zones) {
        if (loaded >= index_->capacity()) {
            Serial.printf("[LockoutStore] Capacity reached (%u), truncating\n",
                          static_cast<unsigned>(index_->capacity()));
            break;
        }

        // lat and lon are required.
        if (z["lat"].isNull() || z["lon"].isNull()) {
            ++skipped;
            continue;
        }

        LockoutEntry entry;
        entry.latE5      = z["lat"].as<int32_t>();
        entry.lonE5      = z["lon"].as<int32_t>();
        entry.radiusE5   = z["rad"]  | (uint16_t)135;
        entry.bandMask   = lockoutSanitizeBandMask(z["band"] | (uint8_t)0);
        if (entry.bandMask == 0) {
            ++skipped;
            continue;
        }
        entry.freqMHz    = z["freq"] | (uint16_t)0;
        entry.freqTolMHz = z["ftol"] | (uint16_t)10;
        entry.confidence = z["conf"] | (uint8_t)100;
        entry.flags      = z["flags"] | (uint8_t)LockoutEntry::FLAG_ACTIVE;
        entry.directionMode = clampDirectionMode(z["dir"] | static_cast<int>(LockoutEntry::DIRECTION_ALL));
        entry.headingDeg = clampHeading(z["hdg"] | -1);
        entry.headingTolDeg = clampHeadingTolerance(z["htol"] | 45);
        entry.missCount  = z["miss"] | (uint8_t)0;
        entry.firstSeenMs = z["first"] | (int64_t)0;
        entry.lastSeenMs  = z["last"]  | (int64_t)0;
        entry.lastPassMs  = z["pass"]  | (int64_t)0;
        entry.lastCountedMissMs = z["mms"] | (int64_t)0;

        if (entry.directionMode != LockoutEntry::DIRECTION_ALL &&
            entry.headingDeg == LockoutEntry::HEADING_INVALID) {
            // Corrupt directional metadata: disable direction gate instead of creating
            // an entry that can never match.
            entry.directionMode = LockoutEntry::DIRECTION_ALL;
        }

        // Always ensure the entry is active (we only serialize active entries,
        // so deserialize should restore them as active).
        entry.setActive(true);

        int slot = index_->add(entry);
        if (slot >= 0) {
            ++loaded;
        }
    }

    if (skipped > 0) {
        Serial.printf("[LockoutStore] Skipped %lu entries (missing lat/lon or unsupported band)\n",
                      static_cast<unsigned long>(skipped));
    }

    stats_.entriesLoaded  = loaded;
    stats_.entriesSkipped = skipped;
    ++stats_.loads;

    Serial.printf("[LockoutStore] Loaded %lu entries\n",
                  static_cast<unsigned long>(loaded));

    return true;
}
