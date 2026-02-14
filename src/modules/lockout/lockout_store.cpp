#include "lockout_store.h"
#include "lockout_band_policy.h"
#include "lockout_entry.h"
#include "lockout_index.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

#include <cstring>

LockoutStore lockoutStore;

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
        z["first"] = e->firstSeenMs;
        z["last"]  = e->lastSeenMs;
        z["pass"]  = e->lastPassMs;
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
        entry.radiusE5   = z["rad"]  | (uint16_t)1350;
        entry.bandMask   = lockoutSanitizeBandMask(z["band"] | (uint8_t)0);
        if (entry.bandMask == 0) {
            ++skipped;
            continue;
        }
        entry.freqMHz    = z["freq"] | (uint16_t)0;
        entry.freqTolMHz = z["ftol"] | (uint16_t)10;
        entry.confidence = z["conf"] | (uint8_t)100;
        entry.flags      = z["flags"] | (uint8_t)LockoutEntry::FLAG_ACTIVE;
        entry.firstSeenMs = z["first"] | (int64_t)0;
        entry.lastSeenMs  = z["last"]  | (int64_t)0;
        entry.lastPassMs  = z["pass"]  | (int64_t)0;

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
