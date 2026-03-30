#pragma once

#include <Arduino.h>
#include "display.h"
#include "packet_parser.h"
#include "ble_client.h"
#include "modules/display/display_preview_module.h"

class DisplayPipelineModule;

/**
 * DisplayRestoreModule - Handles display restoration after preview modes end.
 *
 * When color preview modes finish, this module:
 * - Forces a full display redraw
 * - Restores the appropriate V1 state (alerts or scanning)
 */
class DisplayRestoreModule {
public:
    void begin(V1Display* disp,
               PacketParser* pktParser,
               V1BLEClient* ble,
               DisplayPreviewModule* preview,
               DisplayPipelineModule* displayPipeline);

    /**
     * Check if preview ended and restore display if needed.
     * @return true if restoration occurred, false otherwise
     */
    bool process();

private:
    V1Display* display = nullptr;
    PacketParser* parser = nullptr;
    V1BLEClient* bleClient = nullptr;
    DisplayPreviewModule* previewModule = nullptr;
    DisplayPipelineModule* displayPipelineModule = nullptr;
};
