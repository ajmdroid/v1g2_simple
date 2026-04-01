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
 * - Manage other display ownership flows
 */
class DisplayPreviewModule {
public:
    DisplayPreviewModule();

    void begin(V1Display* display);

    // Start/stop preview
    void requestHold(uint32_t durationMs);
    void cancel();

    // State queries
    bool isRunning() const { return previewActive_; }
    // Returns true once when a preview has ended (cancel or elapsed), then resets the flag
    bool consumeEnded();

    // Drive the preview; call from loop()
    void update();

private:
    enum class PreviewMode : uint8_t {
        ALERT = 0,
    };

    struct ColorPreviewStep {
        unsigned long offsetMs;
        Band band;
        uint8_t bars;
        Direction dir;
        uint32_t freqMHz;
        bool muted;
    };

    static constexpr ColorPreviewStep STEPS[] = {
        {0,    BAND_X,    3, DIR_FRONT, 10525, false}, // X band
        {1000, BAND_K,    5, DIR_SIDE, 24150,  false}, // K band
        {2000, BAND_KA,   6, DIR_REAR, 35500,  false}, // Ka band
        {3000, BAND_LASER,8, static_cast<Direction>(DIR_FRONT | DIR_REAR), 0, false}, // Laser
        {4000, BAND_KA,   5, DIR_FRONT, 34700, true}  // Muted Ka
    };
    static constexpr int STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
    static constexpr uint32_t PREVIEW_TAIL_MS = 600;  // Extra time after last step to keep frame visible

    V1Display* display_ = nullptr;

    PreviewMode previewMode_ = PreviewMode::ALERT;
    bool previewActive_ = false;
    bool previewEnded_ = false;
    unsigned long previewStartMs_ = 0;
    unsigned long previewDurationMs_ = 0;
    int previewStep_ = 0;
};
