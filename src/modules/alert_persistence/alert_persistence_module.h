// Alert Persistence Module - Header
// Handles V1 radar alert display persistence

#pragma once

#include <Arduino.h>
#include "packet_parser.h"  // For AlertData type

// Forward declarations
class V1BLEClient;
class V1Display;
class SettingsManager;
class PacketParser;

/**
 * AlertPersistenceModule - V1 alert persistence and cleanup
 *
 * Responsibilities:
 * - Alert persistence (shows faded alert after V1 clears it)
 * - Combined state clearing (voice module handles voice state)
 */
class AlertPersistenceModule {
public:
    AlertPersistenceModule();

    // Initialize with dependencies (call from setup())
    void begin(V1BLEClient* ble, PacketParser* parser, V1Display* display, SettingsManager* settings);

    // Compatibility-retained no-op hook. Production no longer calls this from
    // the main loop, but older/tests callers may still invoke it safely.
    void update();

    // Alert persistence - shows last alert briefly after V1 clears it
    void setPersistedAlert(const AlertData& alert);
    void startPersistence(unsigned long now);
    void clearPersistence();
    bool shouldShowPersisted(unsigned long now, unsigned long persistMs) const;
    const AlertData& getPersistedAlert() const { return persistedAlert; }
    bool isPersistenceActive() const { return alertPersistenceActive; }

private:
    // Dependencies
    V1BLEClient* bleClient = nullptr;
    PacketParser* parser = nullptr;
    V1Display* display = nullptr;
    SettingsManager* settings = nullptr;

    bool initialized = false;

    // Alert persistence state
    AlertData persistedAlert;
    unsigned long alertClearedTime = 0;
    bool alertPersistenceActive = false;
};
