/**
 * Safe clock service for monotonic + optional epoch time.
 *
 * Design goals:
 * - Always-available monotonic time (millis)
 * - Optional epoch time based on a trusted base offset
 * - No background sync, no blocking, no dynamic allocation
 */

#pragma once

#include <Arduino.h>
#include <atomic>
#include <cstdint>

class TimeService {
public:
    enum Source : uint8_t {
        SOURCE_NONE = 0,
        SOURCE_CLIENT_AP = 1,
        SOURCE_SNTP_STA = 3,
        SOURCE_RTC = 4
    };

    enum Confidence : uint8_t {
        CONFIDENCE_NONE = 0,
        CONFIDENCE_ESTIMATED = 1,
        CONFIDENCE_ACCURATE = 2
    };

    // Initialize from persisted/system time once at boot (idempotent).
    void begin();

    uint32_t nowMonoMs() const { return millis(); }
    bool timeValid() const { return valid_.load(std::memory_order_acquire) != 0; }
    uint8_t timeSource() const { return source_.load(std::memory_order_relaxed); }
    uint8_t timeConfidence() const { return confidence_.load(std::memory_order_relaxed); }
    int32_t tzOffsetMinutes() const { return tzOffsetMinutes_.load(std::memory_order_relaxed); }

    // Returns 0 when epoch is not valid.
    int64_t nowEpochMsOr0() const;
    uint32_t epochAgeMsOr0() const;

    // Sets epoch base from a trusted unix epoch (milliseconds).
    void setEpochBaseMs(int64_t trustedEpochMs, int32_t tzOffsetMinutes, Source source);
    void clear();

    // Persist current epoch to NVS (call before deep sleep / shutdown).
    void persistCurrentTime();

    // Periodic NVS save — call from main loop. Saves every ~5 minutes.
    void periodicSave(uint32_t nowMs);

private:
    std::atomic<uint8_t> valid_{0};
    std::atomic<uint8_t> source_{SOURCE_NONE};
    std::atomic<uint8_t> confidence_{CONFIDENCE_NONE};
    std::atomic<int32_t> tzOffsetMinutes_{0};
    std::atomic<int64_t> epochBaseMs_{0};   // epoch_ms - mono_ms_at_set
    std::atomic<uint32_t> setMonoMs_{0};    // millis() when epoch base was set
    std::atomic<uint8_t> initialized_{0};
    uint32_t lastPeriodicSaveMs_{0};           // millis() of last periodic NVS save
    static constexpr uint32_t PERIODIC_SAVE_INTERVAL_MS = 5UL * 60UL * 1000UL;  // 5 minutes
};

extern TimeService timeService;

