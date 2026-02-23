#include "display_restore_module.h"

void DisplayRestoreModule::begin(V1Display* disp,
                                  PacketParser* pktParser,
                                  V1BLEClient* ble,
                                  DisplayPreviewModule* preview) {
    display = disp;
    parser = pktParser;
    bleClient = ble;
    previewModule = preview;
}

bool DisplayRestoreModule::process() {
    if (!previewModule) return false;

    bool previewEnded = previewModule->consumeEnded();

    if (!previewEnded) {
        return false;
    }

    // Preview finished - restore normal display with fresh V1 data
    display->forceNextRedraw();

    if (bleClient->isConnected()) {
        // Immediately refresh with current V1 state (don't wait for next packet)
        DisplayState state = parser->getDisplayState();
        if (parser->hasAlerts()) {
            AlertData priority;
            if (parser->getRenderablePriorityAlert(priority)) {
                const auto& alerts = parser->getAllAlerts();
                display->update(priority, alerts.data(), parser->getAlertCount(), state);
            } else {
                display->update(state);
            }
        } else {
            display->update(state);
        }
    } else {
        // V1 not connected - show scanning screen (not resting!)
        display->showScanning();
    }

    Serial.println("[Display] Color preview ended - restored display");

    return true;
}
