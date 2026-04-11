#pragma once

// ============================================================================
// DisplayDirtyFlags — tracks which display elements need a forced redraw
// after a full screen clear or mode change.
//
// Extracted from display.cpp so that display sub-modules (display_arrow.cpp,
// etc.) can read/write the shared dirty-flag aggregate.
// ============================================================================
struct DisplayDirtyFlags {
    bool multiAlert     = false;  // Layout mode flag (not element cache)
    bool cards          = false;  // Force-redraw signal set from display_update.cpp
    bool obdIndicator   = false;  // Force-redraw signal for OBD indicator flush
    bool alpIndicator   = false;  // Force-redraw signal for ALP indicator flush
    bool resetTracking  = false;  // Signals element cache and tracking state reset

    /// Mark residual flags after a full screen clear.
    /// Element-level invalidation is now handled by g_elementCaches.invalidateAll()
    /// called from prepareFullRedrawNoClear() alongside this function.
    void setAll() {
        obdIndicator = true;   // Still read externally for flush routing
        alpIndicator = true;
    }
};

// Single shared instance — defined in display.cpp, available to extracted
// display sub-modules (display_arrow.cpp, etc.)
extern DisplayDirtyFlags dirty;
