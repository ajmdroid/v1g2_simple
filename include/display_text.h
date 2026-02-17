#pragma once

// ============================================================================
// Display Text Drawing Helpers — cross-platform text rendering abstraction
//
// Arduino_GFX has no setTextDatum()/drawString(). These helpers emulate the
// TFT_eSPI text-alignment API so callers are platform-agnostic.
//
// REQUIREMENT: `tft` (Arduino_Canvas* or TFT_eSPI&) must be in scope at
// every call site, same as the drawing macros in display_draw.h.
// ============================================================================

#include "display_driver.h"  // DISPLAY_USE_ARDUINO_GFX, Arduino_Canvas, etc.

#if defined(DISPLAY_USE_ARDUINO_GFX)

// Shared datum state (file-scope — set by GFX_setTextDatum, read by GFX_drawString)
inline uint8_t& gfxCurrentTextDatum() {
    static uint8_t datum = TL_DATUM;
    return datum;
}

#define GFX_setTextDatum(d) do { gfxCurrentTextDatum() = (d); } while(0)

// Arduino_GFX implementation of drawString with datum support.
// Uses getTextBounds() to compute alignment offsets.
inline void GFX_drawString(Arduino_Canvas* canvas, const char* str, int16_t x, int16_t y) {
    int16_t x1, y1;
    uint16_t w, h;
    canvas->getTextBounds(str, 0, 0, &x1, &y1, &w, &h);

    int16_t drawX = x, drawY = y;
    const uint8_t datum = gfxCurrentTextDatum();

    // Horizontal alignment
    switch (datum) {
        case TC_DATUM: case MC_DATUM: case BC_DATUM:
            drawX = x - x1 - w / 2;
            break;
        case TR_DATUM: case MR_DATUM: case BR_DATUM:
            drawX = x - x1 - w;
            break;
        default:  // TL, ML, BL — left aligned
            drawX = x - x1;
            break;
    }

    // Vertical alignment
    switch (datum) {
        case ML_DATUM: case MC_DATUM: case MR_DATUM:
            drawY = y - y1 - h / 2;
            break;
        case BL_DATUM: case BC_DATUM: case BR_DATUM:
            drawY = y - y1 - h;
            break;
        default:  // TL, TC, TR — top aligned
            drawY = y - y1;
            break;
    }

    canvas->setCursor(drawX, drawY);
    canvas->print(str);
}

#else  // TFT_eSPI — native methods

#define GFX_setTextDatum(d) tft.setTextDatum(d)
inline void GFX_drawString(TFT_eSPI& canvas, const char* str, int16_t x, int16_t y) {
    canvas.drawString(str, x, y);
}

#endif
