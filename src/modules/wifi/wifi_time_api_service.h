#pragma once

#include <WebServer.h>

#include <cstdint>
#include <functional>

namespace WifiTimeApiService {

struct TimeRuntime {
    std::function<bool()> timeValid;
    std::function<int64_t()> nowEpochMsOr0;
    std::function<int32_t()> tzOffsetMinutes;
    std::function<uint8_t()> timeSource;
    std::function<void(int64_t, int32_t, uint8_t)> setEpochBaseMs;
    std::function<uint8_t()> timeConfidence;
    std::function<uint32_t()> nowMonoMs;
    std::function<uint32_t()> epochAgeMsOr0;
};

void handleApiTimeSet(WebServer& server,
                      const TimeRuntime& runtime,
                      uint8_t clientSource,
                      const std::function<void()>& invalidateStatusCache,
                      const std::function<bool()>& checkRateLimit);

}  // namespace WifiTimeApiService
