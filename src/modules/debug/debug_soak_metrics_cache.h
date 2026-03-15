#pragma once

#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace DebugApiService {

struct SoakMetricsJsonCache {
    char* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool inPsram = false;
    uint32_t lastBuildMs = 0;
    bool valid = false;
};

using SoakMetricsBuildFn = std::function<void(JsonDocument&)>;

bool sendCachedSoakMetrics(WebServer& server,
                           SoakMetricsJsonCache& cache,
                           uint32_t cacheTtlMs,
                           const SoakMetricsBuildFn& buildMetrics,
                           const std::function<uint32_t()>& millisFn = nullptr);

void invalidateSoakMetricsCache(SoakMetricsJsonCache& cache);

void releaseSoakMetricsCache(SoakMetricsJsonCache& cache);

}  // namespace DebugApiService
