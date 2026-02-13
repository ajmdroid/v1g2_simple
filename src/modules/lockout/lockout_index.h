#pragma once

#include <stddef.h>
#include <stdint.h>

#include "lockout_entry.h"

/// Fixed-size lockout zone index.
///
/// Provides O(N) scan per query where N ≤ kCapacity (~200).
/// All operations are integer-only (no floats, no haversine) to keep
/// query time bounded (~50 μs) on the Core-1 hot path.
///
/// Thread safety: designed for single-threaded access from loop().
/// If background tasks need to mutate (e.g. persistence restore at boot),
/// callers must ensure mutual exclusion externally.
class LockoutIndex {
public:
    static constexpr size_t kCapacity = 200;

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
                             uint16_t freqMHz) const;

    // --- Mutation (boot restore, learner promotion, user creation) ---

    /// Insert or overwrite at first available slot.  Returns slot index, or -1 if full.
    int add(const LockoutEntry& entry);

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

    /// Record a hit (alert matched) at the given index.
    /// Returns the new confidence value.
    uint8_t recordHit(size_t index, int64_t epochMs);

    /// Find the first active entry whose zone contains the given position
    /// and matches band+freq.  Returns index, or -1 if none.
    int findMatch(int32_t latE5,
                  int32_t lonE5,
                  uint8_t band,
                  uint16_t freqMHz) const;

private:
    /// Fast integer-only proximity check.
    /// Returns true if (latE5, lonE5) is within the entry's bounding box AND
    /// the squared E5 distance is within radius^2.
    static bool withinRadius(int32_t latE5,
                             int32_t lonE5,
                             const LockoutEntry& entry);

    /// Check whether the alert frequency is within entry's tolerance window.
    static bool freqMatches(uint16_t alertFreqMHz, const LockoutEntry& entry);

    LockoutEntry entries_[kCapacity] = {};
};
