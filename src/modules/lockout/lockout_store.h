#pragma once

#include <stdint.h>
#include <ArduinoJson.h>

class LockoutIndex;

/// JSON serialization / deserialization for LockoutIndex.
///
/// Converts the flat-array index to/from a compact JSON document.
/// Designed for periodic SD card persistence (Tier 7 — best-effort,
/// drops OK, corruption not).
///
/// File format (v1):
/// {
///   "_type": "v1simple_lockout_zones",
///   "_version": 1,
///   "zones": [ { "lat":…, "lon":…, … }, … ]
/// }
///
/// Thread safety: single-threaded from loop() like everything else
/// in the lockout subsystem.
class LockoutStore {
public:
    static constexpr const char* kTypeTag = "v1simple_lockout_zones";
    static constexpr uint8_t     kVersion = 1;

    /// Wire the index dependency.  Must be called once before toJson/fromJson.
    void begin(LockoutIndex* index);

    /// Populate a JsonDocument with all active entries from the index.
    /// Caller owns the doc (typically stack-allocated JsonDocument).
    void toJson(JsonDocument& doc) const;

    /// Clear the index, then populate it from the given doc.
    /// Validates _type and _version.  Skips entries missing lat/lon.
    /// Truncates at index capacity (200).
    /// Returns true on success (even if some entries were skipped).
    bool fromJson(JsonDocument& doc);

    // --- Dirty tracking (for rate-limited saves) ---

    void markDirty()   { dirty_ = true; }
    bool isDirty() const { return dirty_; }
    void clearDirty()  { dirty_ = false; }

    // --- Stats ---

    struct Stats {
        uint32_t loads         = 0; // Successful fromJson calls
        uint32_t saves         = 0; // Successful toJson calls (caller increments)
        uint32_t loadErrors    = 0; // Failed fromJson calls
        uint32_t entriesLoaded = 0; // Entries populated on last load
        uint32_t entriesSaved  = 0; // Entries serialized on last save
        uint32_t entriesSkipped = 0; // Entries skipped on last load (missing fields)
    };
    const Stats& stats() const { return stats_; }

private:
    LockoutIndex* index_ = nullptr;
    bool          dirty_ = false;
    mutable Stats stats_ = {};
};

extern LockoutStore lockoutStore;
