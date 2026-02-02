#include "perf_reporter_module.h"
#include <ArduinoJson.h>

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

    // Emit machine-readable METRICS JSON line for benchmark tracking
    JsonDocument doc;
    doc["ms"] = nowMs;
    doc["rx"] = perfCounters.rxPackets.load();
    doc["qDrop"] = perfCounters.queueDrops.load();
    doc["qHW"] = perfCounters.queueHighWater.load();
    doc["prxHW"] = perfCounters.proxyQueueHighWater.load();
    doc["phoneHW"] = perfCounters.phoneCmdQueueHighWater.load();
    doc["obdHW"] = perfCounters.obdScanQueueHighWater.load();
    doc["parseOK"] = perfCounters.parseSuccesses.load();
    doc["parseFail"] = perfCounters.parseFailures.load();
    doc["disc"] = perfCounters.disconnects.load();
    doc["reconn"] = perfCounters.reconnects.load();
    doc["dispP95"] = perfGetNotifyToDisplayP95Ms();
    doc["dispMax"] = perfGetNotifyToDisplayMaxMs();
    doc["prxP95"] = perfGetNotifyToProxyP95Ms();
    doc["prxMax"] = perfGetNotifyToProxyMaxMs();
    doc["loopMaxUs"] = perfGetLoopMaxUs();
    doc["heapMin"] = perfGetMinFreeHeap();
    doc["blockMin"] = perfGetMinLargestBlock();
    
    String jsonStr;
    serializeJson(doc, jsonStr);
    debugLogger->logf(DebugLogCategory::PerfMetrics, "METRICS %s", jsonStr.c_str());

    // Keep existing human-readable format for backward compatibility
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
