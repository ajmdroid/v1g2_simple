#include "obd_auto_connector_module.h"

void ObdAutoConnector::begin(OBDHandler* handler) {
    obd = handler;
}

void ObdAutoConnector::scheduleAfterConnect(unsigned long delayMs) {
    if (!obd) return;
    connectAtMs = millis() + delayMs;
}

void ObdAutoConnector::process(unsigned long nowMs) {
    if (!obd) return;
    if (connectAtMs != 0 && nowMs >= connectAtMs) {
        connectAtMs = 0;
        Serial.println("[OBD] V1 settle delay complete - attempting OBD auto-connect");
        obd->tryAutoConnect();
    }
    obd->update();
}
