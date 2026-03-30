#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lockout_entry.h"

struct LockoutCleanPassResult {
    uint8_t confidence = 0; // Current confidence after mutation
    bool counted = false;   // True if this pass counted toward policy threshold
    bool demoted = false;   // True if the entry was removed
};

/// Fixed-size lockout zone index.
///
/// Provides O(N) scan per query where N ≤ kCapacity (~500).
/// Proximity checks use integer bounding boxes with a cos(lat)-corrected
/// squared-distance test to produce circular zones in real-world space.
///
/// Thread safety: designed for single-threaded access from loop().
/// If background tasks need to mutate (e.g. persistence restore at boot),
/// callers must ensure mutual exclusion externally.
class LockoutIndex {
public:
    static constexpr size_t kCapacity = 500;

    /// Reset all slots to inactive.
    void clear();

    /// Query: should this alert be muted?
    /// @param latE5   Current latitude  (degrees * 100000)
    /// @param lonE5   Current longitude (degrees * 100000)
    /// @param band    Alert band bitmask (matches Band enum values)
    /// @param freqMHz Alert frequency   (MHz)
    LockoutDecision evaluate(int32_t latE5,
                             int32_t lonE5,
                             uint8_t band,
                             uint16_t freqMHz,
                             bool courseValid = false,
                             float courseDeg = 0.0f) const;

    // --- Mutation (boot restore, learner promotion, user creation) ---

    /// Insert at first available slot.  If full, evicts the lowest-priority
    /// learned (non-manual) entry.  Returns slot index, or -1 if all slots
    /// are occupied by manual entries (un-evictable).
    int add(const LockoutEntry& entry);

    /// Insert, or update an existing entry if one already covers the same zone.
    /// Uses findMatch() to detect overlap.  On match: merges confidence
    /// (max of old and new), updates timestamps if newer.  Returns slot index,
    /// or -1 if full and no match found.
    int addOrUpdate(const LockoutEntry& entry);

    /// Remove entry at index (marks inactive).  Returns false if index out of range.
    bool remove(size_t index);

    /// Direct slot access (bounds-checked).  Returns nullptr if out of range.
    const LockoutEntry* at(size_t index) const;
    LockoutEntry* mutableAt(size_t index);

    /// Number of active entries.
    size_t activeCount() const;

    /// Total capacity.
    size_t capacity() const { return kCapacity; }

    // --- Helpers for learner / demotion ---

    /// Record a clean pass through the zone at the given index.
    /// Increments the internal miss counter state in the entry's confidence field.
    /// Returns the new confidence value (0 = fully demoted).
    uint8_t recordCleanPass(size_t index, int64_t epochMs);

    /// Record a clean pass with optional interval/count policy.
    /// - missThreshold == 0 => legacy behavior (recordCleanPass).
    /// - missThreshold > 0  => counted miss streak with optional interval gate.
    ///   The entry demotes when missCount reaches missThreshold.
    LockoutCleanPassResult recordCleanPassWithPolicy(size_t index,
                                                     int64_t epochMs,
                                                     uint8_t localHour,
                                                     uint32_t missIntervalMs,
                                                     uint8_t missThreshold);
    LockoutCleanPassResult recordCleanPassWithPolicy(size_t index,
                                                     int64_t epochMs,
                                                     uint32_t missIntervalMs,
                                                     uint8_t missThreshold) {
        return recordCleanPassWithPolicy(index, epochMs, 0xFF, missIntervalMs, missThreshold);
    }

    /// Record a hit (alert matched) at the given index.
    /// Returns the new confidence value.
    uint8_t recordHit(size_t index, int64_t epochMs, uint16_t observedFreqMHz, uint8_t localHour);
    uint8_t recordHit(size_t index, int64_t epochMs) {
        return recordHit(index, epochMs, 0, 0xFF);
    }

    /// Find the first active entry whose zone contains the given position
    /// and matches band+freq.  Returns index, or -1 if none.
    int findMatch(int32_t latE5,
                  int32_t lonE5,
                  uint8_t band,
                  uint16_t freqMHz,
                  bool courseValid = false,
                  float courseDeg = 0.0f) const;

    /// Find all active entries whose zone contains the given position
    /// (ignores band/freq — position-only).  Fills `out` with slot indices.
    /// Returns number of entries found (≤ outCap).
    size_t findNearby(int32_t latE5,
                      int32_t lonE5,
                      int16_t* out,
                      size_t outCap) const;

    /// Like findNearby(), but inflates each zone's radius by bufferE5 units.
    /// Used by pre-quiet to detect approaching a lockout zone before entry.
    size_t findNearbyInflated(int32_t latE5,
                              int32_t lonE5,
                              uint16_t bufferE5,
                              int16_t* out,
                              size_t outCap) const;

    /// Direction-aware pre-quiet proximity check.
    ///
    /// For DIRECTION_ALL zones: symmetric circle + full buffer (same as
    /// findNearbyInflated).  For DIRECTION_FORWARD zones with a known heading:
    ///   - If course matches zone heading AND device is upstream (approaching):
    ///     use full buffer → triggers volume drop early.
    ///   - If device is downstream (past the zone center): no buffer →
    ///     the zone's base radius controls exit, releasing volume sooner.
    ///   - If course doesn't match: no buffer (wrong direction of travel).
    ///
    /// When courseValid is false, falls back to findNearbyInflated() behavior.
    size_t findNearbyDirectional(int32_t latE5,
                                 int32_t lonE5,
                                 bool courseValid,
                                 float courseDeg,
                                 uint16_t bufferE5,
                                 int16_t* out,
                                 size_t outCap) const;

private:
    int findEquivalentSignature(const LockoutEntry& entry) const;
    uint16_t nextAreaId() const;

    /// Fast proximity check with cos(lat) correction.
    /// Returns true if (latE5, lonE5) is within the entry's bounding box AND
    /// the cos(lat)-corrected squared distance is within radius^2.
    static bool withinRadius(int32_t latE5,
                             int32_t lonE5,
                             const LockoutEntry& entry);

    /// Like withinRadius() but with an extra buffer added to the entry's radius.
    static bool withinInflatedRadius(int32_t latE5,
                                     int32_t lonE5,
                                     const LockoutEntry& entry,
                                     uint16_t bufferE5);

    /// Check whether the alert frequency is within entry's tolerance window.
    static bool freqMatches(uint16_t alertFreqMHz, const LockoutEntry& entry);

    /// Direction gate for forward/reverse entries.
    static bool courseMatches(bool courseValid, float courseDeg, const LockoutEntry& entry);

    /// Find the lowest-priority evictable entry (learned, non-manual).
    /// Among candidates, selects lowest confidence; breaks ties by oldest
    /// lastSeenMs.  Returns slot index, or -1 if no evictable candidate.
    int findEvictionCandidate() const;

    LockoutEntry entries_[kCapacity] = {};
};
