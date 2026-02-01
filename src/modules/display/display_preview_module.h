#pragma once

#include <Arduino.h>
#include "packet_parser.h"  // For AlertData, Band, Direction
#include "display.h"        // For V1Display, DisplayState

/**
 * DisplayPreviewModule - drives the color preview/demo cycle
 *
 * Responsibilities:
 * - Run the timed color preview sequence
 * - Expose simple control (start/cancel) and completion flag
 *
 * Does NOT:
 * - Own display connection (expects V1Display pointer)
 * - Manage camera tests or other display flows
 */
class DisplayPreviewModule {
public:
    DisplayPreviewModule();

    void begin(V1Display* display);

    // Start/stop preview
    void requestHold(uint32_t durationMs);
    void cancel();

    // State queries
    bool isRunning() const { return previewActive; }
    // Returns true once when a preview has ended (cancel or elapsed), then resets the flag
    bool consumeEnded();

    // Drive the preview; call from loop()
    void update();

private:
    struct ColorPreviewStep {
        unsigned long offsetMs;
        Band band;
        uint8_t bars;
        Direction dir;
        uint32_t freqMHz;
        bool muted;
        bool showObdIdle;  // If true, show OBD idle display instead of alert
    };

    static constexpr ColorPreviewStep STEPS[] = {
        {0,    BAND_NONE, 0, DIR_NONE, 0,     false, true},   // OBD idle preview
        {1500, BAND_X,    3, DIR_FRONT, 10525, false, false}, // X band
        {2500, BAND_K,    5, DIR_SIDE, 24150,  false, false}, // K band
        {3500, BAND_KA,   6, DIR_REAR, 35500,  false, false}, // Ka band
        {4500, BAND_LASER,8, static_cast<Direction>(DIR_FRONT | DIR_REAR), 0, false, false}, // Laser
        {5500, BAND_KA,   5, DIR_FRONT, 34700, true,  false}  // Muted Ka
    };
    static constexpr int STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);

    V1Display* display = nullptr;

    bool previewActive = false;
    bool previewEnded = false;
    unsigned long previewStartMs = 0;
    unsigned long previewDurationMs = 0;
    int previewStep = 0;
};
