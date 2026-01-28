#pragma once

#include <Arduino.h>
#include "display.h"
#include "packet_parser.h"
#include "ble_client.h"
#include "modules/display/display_preview_module.h"
#include "modules/camera/camera_alert_module.h"

/**
 * DisplayRestoreModule - Handles display restoration after preview/test modes end.
 * 
 * When color preview or camera test modes finish, this module:
 * - Forces a full display redraw
 * - Restores the appropriate V1 state (alerts or scanning)
 * - Updates camera card visibility based on V1 alert state
 */
class DisplayRestoreModule {
public:
    void begin(V1Display* disp,
               PacketParser* pktParser,
               V1BLEClient* ble,
               DisplayPreviewModule* preview,
               CameraAlertModule* cameraAlert);

    /**
     * Check if preview/test ended and restore display if needed.
     * @return true if restoration occurred, false otherwise
     */
    bool process();

private:
    V1Display* display = nullptr;
    PacketParser* parser = nullptr;
    V1BLEClient* bleClient = nullptr;
    DisplayPreviewModule* previewModule = nullptr;
    CameraAlertModule* cameraAlertModule = nullptr;
};
