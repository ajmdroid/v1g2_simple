#include "loop_telemetry_module.h"

void LoopTelemetryModule::begin(const Providers& hooks) {
    providers = hooks;
}

void LoopTelemetryModule::process(uint32_t loopStartUs) {
    if (providers.recordLoopJitterUs && providers.microsNow) {
        const uint32_t jitterUs =
            static_cast<uint32_t>(providers.microsNow(providers.microsContext) - loopStartUs);
        providers.recordLoopJitterUs(providers.loopJitterContext, jitterUs);
    }

    if (providers.refreshDmaCache) {
        providers.refreshDmaCache(providers.dmaCacheContext);
    }

    if (providers.recordHeapStats &&
        providers.readFreeHeap &&
        providers.readLargestHeapBlock &&
        providers.readCachedFreeDma &&
        providers.readCachedLargestDma) {
        providers.recordHeapStats(
            providers.heapStatsContext,
            providers.readFreeHeap(providers.freeHeapContext),
            providers.readLargestHeapBlock(providers.largestHeapBlockContext),
            providers.readCachedFreeDma(providers.cachedFreeDmaContext),
            providers.readCachedLargestDma(providers.cachedLargestDmaContext));
    }
}
