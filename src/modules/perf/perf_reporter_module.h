#pragma once

#include <Arduino.h>

#include "perf_metrics.h"
#include "debug_logger.h"
#include "settings.h"

class PerfReporterModule {
public:
    void begin(DebugLogger* dbgLogger, SettingsManager* settingsMgr);
    void process(unsigned long nowMs);
    void applyBenchmarkPreset(bool persist);
    void startInvestigationBurst(unsigned long nowMs, bool persist = false);
    bool isInvestigationActive() const { return investigationActive; }

private:
    DebugLogger* debugLogger = nullptr;
    SettingsManager* settings = nullptr;
    unsigned long lastLogMs = 0;
    uint32_t lastQDrop = 0;
    bool investigationActive = false;
    unsigned long investigationEndMs = 0;
    unsigned long lastAutoTriggerMs = 0;  // Cooldown for auto-trigger
    
    // Snapshot of log config before investigation (for restore)
    DebugLogFilter savedFilter;
    bool hasSavedConfig = false;

    void applyCurrentLogConfig();
    void snapshotLogConfig();
    void restoreLogConfig();
};
