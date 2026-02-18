#pragma once

#include <stdint.h>

/// Compact lockout zone stored in a flat static array.
/// All coordinates are fixed-point E5 (degrees * 100000).
/// Timestamps are Unix epoch milliseconds (int64_t) from TimeService.
struct LockoutEntry {
    enum DirectionMode : uint8_t {
        DIRECTION_ALL = 0,      // Match regardless of travel direction.
        DIRECTION_FORWARD = 1,  // Match when course aligns with headingDeg.
        DIRECTION_REVERSE = 2   // Match when course is opposite headingDeg.
    };

    static constexpr uint16_t HEADING_INVALID = 0xFFFF;

    int32_t  latE5       = 0;       // Center latitude  (E5 fixed-point)
    int32_t  lonE5       = 0;       // Center longitude (E5 fixed-point)
    uint16_t radiusE5    = 0;       // Radius in E5 units (~135 ≈ 150 m lat)
    uint8_t  bandMask    = 0;       // Which bands to mute (bitmask, matches Band enum)
    uint16_t freqMHz     = 0;       // Center frequency (MHz)
    uint16_t freqTolMHz  = 10;      // ±tolerance (MHz) for frequency match
    uint8_t  confidence  = 0;       // 0-255: decays on clean pass, grows on hit
    uint8_t  flags       = 0;       // See Flag constants below
    uint8_t  directionMode = DIRECTION_ALL; // Optional directional gate (all/forward/reverse)
    uint8_t  headingTolDeg = 45;    // Allowed angular delta for directional matching
    uint8_t  missCount   = 0;       // Counted clean passes since most recent hit (policy path)
    uint16_t headingDeg  = HEADING_INVALID; // Heading reference for directional matching
    int64_t  firstSeenMs = 0;       // Unix epoch ms — first observation
    int64_t  lastSeenMs  = 0;       // Unix epoch ms — most recent hit
    int64_t  lastPassMs  = 0;       // Unix epoch ms — most recent clean pass
    int64_t  lastCountedMissMs = 0; // Unix epoch ms — most recent missCount increment

    // --- Flag bit definitions ---
    static constexpr uint8_t FLAG_ACTIVE   = 1 << 0;  // Slot is in use
    static constexpr uint8_t FLAG_MANUAL   = 1 << 1;  // User-created (auto-demotion optional)
    static constexpr uint8_t FLAG_LEARNED  = 1 << 2;  // Auto-promoted from learner

    bool isActive()  const { return (flags & FLAG_ACTIVE)  != 0; }
    bool isManual()  const { return (flags & FLAG_MANUAL)  != 0; }
    bool isLearned() const { return (flags & FLAG_LEARNED) != 0; }
    bool hasDirectionConstraint() const { return directionMode != DIRECTION_ALL; }

    void setActive(bool v)  { if (v) flags |= FLAG_ACTIVE;  else flags &= ~FLAG_ACTIVE; }
    void setManual(bool v)  { if (v) flags |= FLAG_MANUAL;  else flags &= ~FLAG_MANUAL; }
    void setLearned(bool v) { if (v) flags |= FLAG_LEARNED; else flags &= ~FLAG_LEARNED; }

    void clear() { *this = LockoutEntry{}; }
};

/// Result of a lockout query for a single alert.
struct LockoutDecision {
    bool     shouldMute = false;    // True if an active lockout zone covers this alert
    int16_t  matchIndex = -1;       // Index into LockoutIndex array (-1 = no match)
    uint8_t  confidence = 0;        // Confidence of the matching entry (0 if no match)
};
