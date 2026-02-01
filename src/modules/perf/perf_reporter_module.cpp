#include "perf_reporter_module.h"

void PerfReporterModule::begin(DebugLogger* dbgLogger, SettingsManager* settingsMgr) {
    debugLogger = dbgLogger;
    settings = settingsMgr;
    lastLogMs = millis();
}

void PerfReporterModule::process(unsigned long nowMs) {
    if (!debugLogger || !settings) return;

    const V1Settings& s = settings->get();
    if (!s.logPerfMetrics || !debugLogger->isEnabledFor(DebugLogCategory::PerfMetrics)) {
        return;
    }

    // Log every 5 seconds (rate-limited)
    if (nowMs - lastLogMs < 5000) {
        return;
    }
    lastLogMs = nowMs;

    debugLogger->logf(DebugLogCategory::PerfMetrics,
        "PerfMetrics: rx=%lu qDrop=%lu qHW=%lu prxHW=%lu phoneHW=%lu obdHW=%lu parseOK=%lu parseFail=%lu disc=%lu reconn=%lu dispP95=%lums dispMax=%lums prxP95=%lums prxMax=%lums loopMax=%luus heapMin=%lu blockMin=%lu",
        perfCounters.rxPackets.load(),
        perfCounters.queueDrops.load(),
        perfCounters.queueHighWater.load(),
        perfCounters.proxyQueueHighWater.load(),
        perfCounters.phoneCmdQueueHighWater.load(),
        perfCounters.obdScanQueueHighWater.load(),
        perfCounters.parseSuccesses.load(),
        perfCounters.parseFailures.load(),
        perfCounters.disconnects.load(),
        perfCounters.reconnects.load(),
        perfGetNotifyToDisplayP95Ms(),
        perfGetNotifyToDisplayMaxMs(),
        perfGetNotifyToProxyP95Ms(),
        perfGetNotifyToProxyMaxMs(),
        perfGetLoopMaxUs(),
        perfGetMinFreeHeap(),
        perfGetMinLargestBlock());

    perfExtendedResetWindow();
}
