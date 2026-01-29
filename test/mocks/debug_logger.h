// Mock debug_logger.h for native unit testing
#pragma once

enum class DebugLogCategory {
    System,
    Wifi,
    Ble,
    Gps,
    Obd,
    Alerts,
    Display,
    PerfMetrics
};

class DebugLogger {
public:
    bool enabled = false;
    int logfCalls = 0;
    
    void reset() {
        enabled = false;
        logfCalls = 0;
    }
    
    bool isEnabledFor(DebugLogCategory /*cat*/) const { return enabled; }
    void logf(DebugLogCategory /*cat*/, const char* /*fmt*/, ...) { logfCalls++; }
    void log(DebugLogCategory /*cat*/, const char* /*msg*/) { logfCalls++; }
};
