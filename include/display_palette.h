#pragma once

// ============================================================================
// Display Palette — colour helpers shared by display sub-modules
//
// Provides the current theme palette and user-configurable colours so that
// drawing code extracted from display.cpp (e.g. display_arrow.cpp) can
// resolve colours without depending on display.cpp-local state.
// ============================================================================

#include "color_themes.h"  // ColorPalette, ColorThemes

// Forward-declare V1Display (full definition not needed for the pointer)
class V1Display;

// Global display instance pointer — set by V1Display constructor, defined in
// display.cpp.  Used to reach the active colour palette and persisted-mode flag.
extern V1Display* g_displayInstance;

// Resolve the current colour palette (falls back to STANDARD if no instance).
// NOTE: must be defined after V1Display::getCurrentPalette() is visible.  In
// display.cpp that's already the case; extracted .cpp files include display.h
// before this header so the full class definition is available.
inline const ColorPalette& getColorPalette() {
    if (g_displayInstance) {
        return g_displayInstance->getCurrentPalette();
    }
    return ColorThemes::STANDARD();
}

// Convenience macros — evaluate to the live palette / user colours.
#define PALETTE_BG        getColorPalette().bg
#define PALETTE_TEXT      getColorPalette().text
#define PALETTE_GRAY      getColorPalette().colorGray
#define PALETTE_MUTED     getColorPalette().colorMuted
#define PALETTE_PERSISTED getColorPalette().colorPersisted

// Returns PALETTE_PERSISTED when in persisted-alert mode, else PALETTE_MUTED.
#define PALETTE_MUTED_OR_PERSISTED \
    (g_displayInstance && g_displayInstance->isPersistedMode() \
     ? PALETTE_PERSISTED : PALETTE_MUTED)
