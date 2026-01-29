// Mock ble_queue_module.h for native unit testing
#pragma once

class BleQueueModule {
public:
    unsigned long lastRxMillis = 0;
    
    void reset() {
        lastRxMillis = 0;
    }
    
    void setLastRxMillis(unsigned long ms) {
        lastRxMillis = ms;
    }
    
    unsigned long getLastRxMillis() const {
        return lastRxMillis;
    }
};
