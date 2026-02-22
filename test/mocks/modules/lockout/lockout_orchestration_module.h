#pragma once

#include <cstdint>

#include "../gps/gps_runtime_module.h"

struct LockoutOrchestrationResult {
    bool prioritySuppressed = false;
};

class LockoutOrchestrationModule {
public:
    int processCalls = 0;
    uint32_t lastNowMs = 0;
    bool lastProxyConnected = false;
    bool lastEnableSignalTrace = false;
    GpsRuntimeStatus lastGpsStatus{};
    LockoutOrchestrationResult nextResult{};

    LockoutOrchestrationResult process(uint32_t nowMs,
                                       const GpsRuntimeStatus& gpsStatus,
                                       bool proxyClientConnected,
                                       bool enableSignalTrace) {
        processCalls++;
        lastNowMs = nowMs;
        lastGpsStatus = gpsStatus;
        lastProxyConnected = proxyClientConnected;
        lastEnableSignalTrace = enableSignalTrace;
        return nextResult;
    }
};
