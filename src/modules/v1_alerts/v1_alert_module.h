// V1 Alert Module - Header
// Handles V1 radar alert detection, display, audio, and clearing
// Extracted from main.cpp to improve maintainability

#ifndef V1_ALERT_MODULE_H
#define V1_ALERT_MODULE_H

#include <Arduino.h>

/**
 * V1AlertModule - Encapsulates all V1 alert handling
 * 
 * Responsibilities:
 * - BLE data parsing (via existing PacketParser)
 * - Alert display rendering
 * - Alert audio playback
 * - Alert clearing/timeout logic
 * 
 * This is a stub for Phase 1 migration. Will grow incrementally.
 */
class V1AlertModule {
public:
    V1AlertModule();
    
    // Main update - call from loop()
    void update();
    
    // Lifecycle
    void begin();
    void end();
    
private:
    // Module state (will be populated during migration)
};

#endif // V1_ALERT_MODULE_H
