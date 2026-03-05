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
    bool isRunning() const { return previewActive; }
    // Returns true once when a preview has ended (cancel or elapsed), then resets the flag
    bool consumeEnded();

    // Drive the preview; call from loop()
    void update();

private:
    enum class PreviewMode : uint8_t {
        ALERT = 0,
    };

    enum class PreviewStepKind : uint8_t {
        RADAR = 0,
        CAMERA,
    };

    struct ColorPreviewStep {
        unsigned long offsetMs;
        PreviewStepKind kind;
        Band band;
        uint8_t bars;
        Direction dir;
        uint32_t freqMHz;
        bool muted;
        CameraType cameraType;
        uint16_t cameraDistanceCm;
    };

    static constexpr ColorPreviewStep STEPS[] = {
        {0,    PreviewStepKind::RADAR,  BAND_X,     3, DIR_FRONT, 10525, false, CameraType::SPEED,     0},
        {1000, PreviewStepKind::RADAR,  BAND_K,     5, DIR_SIDE,  24150, false, CameraType::SPEED,     0},
        {2000, PreviewStepKind::RADAR,  BAND_KA,    6, DIR_REAR,  35500, false, CameraType::SPEED,     0},
        {3000, PreviewStepKind::RADAR,  BAND_LASER, 8, static_cast<Direction>(DIR_FRONT | DIR_REAR), 0, false, CameraType::SPEED,     0},
        {4000, PreviewStepKind::RADAR,  BAND_KA,    5, DIR_FRONT, 34700, true,  CameraType::SPEED,     0},
        {4500, PreviewStepKind::CAMERA, BAND_NONE,  0, DIR_NONE,      0, false, CameraType::SPEED, 30480},  // ~1000 ft
        {5000, PreviewStepKind::CAMERA, BAND_NONE,  0, DIR_NONE,      0, false, CameraType::RED_LIGHT, 15240}, // ~500 ft
    };
    static constexpr int STEP_COUNT = sizeof(STEPS) / sizeof(STEPS[0]);
    static constexpr uint32_t PREVIEW_TAIL_MS = 600;  // Extra time after last step to keep frame visible

    V1Display* display = nullptr;

    PreviewMode previewMode = PreviewMode::ALERT;
    bool previewActive = false;
    bool previewEnded = false;
    unsigned long previewStartMs = 0;
    unsigned long previewDurationMs = 0;
    int previewStep = 0;
};
