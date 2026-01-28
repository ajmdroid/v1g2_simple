// V1 Alert Module - Header
// Handles V1 radar alert detection, display, audio, and clearing
// Extracted from main.cpp to improve maintainability

#ifndef V1_ALERT_MODULE_H
#define V1_ALERT_MODULE_H

#include <Arduino.h>
#include "packet_parser.h"  // For AlertData type

// Forward declarations (avoid including heavy headers)
class V1BLEClient;
class V1Display;
class SettingsManager;

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
    
    // Initialize with dependencies (call from setup())
    void begin(V1BLEClient* ble, PacketParser* parser, V1Display* display, SettingsManager* settings);
    
    // Main update - call from loop()
    void update();
    
    // Cleanup
    void end();
    
    // Static utility: Get signal bars for alert based on direction
    // Returns front strength if front, rear if rear, otherwise max of both
    static uint8_t getAlertBars(const AlertData& alert);

private:
    // Dependencies (set in begin())
    V1BLEClient* bleClient = nullptr;
    PacketParser* parser = nullptr;
    V1Display* display = nullptr;
    SettingsManager* settings = nullptr;
    
    bool initialized = false;
};

#endif // V1_ALERT_MODULE_H
