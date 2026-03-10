#pragma once

#include <stdint.h>
#include <ArduinoJson.h>
#include <FS.h>

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
    static constexpr const char* kBinaryPath = "/v1simple_lockout_zones.bin";
    static constexpr const char* kJsonMigratedBackupPath = "/v1simple_lockout_zones.json.migrated.bak";
    inline static constexpr uint8_t kBinaryMagic[4] = {'L', 'Z', 'O', 'N'};
    static constexpr uint16_t kBinaryVersion = 1;

    struct __attribute__((packed)) LockoutDiskHeader {
        uint8_t magic[4];
        uint16_t version;
        uint16_t entryCount;
        uint32_t payloadBytes;
        uint32_t payloadCrc32;
    };
    static_assert(sizeof(LockoutDiskHeader) == 16, "LockoutDiskHeader must be 16 bytes");

    struct __attribute__((packed)) LockoutDiskEntry {
        int32_t latE5;
        int32_t lonE5;
        uint16_t radiusE5;
        uint8_t bandMask;
        uint16_t freqMHz;
        uint16_t freqTolMHz;
        uint8_t confidence;
        uint8_t flags;
        uint8_t directionMode;
        uint8_t headingTolDeg;
        uint8_t missCount;
        uint16_t headingDeg;
        int64_t firstSeenMs;
        int64_t lastSeenMs;
        int64_t lastPassMs;
        int64_t lastCountedMissMs;
    };
    static_assert(sizeof(LockoutDiskEntry) == 54, "LockoutDiskEntry must be 54 bytes");

    /// Wire the index dependency.  Must be called once before toJson/fromJson.
    void begin(LockoutIndex* index);

    /// True after begin() has been called with a non-null index.
    bool isInitialized() const { return index_ != nullptr; }

    /// Populate a JsonDocument with all active entries from the index.
    /// Caller owns the doc (typically stack-allocated JsonDocument).
    void toJson(JsonDocument& doc) const;

    /// Clear the index, then populate it from the given doc.
    /// Validates _type and _version.  Skips entries missing lat/lon.
    /// Truncates at index capacity (500).
    /// Returns true on success (even if some entries were skipped).
    bool fromJson(JsonDocument& doc);

    /// Atomically save active lockout entries using the binary disk format.
    bool saveBinary(fs::FS& fs, const char* path) const;

    /// Load lockout entries from the binary disk format.
    bool loadBinary(fs::FS& fs, const char* path);

    // --- Dirty tracking (for rate-limited saves) ---

    void markDirty()   { dirty_ = true; }
    bool isDirty() const { return dirty_; }
    void clearDirty()  { dirty_ = false; }

    // --- Stats ---

    struct Stats {
        uint32_t loads         = 0; // Successful fromJson calls
        uint32_t saves         = 0; // Successful binary saves
        uint32_t loadErrors    = 0; // Failed fromJson calls
        uint32_t entriesLoaded = 0; // Entries populated on last load
        uint32_t entriesSaved  = 0; // Entries serialized on last save/export
        uint32_t entriesSkipped = 0; // Entries skipped on last load (missing fields)
    };
    const Stats& stats() const { return stats_; }

private:
    LockoutIndex* index_ = nullptr;
    bool          dirty_ = false;
    mutable Stats stats_ = {};
};

extern LockoutStore lockoutStore;
