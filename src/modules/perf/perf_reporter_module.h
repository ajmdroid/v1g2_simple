#pragma once

#include <Arduino.h>

#include "perf_metrics.h"
#include "debug_logger.h"
#include "settings.h"

class PerfReporterModule {
public:
    void begin(DebugLogger* dbgLogger, SettingsManager* settingsMgr);
    void process(unsigned long nowMs);

private:
    DebugLogger* debugLogger = nullptr;
    SettingsManager* settings = nullptr;
    unsigned long lastLogMs = 0;
};
