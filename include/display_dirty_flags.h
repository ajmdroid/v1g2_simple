#pragma once

// ============================================================================
// DisplayDirtyFlags — tracks which display elements need a forced redraw
// after a full screen clear or mode change.
//
// Extracted from display.cpp so that display sub-modules (display_arrow.cpp,
// etc.) can read/write the shared dirty-flag aggregate.
// ============================================================================
struct DisplayDirtyFlags {
    bool multiAlert     = false;  // Multi-alert mode (secondary card row visible)
    bool cards          = false;  // Force secondary-card row redraw
    bool frequency      = false;  // Force frequency area redraw
    bool battery        = false;  // Force battery percentage redraw
    bool bands          = false;  // Force band indicator redraw
    bool signalBars     = false;  // Force signal bars redraw
    bool arrow          = false;  // Force direction arrow redraw
    bool muteIcon       = false;  // Force mute icon redraw
    bool topCounter     = false;  // Force top counter (bogey symbol) redraw
    bool lockout        = false;  // Force lockout "L" badge redraw
    bool gpsIndicator   = false;  // Force GPS indicator redraw
    bool obdIndicator   = false;  // Force OBD indicator redraw
    bool resetTracking  = false;  // Signal to reset change-tracking statics

    /// Mark every element as needing a forced redraw (called after screen clear).
    void setAll() {
        frequency    = true;
        battery      = true;
        bands        = true;
        signalBars   = true;
        arrow        = true;
        muteIcon     = true;
        topCounter   = true;
        lockout      = true;
        gpsIndicator = true;
        obdIndicator = true;
    }
};
