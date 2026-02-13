#pragma once

#include <stdint.h>

#include "lockout_entry.h"
#include "lockout_index.h"

class SettingsManager;
struct GpsRuntimeStatus;
class PacketParser;

/// Per-frame summary emitted by the enforcer for diagnostics / display.
struct LockoutEnforcerResult {
    bool     evaluated   = false;  // True if evaluation ran (mode >= SHADOW, GPS valid)
    bool     shouldMute  = false;  // True if a lockout zone matched the priority alert
    int16_t  matchIndex  = -1;     // Slot index in LockoutIndex (-1 = no match)
    uint8_t  confidence  = 0;      // Confidence of the matched entry
    uint8_t  mode        = 0;      // LockoutRuntimeMode active during evaluation
};

/// Evaluates incoming alerts against the LockoutIndex and decides
/// whether to mute, advise, or just log.
///
/// This module is intentionally side-effect-free in SHADOW and ADVISORY
/// modes — it only produces a decision.  The caller (main loop) is
/// responsible for acting on it (e.g. sending mute commands, updating
/// display indicators).
///
/// Thread safety: designed for single-threaded access from loop().
class LockoutEnforcer {
public:
    /// Wire dependencies.  Must be called once before process().
    /// All pointers must remain valid for the lifetime of the enforcer.
    void begin(const SettingsManager* settings,
               LockoutIndex* index);

    /// Evaluate the current priority alert against the lockout index.
    /// Called once per parsed BLE frame from the main loop.
    ///
    /// @param nowMs       Current millis() timestamp
    /// @param epochMs     Current Unix epoch ms (0 = time not yet valid)
    /// @param parser      Packet parser with current alert state
    /// @param gpsStatus   Current GPS snapshot
    /// @return            Decision result (also cached in lastResult())
    LockoutEnforcerResult process(uint32_t nowMs,
                                  int64_t epochMs,
                                  const PacketParser& parser,
                                  const GpsRuntimeStatus& gpsStatus);

    /// Most recent evaluation result (valid after process() returns).
    const LockoutEnforcerResult& lastResult() const { return lastResult_; }

    /// Cumulative counters for diagnostics.
    struct Stats {
        uint32_t evaluations  = 0;  // Total process() calls that ran evaluation
        uint32_t matches      = 0;  // Times a lockout zone matched an alert
        uint32_t skippedOff   = 0;  // Skipped because mode == OFF
        uint32_t skippedNoGps = 0;  // Skipped because GPS position not valid
        uint32_t skippedNoFix = 0;  // Skipped because GPS has no fix
    };
    const Stats& stats() const { return stats_; }

private:
    const SettingsManager* settings_ = nullptr;
    LockoutIndex* index_             = nullptr;
    LockoutEnforcerResult lastResult_;
    Stats stats_;

    // Rate-limited serial logging to avoid flooding.
    uint32_t lastLogMs_ = 0;
    static constexpr uint32_t LOG_INTERVAL_MS = 5000;
};

extern LockoutEnforcer lockoutEnforcer;
