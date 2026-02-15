#include "display_preview_module.h"

DisplayPreviewModule::DisplayPreviewModule() {
    // display set in begin()
}

void DisplayPreviewModule::begin(V1Display* disp) {
    display = disp;
}

void DisplayPreviewModule::requestHold(uint32_t durationMs) {
    previewMode = PreviewMode::ALERT;
    previewActive = true;
    previewStartMs = millis();
    previewDurationMs = durationMs + PREVIEW_TAIL_MS;
    previewStep = 0;
    previewEnded = false;
    singleCameraType = CAMERA_TYPE_MIN;
    singleCameraMuted = false;
}

void DisplayPreviewModule::requestCameraCycle(uint32_t durationMs) {
    previewMode = PreviewMode::CAMERA_CYCLE;
    previewActive = true;
    previewStartMs = millis();
    previewDurationMs = durationMs + PREVIEW_TAIL_MS;
    previewStep = 0;
    previewEnded = false;
    singleCameraType = CAMERA_TYPE_MIN;
    singleCameraMuted = false;
}

void DisplayPreviewModule::requestCameraSingle(uint8_t cameraType, uint32_t durationMs, bool muted) {
    if (cameraType < CAMERA_TYPE_MIN || cameraType > CAMERA_TYPE_MAX) {
        cameraType = CAMERA_TYPE_MIN;
    }
    previewMode = PreviewMode::CAMERA_SINGLE;
    previewActive = true;
    previewStartMs = millis();
    previewDurationMs = durationMs + PREVIEW_TAIL_MS;
    previewStep = 0;
    previewEnded = false;
    singleCameraType = cameraType;
    singleCameraMuted = muted;
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

    if (previewMode == PreviewMode::ALERT) {
        // Advance through at most one band sample per update to avoid multiple flushes in one loop.
        if (previewStep < STEP_COUNT && elapsed >= STEPS[previewStep].offsetMs) {
            const auto& step = STEPS[previewStep];

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

            // Build array with secondary alerts to show cards during preview.
            AlertData allAlerts[3];
            int alertCount = 1;
            allAlerts[0] = previewAlert;
            previewState.activeBands = static_cast<Band>(previewState.activeBands);

            if (previewStep >= 2) {
                // Add X band as secondary card.
                allAlerts[alertCount].band = BAND_X;
                allAlerts[alertCount].direction = DIR_FRONT;
                allAlerts[alertCount].frontStrength = 3;
                allAlerts[alertCount].frequency = 10525;
                allAlerts[alertCount].isValid = true;
                previewState.activeBands = static_cast<Band>(previewState.activeBands | BAND_X);
                alertCount++;
            }
            if (previewStep >= 3) {
                // Add K band as secondary card.
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
    } else if (previewMode == PreviewMode::CAMERA_CYCLE) {
        // Advance through at most one camera frame per update.
        if (previewStep < CAMERA_STEP_COUNT && elapsed >= CAMERA_STEPS[previewStep].offsetMs) {
            const auto& step = CAMERA_STEPS[previewStep];
            display->updateCameraAlert(step.type, step.muted);
            previewStep++;
        }
    } else if (previewMode == PreviewMode::CAMERA_SINGLE) {
        // Single camera demo only draws once per run.
        if (previewStep == 0) {
            display->updateCameraAlert(singleCameraType, singleCameraMuted);
            previewStep = 1;
        }
    }

    if (elapsed >= previewDurationMs) {
        previewActive = false;
        previewEnded = true;
    }
}
