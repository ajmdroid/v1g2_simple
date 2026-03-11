#include "lockout_store.h"
#include "lockout_band_policy.h"
#include "lockout_entry.h"
#include "lockout_index.h"
#include "../../storage_manager.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#else
#include "../../../test/mocks/Arduino.h"
#endif

#include <algorithm>
#include <cstring>
#include <memory>

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

void diskEntryFromRuntime(const LockoutEntry& entry, LockoutStore::LockoutDiskEntry& diskEntry) {
    memset(&diskEntry, 0, sizeof(diskEntry));
    diskEntry.latE5 = entry.latE5;
    diskEntry.lonE5 = entry.lonE5;
    diskEntry.radiusE5 = entry.radiusE5;
    diskEntry.bandMask = lockoutSanitizeBandMask(entry.bandMask);
    diskEntry.freqMHz = entry.freqMHz;
    diskEntry.freqTolMHz = entry.freqTolMHz;
    diskEntry.confidence = entry.confidence;
    diskEntry.flags = entry.flags;
    diskEntry.directionMode = clampDirectionMode(entry.directionMode);
    diskEntry.headingTolDeg = clampHeadingTolerance(entry.headingTolDeg);
    diskEntry.missCount = entry.missCount;
    diskEntry.headingDeg = (entry.headingDeg == LockoutEntry::HEADING_INVALID)
                               ? LockoutEntry::HEADING_INVALID
                               : entry.headingDeg;
    diskEntry.firstSeenMs = entry.firstSeenMs;
    diskEntry.lastSeenMs = entry.lastSeenMs;
    diskEntry.lastPassMs = entry.lastPassMs;
    diskEntry.lastCountedMissMs = entry.lastCountedMissMs;
}

bool runtimeEntryFromDisk(const LockoutStore::LockoutDiskEntry& diskEntry, LockoutEntry& entry) {
    entry.clear();
    entry.latE5 = diskEntry.latE5;
    entry.lonE5 = diskEntry.lonE5;
    entry.radiusE5 = diskEntry.radiusE5;
    entry.bandMask = lockoutSanitizeBandMask(diskEntry.bandMask);
    if (entry.bandMask == 0) {
        return false;
    }
    entry.freqMHz = diskEntry.freqMHz;
    entry.freqTolMHz = diskEntry.freqTolMHz;
    entry.confidence = diskEntry.confidence;
    entry.flags = diskEntry.flags;
    entry.directionMode = clampDirectionMode(diskEntry.directionMode);
    entry.headingDeg = (diskEntry.headingDeg == LockoutEntry::HEADING_INVALID)
                           ? LockoutEntry::HEADING_INVALID
                           : clampHeading(diskEntry.headingDeg);
    entry.headingTolDeg = clampHeadingTolerance(diskEntry.headingTolDeg);
    entry.missCount = diskEntry.missCount;
    entry.firstSeenMs = diskEntry.firstSeenMs;
    entry.lastSeenMs = diskEntry.lastSeenMs;
    entry.lastPassMs = diskEntry.lastPassMs;
    entry.lastCountedMissMs = diskEntry.lastCountedMissMs;

    if (entry.directionMode != LockoutEntry::DIRECTION_ALL &&
        entry.headingDeg == LockoutEntry::HEADING_INVALID) {
        entry.directionMode = LockoutEntry::DIRECTION_ALL;
    }

    entry.setActive(true);
    return true;
}

size_t countActiveEntries(const LockoutIndex* index) {
    if (!index) {
        return 0;
    }

    size_t count = 0;
    for (size_t i = 0; i < index->capacity(); ++i) {
        const LockoutEntry* entry = index->at(i);
        if (!entry || !entry->isActive()) {
            continue;
        }
        if (lockoutSanitizeBandMask(entry->bandMask) == 0) {
            continue;
        }
        ++count;
    }
    return count;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; ++bit) {
            const uint32_t mask = static_cast<uint32_t>(-(static_cast<int32_t>(crc & 1u)));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc;
}

bool ensureParentDirectory(fs::FS& fs, const char* path) {
    if (!path || path[0] != '/') {
        return true;
    }

    const char* lastSlash = strrchr(path + 1, '/');
    if (!lastSlash) {
        return true;
    }

    String parent(path);
    parent = parent.substring(0, static_cast<size_t>(lastSlash - path));
    return fs.exists(parent) || fs.mkdir(parent);
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

bool LockoutStore::saveBinary(fs::FS& fs, const char* path) const {
    if (!index_ || !path || path[0] == '\0') {
        return false;
    }

    if (!ensureParentDirectory(fs, path)) {
        Serial.printf("[LockoutStore] saveBinary: failed to ensure parent dir for %s\n", path);
        return false;
    }

    const size_t entryCount = countActiveEntries(index_);
    if (entryCount > UINT16_MAX) {
        Serial.printf("[LockoutStore] saveBinary: entry count too large (%lu)\n",
                      static_cast<unsigned long>(entryCount));
        return false;
    }

    LockoutDiskHeader header = {};
    memcpy(header.magic, kBinaryMagic, sizeof(header.magic));
    header.version = kBinaryVersion;
    header.entryCount = static_cast<uint16_t>(entryCount);
    header.payloadBytes = static_cast<uint32_t>(entryCount * sizeof(LockoutDiskEntry));
    header.payloadCrc32 = 0;

    const String tmpPath = String(path) + ".tmp";
    File tmp = fs.open(tmpPath.c_str(), "w");
    if (!tmp) {
        Serial.printf("[LockoutStore] saveBinary: failed to open %s\n", tmpPath.c_str());
        return false;
    }

    if (tmp.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        tmp.close();
        fs.remove(tmpPath.c_str());
        Serial.printf("[LockoutStore] saveBinary: failed to write header to %s\n", tmpPath.c_str());
        return false;
    }

    uint32_t crc = 0xFFFFFFFFu;
    uint32_t writtenEntries = 0;

    for (size_t i = 0; i < index_->capacity(); ++i) {
        const LockoutEntry* entry = index_->at(i);
        if (!entry || !entry->isActive()) {
            continue;
        }
        if (lockoutSanitizeBandMask(entry->bandMask) == 0) {
            continue;
        }

        LockoutDiskEntry diskEntry;
        diskEntryFromRuntime(*entry, diskEntry);
        crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(&diskEntry), sizeof(diskEntry));
        if (tmp.write(reinterpret_cast<const uint8_t*>(&diskEntry), sizeof(diskEntry)) != sizeof(diskEntry)) {
            tmp.close();
            fs.remove(tmpPath.c_str());
            Serial.printf("[LockoutStore] saveBinary: failed to write entry %lu\n",
                          static_cast<unsigned long>(writtenEntries));
            return false;
        }
        ++writtenEntries;
    }

    header.payloadCrc32 = crc ^ 0xFFFFFFFFu;
    if (!tmp.seek(0) ||
        tmp.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        tmp.close();
        fs.remove(tmpPath.c_str());
        Serial.printf("[LockoutStore] saveBinary: failed to rewrite header for %s\n", tmpPath.c_str());
        return false;
    }

    tmp.flush();
    tmp.close();

    if (!StorageManager::promoteTempFileWithRollback(fs, tmpPath.c_str(), path)) {
        Serial.printf("[LockoutStore] saveBinary: promote failed %s -> %s\n",
                      tmpPath.c_str(),
                      path);
        return false;
    }

    stats_.entriesSaved = writtenEntries;
    ++stats_.saves;
    return true;
}

bool LockoutStore::loadBinary(fs::FS& fs, const char* path) {
    if (!index_) {
        Serial.println("[LockoutStore] loadBinary: no index wired");
        ++stats_.loadErrors;
        return false;
    }

    if (!path || path[0] == '\0' || !fs.exists(path)) {
        return false;
    }

    File file = fs.open(path, "r");
    if (!file) {
        Serial.printf("[LockoutStore] loadBinary: failed to open %s\n", path);
        ++stats_.loadErrors;
        return false;
    }

    const size_t fileSize = file.size();
    if (fileSize < sizeof(LockoutDiskHeader)) {
        file.close();
        Serial.printf("[LockoutStore] loadBinary: file too small (%lu bytes)\n",
                      static_cast<unsigned long>(fileSize));
        ++stats_.loadErrors;
        return false;
    }

    LockoutDiskHeader header = {};
    if (file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header)) {
        file.close();
        Serial.println("[LockoutStore] loadBinary: failed to read header");
        ++stats_.loadErrors;
        return false;
    }

    if (memcmp(header.magic, kBinaryMagic, sizeof(header.magic)) != 0) {
        file.close();
        Serial.println("[LockoutStore] loadBinary: bad magic");
        ++stats_.loadErrors;
        return false;
    }

    if (header.version != kBinaryVersion) {
        file.close();
        Serial.printf("[LockoutStore] loadBinary: unsupported version %u\n",
                      static_cast<unsigned>(header.version));
        ++stats_.loadErrors;
        return false;
    }

    if (header.entryCount > index_->capacity()) {
        file.close();
        Serial.printf("[LockoutStore] loadBinary: entry count %u exceeds capacity %u\n",
                      static_cast<unsigned>(header.entryCount),
                      static_cast<unsigned>(index_->capacity()));
        ++stats_.loadErrors;
        return false;
    }

    const uint32_t expectedPayloadBytes =
        static_cast<uint32_t>(header.entryCount) * sizeof(LockoutDiskEntry);
    if (header.payloadBytes != expectedPayloadBytes ||
        fileSize != (sizeof(LockoutDiskHeader) + static_cast<size_t>(header.payloadBytes))) {
        file.close();
        Serial.println("[LockoutStore] loadBinary: payload size mismatch");
        ++stats_.loadErrors;
        return false;
    }

    std::unique_ptr<LockoutIndex> stagedIndex(new (std::nothrow) LockoutIndex());
    if (!stagedIndex) {
        file.close();
        Serial.println("[LockoutStore] loadBinary: failed to allocate staging index");
        ++stats_.loadErrors;
        return false;
    }

    uint32_t crc = 0xFFFFFFFFu;
    uint32_t loaded = 0;
    uint32_t skipped = 0;

    for (uint16_t i = 0; i < header.entryCount; ++i) {
        LockoutDiskEntry diskEntry = {};
        if (file.read(reinterpret_cast<uint8_t*>(&diskEntry), sizeof(diskEntry)) != sizeof(diskEntry)) {
            file.close();
            Serial.printf("[LockoutStore] loadBinary: truncated while reading entry %u\n",
                          static_cast<unsigned>(i));
            ++stats_.loadErrors;
            return false;
        }

        crc = crc32Update(crc, reinterpret_cast<const uint8_t*>(&diskEntry), sizeof(diskEntry));

        LockoutEntry entry;
        if (!runtimeEntryFromDisk(diskEntry, entry)) {
            ++skipped;
            continue;
        }

        if (stagedIndex->add(entry) >= 0) {
            ++loaded;
        }
    }
    file.close();

    if ((crc ^ 0xFFFFFFFFu) != header.payloadCrc32) {
        Serial.println("[LockoutStore] loadBinary: CRC32 mismatch");
        ++stats_.loadErrors;
        return false;
    }

    index_->clear();
    for (size_t i = 0; i < stagedIndex->capacity(); ++i) {
        const LockoutEntry* entry = stagedIndex->at(i);
        if (!entry || !entry->isActive()) {
            continue;
        }
        (void)index_->add(*entry);
    }

    stats_.entriesLoaded = loaded;
    stats_.entriesSkipped = skipped;
    ++stats_.loads;

    Serial.printf("[LockoutStore] Loaded %lu entries from binary\n",
                  static_cast<unsigned long>(loaded));
    return true;
}
