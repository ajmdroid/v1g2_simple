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
    bleClient_ = ble;
    parser_ = pParser;
    display_ = disp;
    settings_ = sett;

    Serial.println("[AlertPersistenceModule] Initialized");
}

// ============================================================================
// Alert Persistence - shows last alert briefly after V1 clears it
// ============================================================================

void AlertPersistenceModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert_ = alert;
    alertPersistenceActive_ = false;
    alertClearedTime_ = 0;
}

void AlertPersistenceModule::startPersistence(unsigned long now) {
    if (persistedAlert_.isValid && alertClearedTime_ == 0) {
        alertClearedTime_ = now;
        alertPersistenceActive_ = true;
        PERF_INC(alertPersistStarts);
    }
}

void AlertPersistenceModule::clearPersistence() {
    const bool hadState = alertPersistenceActive_ || persistedAlert_.isValid || (alertClearedTime_ != 0);
    if (hadState) {
        PERF_INC(alertPersistClears);
    }
    persistedAlert_ = AlertData();
    alertPersistenceActive_ = false;
    alertClearedTime_ = 0;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive_ && (now - alertClearedTime_) < persistMs;
}
