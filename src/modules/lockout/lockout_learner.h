#pragma once

#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>

class LockoutIndex;
class SignalObservationLog;
struct SignalObservation;

/// A proto-lockout zone accumulating evidence from signal observations.
/// When enough hits are recorded, it is promoted to a full LockoutEntry.
struct LearnerCandidate {
    int32_t  latE5      = 0;     // Center latitude (E5 fixed-point)
    int32_t  lonE5      = 0;     // Center longitude (E5 fixed-point)
    uint8_t  band       = 0;     // Band enum value (single, not a mask)
    uint16_t freqMHz    = 0;     // Center frequency (MHz)
    uint8_t  hitCount   = 0;     // Number of matching observations
    int64_t  firstSeenMs = 0;    // Epoch ms — first observation
    int64_t  lastSeenMs  = 0;    // Epoch ms — most recent observation
    int64_t  lastCountedHitMs = 0; // Epoch ms — most recent hitCount increment
    bool     active     = false; // Slot in use
};

/// Consumes signal observations from the observation log, clusters them
/// by location+band+frequency, and promotes persistent clusters to
/// LockoutEntry slots in the LockoutIndex.
///
/// Priority: Tier 7 (logging/persistence). Rate-limited to ~every 2s.
/// Thread safety: single-threaded from loop().
class LockoutLearner {
public:
    static constexpr size_t   kCandidateCapacity  = 64;
    static constexpr uint8_t  kDefaultPromotionHits      = 3;
    static constexpr uint8_t  kMinPromotionHits          = 2;
    static constexpr uint8_t  kMaxPromotionHits          = 6;
    static constexpr uint16_t kDefaultRadiusE5           = 135;    // ~150m lat / ~492ft
    static constexpr uint16_t kMinRadiusE5               = 45;     // ~50m / ~164ft
    static constexpr uint16_t kMaxRadiusE5               = 360;    // ~400m / ~1312ft
    static constexpr uint16_t kDefaultFreqToleranceMHz   = 10;
    static constexpr uint16_t kMinFreqToleranceMHz       = 2;
    static constexpr uint16_t kMaxFreqToleranceMHz       = 20;
    static constexpr uint8_t  kDefaultLearnIntervalHours = 0;  // 0 = disabled
    static constexpr uint32_t kPollIntervalMs     = 2000;
    static constexpr int64_t  kStaleDurationMs    = 7LL * 24 * 3600 * 1000; // 7 days
    static constexpr uint32_t kPruneIntervalMs    = 60000;  // Prune every 60s
    static constexpr size_t   kBatchSize          = 32;     // Max observations per poll
    static constexpr const char* kPersistTypeTag = "v1simple_lockout_pending";
    static constexpr uint8_t kPersistVersion = 1;

    /// Wire dependencies. Must be called once before process().
    void begin(LockoutIndex* index, SignalObservationLog* log);

    // Runtime tuning (persisted in SettingsManager and applied at boot/runtime).
    void setTuning(uint8_t promotionHits,
                   uint16_t radiusE5,
                   uint16_t freqToleranceMHz,
                   uint8_t learnIntervalHours = kDefaultLearnIntervalHours);
    uint8_t promotionHits() const { return promotionHits_; }
    uint16_t radiusE5() const { return radiusE5_; }
    uint16_t freqToleranceMHz() const { return freqToleranceMHz_; }
    uint8_t learnIntervalHours() const { return learnIntervalHours_; }

    /// Ingest new observations and manage candidates.
    /// Rate-limited internally; safe to call every loop().
    void process(uint32_t nowMs, int64_t epochMs);

    /// Read-only access to candidate table.
    const LearnerCandidate* candidateAt(size_t index) const;
    size_t activeCandidateCount() const;

    // --- Persistence (best-effort Tier 7) ---
    void toJson(JsonDocument& doc) const;
    bool fromJson(JsonDocument& doc, int64_t epochMs);
    bool isDirty() const { return dirty_; }
    void clearDirty() { dirty_ = false; }

    struct Stats {
        uint32_t observed          = 0; // Observations ingested
        uint32_t candidatesCreated = 0; // New candidates started
        uint32_t promotions        = 0; // Promoted to LockoutIndex
        uint32_t promotionsFailed  = 0; // Promotion attempted but index full
        uint32_t pruned            = 0; // Stale candidates removed
        uint32_t skippedNoLocation = 0; // Observations without valid GPS
        uint32_t skippedBand       = 0; // Observations outside lockout band policy
        uint32_t skippedInIndex    = 0; // Already covered by existing lockout
    };
    const Stats& stats() const { return stats_; }

private:
    int findCandidate(int32_t latE5, int32_t lonE5, uint8_t band, uint16_t freqMHz) const;
    int allocCandidate();
    void promoteCandidate(size_t idx, int64_t epochMs);
    void pruneStale(int64_t epochMs);

    /// Integer-only proximity check (same algorithm as LockoutIndex).
    static bool withinRadius(int32_t latE5, int32_t lonE5,
                             int32_t centerLatE5, int32_t centerLonE5,
                             uint16_t radiusE5);
    static bool freqClose(uint16_t freqA, uint16_t freqB, uint16_t tolerance);

    LockoutIndex*         index_ = nullptr;
    SignalObservationLog* log_   = nullptr;

    LearnerCandidate candidates_[kCandidateCapacity] = {};
    uint32_t lastProcessedPublished_ = 0;
    uint32_t lastPollMs_             = 0;
    uint32_t lastPruneMs_            = 0;
    uint32_t lastLogMs_              = 0;
    Stats stats_;
    uint8_t promotionHits_ = kDefaultPromotionHits;
    uint16_t radiusE5_ = kDefaultRadiusE5;
    uint16_t freqToleranceMHz_ = kDefaultFreqToleranceMHz;
    uint8_t learnIntervalHours_ = kDefaultLearnIntervalHours;
    int64_t learnHitIntervalMs_ = 0;
    bool dirty_ = false;

    static constexpr uint32_t LOG_INTERVAL_MS = 10000;
};

extern LockoutLearner lockoutLearner;
