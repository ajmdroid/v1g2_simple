#include "debug_soak_metrics_cache.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "json_stream_response.h"

namespace DebugApiService {

namespace {

constexpr size_t SOAK_CACHE_GROWTH_QUANTUM = 256u;

size_t roundUpSoakCacheCapacity(size_t required) {
    return ((required + SOAK_CACHE_GROWTH_QUANTUM - 1u) / SOAK_CACHE_GROWTH_QUANTUM) *
           SOAK_CACHE_GROWTH_QUANTUM;
}

bool hasFreshSoakMetricsCache(const SoakMetricsJsonCache& cache,
                              uint32_t nowMs,
                              uint32_t cacheTtlMs) {
    if (!cache.valid || cache.data == nullptr || cache.length == 0) {
        return false;
    }
    if (cacheTtlMs == 0) {
        return false;
    }
    return static_cast<uint32_t>(nowMs - cache.lastBuildMs) < cacheTtlMs;
}

bool allocateSoakCacheBuffer(size_t required,
                             char*& newData,
                             size_t& newCapacity,
                             bool& inPsram) {
    newCapacity = roundUpSoakCacheCapacity(required);
    newData = static_cast<char*>(
        heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
    inPsram = true;

    if (newData == nullptr) {
        Serial.printf(
            "[DebugApi] Soak cache PSRAM alloc failed; falling back to internal (%lu bytes)\n",
            static_cast<unsigned long>(newCapacity));
        newData = static_cast<char*>(
            heap_caps_malloc(newCapacity, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        inPsram = false;
    }

    if (newData == nullptr) {
        Serial.printf("[DebugApi] Soak cache alloc failed (%lu bytes); streaming uncached response\n",
                      static_cast<unsigned long>(newCapacity));
        return false;
    }

    return true;
}

}  // namespace

bool sendCachedSoakMetrics(WebServer& server,
                           SoakMetricsJsonCache& cache,
                           uint32_t cacheTtlMs,
                           const SoakMetricsBuildFn& buildMetrics,
                           const std::function<uint32_t()>& millisFn) {
    const uint32_t nowMs = millisFn ? millisFn() : static_cast<uint32_t>(millis());
    if (hasFreshSoakMetricsCache(cache, nowMs, cacheTtlMs)) {
        sendSerializedJson(server, cache.data, cache.length);
        return true;
    }

    JsonDocument doc;
    if (buildMetrics) {
        buildMetrics(doc);
    }

    const size_t required = measureJson(doc) + 1u;
    char* targetData = cache.data;
    size_t targetCapacity = cache.capacity;
    bool targetInPsram = cache.inPsram;
    bool usingNewAllocation = false;

    if (targetData == nullptr || targetCapacity < required) {
        char* newData = nullptr;
        size_t newCapacity = 0;
        bool newInPsram = false;
        if (!allocateSoakCacheBuffer(required, newData, newCapacity, newInPsram)) {
            sendJsonStream(server, doc);
            return false;
        }

        targetData = newData;
        targetCapacity = newCapacity;
        targetInPsram = newInPsram;
        usingNewAllocation = true;

        Serial.printf("[DebugApi] Soak cache grow %lu -> %lu bytes (%s)\n",
                      static_cast<unsigned long>(cache.capacity),
                      static_cast<unsigned long>(targetCapacity),
                      targetInPsram ? "psram" : "internal");
    }

    const size_t length = serializeJson(doc, targetData, targetCapacity);
    if (length == 0 || length >= targetCapacity) {
        if (usingNewAllocation) {
            heap_caps_free(targetData);
        }
        sendJsonStream(server, doc);
        return false;
    }
    targetData[length] = '\0';

    if (usingNewAllocation) {
        if (cache.data != nullptr) {
            heap_caps_free(cache.data);
        }
        cache.data = targetData;
        cache.capacity = targetCapacity;
        cache.inPsram = targetInPsram;
    }

    cache.length = length;
    cache.lastBuildMs = nowMs;
    cache.valid = true;

    sendSerializedJson(server, cache.data, cache.length);
    return true;
}

void invalidateSoakMetricsCache(SoakMetricsJsonCache& cache) {
    cache.length = 0;
    cache.lastBuildMs = 0;
    cache.valid = false;
}

void releaseSoakMetricsCache(SoakMetricsJsonCache& cache) {
    if (cache.data != nullptr) {
        heap_caps_free(cache.data);
    }

    cache.data = nullptr;
    cache.capacity = 0;
    cache.length = 0;
    cache.inPsram = false;
    cache.lastBuildMs = 0;
    cache.valid = false;
}

}  // namespace DebugApiService
