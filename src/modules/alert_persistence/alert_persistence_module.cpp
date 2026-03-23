// Alert Persistence Module - Implementation
// Handles V1 radar alert display state and persistence

#include "alert_persistence_module.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "settings.h"
#include "perf_metrics.h"

AlertPersistenceModule::AlertPersistenceModule() {
    // Dependencies set in begin()
}

void AlertPersistenceModule::begin(V1BLEClient* ble, PacketParser* pParser, V1Display* disp, SettingsManager* sett) {
    bleClient = ble;
    parser = pParser;
    display = disp;
    settings = sett;
    initialized = true;

    Serial.println("[AlertPersistenceModule] Initialized");
}

void AlertPersistenceModule::update() {
    if (!initialized) return;
    // Compatibility-retained no-op: production no longer needs loop work here.
}

// ============================================================================
// Alert Persistence - shows last alert briefly after V1 clears it
// ============================================================================

void AlertPersistenceModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert = alert;
    alertPersistenceActive = false;
    alertClearedTime = 0;
}

void AlertPersistenceModule::startPersistence(unsigned long now) {
    if (persistedAlert.isValid && alertClearedTime == 0) {
        alertClearedTime = now;
        alertPersistenceActive = true;
        PERF_INC(alertPersistStarts);
    }
}

void AlertPersistenceModule::clearPersistence() {
    const bool hadState = alertPersistenceActive || persistedAlert.isValid || (alertClearedTime != 0);
    if (hadState) {
        PERF_INC(alertPersistClears);
    }
    persistedAlert = AlertData();
    alertPersistenceActive = false;
    alertClearedTime = 0;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive && (now - alertClearedTime) < persistMs;
}
