// Alert Persistence Module - Implementation
// Handles V1 radar alert display state and persistence

#include "alert_persistence_module.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "settings.h"

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
    // Future: could handle periodic tasks here
}

void AlertPersistenceModule::end() {
    initialized = false;
}

void AlertPersistenceModule::clearAllAlertState() {
    clearPersistence();
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
    }
}

void AlertPersistenceModule::clearPersistence() {
    persistedAlert = AlertData();
    alertPersistenceActive = false;
    alertClearedTime = 0;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive && (now - alertClearedTime) < persistMs;
}

