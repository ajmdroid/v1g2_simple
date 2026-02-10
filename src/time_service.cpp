#include "time_service.h"

TimeService timeService;

int64_t TimeService::nowEpochMsOr0() const {
    if (valid_.load(std::memory_order_acquire) == 0) {
        return 0;
    }

    const int64_t base = epochBaseMs_.load(std::memory_order_relaxed);
    const int64_t mono = static_cast<int64_t>(nowMonoMs());
    const int64_t epoch = base + mono;
    if (epoch < 0) {
        return 0;
    }
    return epoch;
}

uint32_t TimeService::epochAgeMsOr0() const {
    if (valid_.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    const uint32_t now = nowMonoMs();
    const uint32_t setMono = setMonoMs_.load(std::memory_order_relaxed);
    return now - setMono;
}

void TimeService::setEpochBaseMs(int64_t trustedEpochMs, int32_t tzOffsetMinutes, Source source) {
    const uint32_t monoNow = nowMonoMs();
    const int64_t base = trustedEpochMs - static_cast<int64_t>(monoNow);

    epochBaseMs_.store(base, std::memory_order_relaxed);
    setMonoMs_.store(monoNow, std::memory_order_relaxed);
    tzOffsetMinutes_.store(tzOffsetMinutes, std::memory_order_relaxed);
    source_.store(static_cast<uint8_t>(source), std::memory_order_relaxed);
    valid_.store(1, std::memory_order_release);
}

void TimeService::clear() {
    valid_.store(0, std::memory_order_release);
    source_.store(SOURCE_NONE, std::memory_order_relaxed);
    tzOffsetMinutes_.store(0, std::memory_order_relaxed);
    epochBaseMs_.store(0, std::memory_order_relaxed);
    setMonoMs_.store(0, std::memory_order_relaxed);
}
