#include "display_restore_module.h"
#include "display_pipeline_module.h"
#include "perf_metrics.h"

void DisplayRestoreModule::begin(V1Display* disp,
                                 PacketParser* pktParser,
                                 V1BLEClient* ble,
                                 DisplayPreviewModule* preview,
                                 DisplayPipelineModule* displayPipeline) {
    display = disp;
    parser = pktParser;
    bleClient = ble;
    previewModule = preview;
    displayPipelineModule = displayPipeline;
}

bool DisplayRestoreModule::process() {
    if (!previewModule) return false;

    bool previewEnded = previewModule->consumeEnded();

    if (!previewEnded) {
        return false;
    }

    if (displayPipelineModule) {
        displayPipelineModule->restoreCurrentOwner(millis());
    } else if (display && parser && bleClient) {
        // Defensive fallback for tests or partial wiring; production should use the pipeline.
        display->forceNextRedraw();
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        const unsigned long renderStartUs = micros();
        if (bleClient->isConnected()) {
            display->update(parser->getDisplayState());
        } else {
            display->showScanning();
        }
        perfRecordDisplayScenarioRenderUs(micros() - renderStartUs);
        perfClearDisplayRenderScenario();
    }

    Serial.println("[Display] Color preview ended - restored display");

    return true;
}
