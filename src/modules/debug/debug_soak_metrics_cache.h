#pragma once

#include <ArduinoJson.h>
#include <WebServer.h>

#include <cstddef>
#include <cstdint>
namespace DebugApiService {

struct SoakMetricsJsonCache {
    char* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool inPsram = false;
    uint32_t lastBuildMs = 0;
    bool valid = false;
};

bool sendCachedSoakMetrics(WebServer& server,
                           SoakMetricsJsonCache& cache,
                           uint32_t cacheTtlMs,
                           void (*buildMetrics)(JsonDocument&, void*), void* buildMetricsCtx,
                           uint32_t (*millisFn)(void*) = nullptr, void* millisCtx = nullptr);

void invalidateSoakMetricsCache(SoakMetricsJsonCache& cache);

void releaseSoakMetricsCache(SoakMetricsJsonCache& cache);

}  // namespace DebugApiService
