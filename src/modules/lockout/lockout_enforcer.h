#pragma once

#include <stdint.h>

#include "lockout_entry.h"
#include "lockout_index.h"

class SettingsManager;
class LockoutStore;
struct GpsRuntimeStatus;
class PacketParser;

/// Per-frame summary emitted by the enforcer for diagnostics / display.
struct LockoutEnforcerResult {
    bool     evaluated   = false;  // True if evaluation ran (mode >= SHADOW, GPS valid)
    bool     shouldMute  = false;  // True if all active lockout-eligible alerts were matched
    int16_t  matchIndex  = -1;     // First matched slot in V1 alert order (-1 = no matched alerts)
    uint8_t  confidence  = 0;      // Confidence of the first matched entry
    uint8_t  mode        = 0;      // LockoutRuntimeMode active during evaluation
    uint8_t  supportedAlertCount = 0; // Eligible V1 alerts considered for lockout this frame
    uint8_t  matchedAlertCount = 0;   // Eligible V1 alerts matched by active lockout entries
};

/// Evaluates incoming alerts against the LockoutIndex and decides
/// whether to mute, advise, or just log.
///
/// SHADOW and ADVISORY modes are read-only — they produce a decision
/// but never mutate the LockoutIndex (no recordHit).  Only ENFORCE
/// mode updates confidence and timestamps on matched entries, records
/// clean passes for nearby unmatched entries, and marks the store dirty.
/// The caller (main loop) is responsible for acting on the decision
/// (e.g. sending mute commands, updating display indicators).
///
/// Thread safety: designed for single-threaded access from loop().
class LockoutEnforcer {
public:
    static constexpr size_t kMaxNearbySlots = 128;
    static constexpr size_t kMaxCandidateSlots = 64;
    static constexpr size_t kMaxSupportedAlerts = 15;
    static constexpr size_t kMaxMatchedSlots = 16;

    struct LiveAlertMatch {
        uint8_t band = 0;
        uint16_t freqMHz = 0;
        int16_t candidates[kMaxCandidateSlots] = {};
        size_t candidateCount = 0;
        int16_t assignedSlot = -1;
    };

    /// Wire dependencies.  Must be called once before process().
    /// All pointers must remain valid for the lifetime of the enforcer.
    /// @param store  Optional — if non-null, markDirty() called on index mutation.
    void begin(const SettingsManager* settings,
               LockoutIndex* index,
               LockoutStore* store = nullptr);

    /// Evaluate the current V1 alert set against the lockout index.
    /// Called once per parsed BLE frame from the main loop.
    ///
    /// @param nowMs       Current millis() timestamp
    /// @param epochMs     Current Unix epoch ms (0 = time not yet valid)
    /// @param parser      Packet parser with current alert state
    /// @param gpsStatus   Current GPS snapshot
    /// @return            Decision result (also cached in lastResult())
    LockoutEnforcerResult process(uint32_t nowMs,
                                  int64_t epochMs,
                                  int32_t tzOffsetMinutes,
                                  const PacketParser& parser,
                                  const GpsRuntimeStatus& gpsStatus);
    LockoutEnforcerResult process(uint32_t nowMs,
                                  int64_t epochMs,
                                  const PacketParser& parser,
                                  const GpsRuntimeStatus& gpsStatus) {
        return process(nowMs, epochMs, 0, parser, gpsStatus);
    }

    /// Most recent evaluation result (valid after process() returns).
    const LockoutEnforcerResult& lastResult() const { return lastResult_; }

    /// Cumulative counters for diagnostics.
    struct Stats {
        uint32_t evaluations   = 0;  // Total process() calls that ran evaluation
        uint32_t matches       = 0;  // Total eligible alerts matched by lockout zones
        uint32_t cleanPasses   = 0;  // Counted clean-pass updates applied
        uint32_t demotions     = 0;  // Entries auto-removed by clean-pass decay
        uint32_t skippedOff    = 0;  // Skipped because mode == OFF
        uint32_t skippedNoGps  = 0;  // Skipped because GPS position not valid
        uint32_t skippedNoFix  = 0;  // Skipped because GPS has no fix
        uint32_t skippedLowSats  = 0;  // Skipped because satellite count < minimum
        uint32_t skippedHighHdop = 0;  // Skipped because HDOP exceeded threshold
    };
    const Stats& stats() const { return stats_; }

private:
    void recordCleanPasses(int32_t latE5, int32_t lonE5,
                           int16_t matchedSlot, int64_t epochMs, uint8_t localHour);

    const SettingsManager* settings_ = nullptr;
    LockoutIndex* index_             = nullptr;
    LockoutStore* store_             = nullptr;
    LockoutEnforcerResult lastResult_;
    Stats stats_;

    // Rate-limited serial logging to avoid flooding.
    uint32_t lastLogMs_ = 0;
    static constexpr uint32_t LOG_INTERVAL_MS = 5000;

    // Rate-limited clean-pass recording (one pass per zone per drive-through).
    int64_t lastCleanPassEpochMs_ = 0;
    static constexpr uint32_t CLEAN_PASS_INTERVAL_MS = 30000;

    // --- Reusable scratch buffers (moved off stack to reduce per-call frame) ---
    int16_t nearbySlots_[kMaxNearbySlots] = {};
    LiveAlertMatch liveAlerts_[kMaxSupportedAlerts] = {};
    int16_t slotToAlert_[LockoutIndex::kCapacity] = {};
    bool visitedSlots_[LockoutIndex::kCapacity] = {};
    int16_t matchedSlots_[kMaxMatchedSlots] = {};
};
