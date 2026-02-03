#include "perf_reporter_module.h"

// Auto-investigation cooldown: minimum 60 seconds between auto-triggers
static constexpr unsigned long AUTO_TRIGGER_COOLDOWN_MS = 60000;
// Loop stall threshold for auto-investigation (500ms)
static constexpr uint32_t LOOP_STALL_THRESHOLD_US = 500000;

void PerfReporterModule::begin(DebugLogger* dbgLogger, SettingsManager* settingsMgr) {
    debugLogger = dbgLogger;
    settings = settingsMgr;
    lastLogMs = millis();
    lastQDrop = 0;
    investigationActive = false;
    investigationEndMs = 0;
    lastAutoTriggerMs = 0;
    hasSavedConfig = false;
}

void PerfReporterModule::snapshotLogConfig() {
    if (!debugLogger || !settings || hasSavedConfig) return;
    DebugLogConfig cfg = settings->getDebugLogConfig();
    savedFilter = {cfg.alerts, cfg.wifi, cfg.ble, cfg.gps, cfg.obd, cfg.system, 
                   cfg.display, cfg.perfMetrics, cfg.audio, cfg.camera, cfg.lockout, cfg.touch};
    hasSavedConfig = true;
}

void PerfReporterModule::restoreLogConfig() {
    if (!debugLogger || !settings || !hasSavedConfig) return;
    debugLogger->setFilter(savedFilter);
    hasSavedConfig = false;
}

void PerfReporterModule::process(unsigned long nowMs) {
    if (!debugLogger || !settings) return;

    if (investigationActive && nowMs >= investigationEndMs) {
        restoreLogConfig();
        investigationActive = false;
    }

    const V1Settings& s = settings->get();
    if (!s.logPerfMetrics || !debugLogger->isEnabledFor(DebugLogCategory::PerfMetrics)) {
        return;
    }

    // Log every 5 seconds (rate-limited)
    if (nowMs - lastLogMs < 5000) {
        return;
    }
    lastLogMs = nowMs;

    uint32_t rxPackets = perfCounters.rxPackets.load();
    uint32_t qDrop = perfCounters.queueDrops.load();
    uint32_t qHW = perfCounters.queueHighWater.load();
    uint32_t prxHW = perfCounters.proxyQueueHighWater.load();
    uint32_t phoneHW = perfCounters.phoneCmdQueueHighWater.load();
    uint32_t obdHW = perfCounters.obdScanQueueHighWater.load();
    uint32_t parseOK = perfCounters.parseSuccesses.load();
    uint32_t parseFail = perfCounters.parseFailures.load();
    uint32_t disc = perfCounters.disconnects.load();
    uint32_t reconn = perfCounters.reconnects.load();
    uint32_t dispP95 = perfGetNotifyToDisplayP95Ms();
    uint32_t dispMax = perfGetNotifyToDisplayMaxMs();
    uint32_t prxP95 = perfGetNotifyToProxyP95Ms();
    uint32_t prxMax = perfGetNotifyToProxyMaxMs();
    uint32_t loopMaxUs = perfGetLoopMaxUs();
    uint32_t heapMin = perfGetMinFreeHeap();
    uint32_t blockMin = perfGetMinLargestBlock();
    uint32_t wifiMaxUs = perfGetWifiMaxUs();
    uint32_t fsMaxUs = perfGetFsMaxUs();
    uint32_t sdMaxUs = perfGetSdMaxUs();
    uint32_t flushMaxUs = perfGetFlushMaxUs();
    uint32_t bleDrainMaxUs = perfGetBleDrainMaxUs();

    // Build structured fields string for NDJSON (no message escaping needed)
    char fields[384];
    snprintf(fields, sizeof(fields),
        "\"uptime_ms\":%lu,\"rx\":%lu,\"qDrop\":%lu,\"qHW\":%lu,\"prxHW\":%lu,"
        "\"phoneHW\":%lu,\"obdHW\":%lu,\"parseOK\":%lu,\"parseFail\":%lu,"
        "\"disc\":%lu,\"reconn\":%lu,\"dispP95_ms\":%lu,\"dispMax_ms\":%lu,"
        "\"prxP95_ms\":%lu,\"prxMax_ms\":%lu,\"loopMax_us\":%lu,"
        "\"heapMin\":%lu,\"blockMin\":%lu,\"wifiMax_us\":%lu,\"fsMax_us\":%lu,"
        "\"sdMax_us\":%lu,\"flushMax_us\":%lu,\"bleDrainMax_us\":%lu",
        (unsigned long)nowMs,
        (unsigned long)rxPackets,
        (unsigned long)qDrop,
        (unsigned long)qHW,
        (unsigned long)prxHW,
        (unsigned long)phoneHW,
        (unsigned long)obdHW,
        (unsigned long)parseOK,
        (unsigned long)parseFail,
        (unsigned long)disc,
        (unsigned long)reconn,
        (unsigned long)dispP95,
        (unsigned long)dispMax,
        (unsigned long)prxP95,
        (unsigned long)prxMax,
        (unsigned long)loopMaxUs,
        (unsigned long)heapMin,
        (unsigned long)blockMin,
        (unsigned long)wifiMaxUs,
        (unsigned long)fsMaxUs,
        (unsigned long)sdMaxUs,
        (unsigned long)flushMaxUs,
        (unsigned long)bleDrainMaxUs);

    debugLogger->logPerfMetrics(fields);

    uint32_t qDropDelta = (qDrop > lastQDrop) ? (qDrop - lastQDrop) : 0;
    lastQDrop = qDrop;

    // Auto-trigger investigation on stalls, with 60s cooldown
    bool triggerCondition = (loopMaxUs > LOOP_STALL_THRESHOLD_US || qDropDelta > 0);
    bool cooldownExpired = (nowMs - lastAutoTriggerMs >= AUTO_TRIGGER_COOLDOWN_MS);
    
    if (!investigationActive && triggerCondition && cooldownExpired) {
        lastAutoTriggerMs = nowMs;
        
        // Capture incident - dump breadcrumb ring buffer to SD for post-mortem
        const char* reason = (loopMaxUs > LOOP_STALL_THRESHOLD_US) ? "loopStall" : "queueDrop";
        debugLogger->captureIncident(reason, loopMaxUs, qDropDelta);
        
        // Log trigger event before starting investigation
        debugLogger->logf(DebugLogCategory::System,
            "investigation_triggered loopMax_us=%lu qDropDelta=%lu",
            (unsigned long)loopMaxUs, (unsigned long)qDropDelta);
        startInvestigationBurst(nowMs, false);
    }

    perfExtendedResetWindow();
}

void PerfReporterModule::applyBenchmarkPreset(bool persist) {
    if (!debugLogger || !settings) return;
    bool deferSave = !persist;

    settings->setEnableDebugLogging(true, deferSave);
    settings->setLogSystem(true, deferSave);
    settings->setLogPerfMetrics(true, deferSave);
    settings->setLogWifi(true, deferSave);

    settings->setLogAlerts(false, deferSave);
    settings->setLogBle(false, deferSave);
    settings->setLogGps(false, deferSave);
    settings->setLogObd(false, deferSave);
    settings->setLogDisplay(false, deferSave);
    settings->setLogAudio(false, deferSave);
    settings->setLogCamera(false, deferSave);
    settings->setLogLockout(false, deferSave);
    settings->setLogTouch(false, deferSave);

    applyCurrentLogConfig();
}

void PerfReporterModule::startInvestigationBurst(unsigned long nowMs, bool persist) {
    if (!debugLogger || !settings) return;
    
    // Snapshot current config before modifying (for restore after burst)
    snapshotLogConfig();
    
    bool deferSave = !persist;

    settings->setEnableDebugLogging(true, deferSave);
    settings->setLogSystem(true, deferSave);
    settings->setLogPerfMetrics(true, deferSave);
    settings->setLogWifi(true, deferSave);
    settings->setLogAlerts(true, deferSave);
    settings->setLogBle(true, deferSave);
    settings->setLogGps(true, deferSave);
    settings->setLogObd(true, deferSave);
    settings->setLogDisplay(true, deferSave);
    settings->setLogAudio(true, deferSave);
    settings->setLogCamera(true, deferSave);
    settings->setLogLockout(true, deferSave);
    settings->setLogTouch(true, deferSave);

    applyCurrentLogConfig();

    investigationActive = true;
    investigationEndMs = nowMs + 20000UL;
}

void PerfReporterModule::applyCurrentLogConfig() {
    if (!debugLogger || !settings) return;
    DebugLogConfig cfg = settings->getDebugLogConfig();
    DebugLogFilter filter{cfg.alerts, cfg.wifi, cfg.ble, cfg.gps, cfg.obd, cfg.system, cfg.display, cfg.perfMetrics, cfg.audio, cfg.camera, cfg.lockout, cfg.touch};
    debugLogger->setFilter(filter);
    debugLogger->setFormat(cfg.format == 1 ? DebugLogFormat::JSON : DebugLogFormat::TEXT);
    debugLogger->setEnabled(settings->get().enableDebugLogging);
}
