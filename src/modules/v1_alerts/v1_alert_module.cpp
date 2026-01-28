// V1 Alert Module - Implementation
// Handles V1 radar alert detection, display, audio, and clearing

#include "v1_alert_module.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "settings.h"

V1AlertModule::V1AlertModule() {
    // Constructor - dependencies set in begin()
}

void V1AlertModule::begin(V1BLEClient* ble, PacketParser* pParser, V1Display* disp, SettingsManager* sett) {
    bleClient = ble;
    parser = pParser;
    display = disp;
    settings = sett;
    initialized = true;
    
    Serial.println("[V1AlertModule] Initialized with dependencies");
}

void V1AlertModule::update() {
    // Main update loop - will be populated during migration
    // For now, does nothing (main.cpp still handles everything)
    if (!initialized) return;
}

void V1AlertModule::end() {
    // Cleanup - will be populated during migration
    initialized = false;
}

// Static utility: Get signal bars for alert based on direction
uint8_t V1AlertModule::getAlertBars(const AlertData& a) {
    if (a.direction & DIR_FRONT) return a.frontStrength;
    if (a.direction & DIR_REAR) return a.rearStrength;
    return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
}

// Static utility: Create unique alert ID from band and frequency
// Alert ID = (band << 16) | frequency - ensures Laser (freq=0) is unique per band
uint32_t V1AlertModule::makeAlertId(Band band, uint16_t freq) {
    return ((uint32_t)band << 16) | freq;
}

// Announced alert tracking - check if alert has been announced
bool V1AlertModule::isAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    for (int i = 0; i < announcedAlertCount; i++) {
        if (announcedAlertIds[i] == id) return true;
    }
    return false;
}

// Announced alert tracking - mark alert as announced
void V1AlertModule::markAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    if (announcedAlertCount < MAX_ANNOUNCED_ALERTS && !isAlertAnnounced(band, freq)) {
        announcedAlertIds[announcedAlertCount++] = id;
    }
}

// Announced alert tracking - clear all announced alerts
void V1AlertModule::clearAnnouncedAlerts() {
    announcedAlertCount = 0;
    memset(announcedAlertIds, 0, sizeof(announcedAlertIds));
}
