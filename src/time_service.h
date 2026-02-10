/**
 * Safe clock service for monotonic + optional epoch time.
 *
 * Design goals:
 * - Always-available monotonic time (millis)
 * - Optional epoch time based on a trusted base offset
 * - No background sync, no blocking, no dynamic allocation
 */

#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <Arduino.h>
#include <atomic>
#include <cstdint>

class TimeService {
public:
    enum Source : uint8_t {
        SOURCE_NONE = 0,
        SOURCE_CLIENT_AP = 1,
        SOURCE_GPS = 2,
        SOURCE_SNTP_STA = 3,
        SOURCE_RTC = 4
    };

    uint32_t nowMonoMs() const { return millis(); }
    bool timeValid() const { return valid_.load(std::memory_order_acquire) != 0; }
    uint8_t timeSource() const { return source_.load(std::memory_order_relaxed); }
    int32_t tzOffsetMinutes() const { return tzOffsetMinutes_.load(std::memory_order_relaxed); }

    // Returns 0 when epoch is not valid.
    int64_t nowEpochMsOr0() const;
    uint32_t epochAgeMsOr0() const;

    // Sets epoch base from a trusted unix epoch (milliseconds).
    void setEpochBaseMs(int64_t trustedEpochMs, int32_t tzOffsetMinutes, Source source);
    void clear();

private:
    std::atomic<uint8_t> valid_{0};
    std::atomic<uint8_t> source_{SOURCE_NONE};
    std::atomic<int32_t> tzOffsetMinutes_{0};
    std::atomic<int64_t> epochBaseMs_{0};   // epoch_ms - mono_ms_at_set
    std::atomic<uint32_t> setMonoMs_{0};    // millis() when epoch base was set
};

extern TimeService timeService;

#endif  // TIME_SERVICE_H
