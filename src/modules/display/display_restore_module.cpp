#include "display_restore_module.h"

void DisplayRestoreModule::begin(V1Display* disp,
                                  PacketParser* pktParser,
                                  V1BLEClient* ble,
                                  DisplayPreviewModule* preview,
                                  CameraAlertModule* cameraAlert) {
    display = disp;
    parser = pktParser;
    bleClient = ble;
    previewModule = preview;
    cameraAlertModule = cameraAlert;
}

bool DisplayRestoreModule::process() {
    if (!previewModule || !cameraAlertModule) return false;

    bool previewEnded = previewModule->consumeEnded();
    bool cameraTestEnded = cameraAlertModule->consumeTestEnded();

    if (!previewEnded && !cameraTestEnded) {
        return false;
    }

    // Preview/test finished - restore normal display with fresh V1 data
    display->forceNextRedraw();

    if (bleClient->isConnected()) {
        // Immediately refresh with current V1 state (don't wait for next packet)
        DisplayState state = parser->getDisplayState();
        if (parser->hasAlerts()) {
            AlertData priority = parser->getPriorityAlert();
            const auto& alerts = parser->getAllAlerts();
            cameraAlertModule->updateCardStateForV1(true);  // V1 has alerts
            display->update(priority, alerts.data(), parser->getAlertCount(), state);
        } else {
            cameraAlertModule->updateCardStateForV1(false);  // No V1 alerts
            display->update(state);
        }
    } else {
        // V1 not connected - show scanning screen (not resting!)
        display->showScanning();
    }

    if (previewEnded) {
        Serial.println("[Display] Color preview ended - restored display");
    }
    if (cameraTestEnded) {
        Serial.println("[Display] Camera test ended - restored display");
    }

    return true;
}
