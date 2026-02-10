#include "display_preview_module.h"

DisplayPreviewModule::DisplayPreviewModule() {
    // display set in begin()
}

void DisplayPreviewModule::begin(V1Display* disp) {
    display = disp;
}

void DisplayPreviewModule::requestHold(uint32_t durationMs) {
    previewActive = true;
    previewStartMs = millis();
    previewDurationMs = durationMs + PREVIEW_TAIL_MS;
    previewStep = 0;
    previewEnded = false;
}

void DisplayPreviewModule::cancel() {
    if (previewActive) {
        previewActive = false;
        previewEnded = true;  // signal caller to restore display
    }
}

bool DisplayPreviewModule::consumeEnded() {
    if (previewEnded) {
        previewEnded = false;
        return true;
    }
    return false;
}

void DisplayPreviewModule::update() {
    if (!previewActive || !display) return;

    unsigned long now = millis();
    unsigned long elapsed = now - previewStartMs;

    // Advance through at most one band sample per update to avoid multiple flushes in one loop
    if (previewStep < STEP_COUNT && elapsed >= STEPS[previewStep].offsetMs) {
        const auto& step = STEPS[previewStep];
        
        // Normal alert preview step
        AlertData previewAlert{};
        previewAlert.band = step.band;
        previewAlert.direction = step.dir;
        previewAlert.frontStrength = step.bars;
        previewAlert.rearStrength = 0;
        previewAlert.frequency = step.freqMHz;
        previewAlert.isValid = true;

        DisplayState previewState{};
        previewState.activeBands = step.band;
        previewState.arrows = step.dir;
        previewState.signalBars = step.bars;
        previewState.muted = step.muted;

        // Build array with secondary alerts to show cards during preview
        AlertData allAlerts[3];
        int alertCount = 1;
        allAlerts[0] = previewAlert;
        previewState.activeBands = static_cast<Band>(previewState.activeBands);
        
        if (previewStep >= 2) {
            // Add X band as secondary card
            allAlerts[alertCount].band = BAND_X;
            allAlerts[alertCount].direction = DIR_FRONT;
            allAlerts[alertCount].frontStrength = 3;
            allAlerts[alertCount].frequency = 10525;
            allAlerts[alertCount].isValid = true;
            previewState.activeBands = static_cast<Band>(previewState.activeBands | BAND_X);
            alertCount++;
        }
        if (previewStep >= 3) {
            // Add K band as secondary card
            allAlerts[alertCount].band = BAND_K;
            allAlerts[alertCount].direction = DIR_REAR;
            allAlerts[alertCount].frontStrength = 0;
            allAlerts[alertCount].rearStrength = 4;
            allAlerts[alertCount].frequency = 24150;
            allAlerts[alertCount].isValid = true;
            previewState.activeBands = static_cast<Band>(previewState.activeBands | BAND_K);
            alertCount++;
        }

        display->update(previewAlert, allAlerts, alertCount, previewState);
        previewStep++;
    }

    if (elapsed >= previewDurationMs) {
        previewActive = false;
        previewEnded = true;
    }
}
