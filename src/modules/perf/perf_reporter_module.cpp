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

    // Log once per minute
    if (nowMs - lastLogMs < 60000) {
        return;
    }
    lastLogMs = nowMs;

    debugLogger->logf(DebugLogCategory::PerfMetrics,
        "PerfMetrics: rx=%lu qDrop=%lu parseOK=%lu parseFail=%lu disc=%lu reconn=%lu",
        perfCounters.rxPackets.load(),
        perfCounters.queueDrops.load(),
        perfCounters.parseSuccesses.load(),
        perfCounters.parseFailures.load(),
        perfCounters.disconnects.load(),
        perfCounters.reconnects.load());
}
