/**
 * Direction Arrow Renderer — extracted from display.cpp (Phase 2I)
 *
 * Draws the stylised front / side / rear direction arrow triplet on the
 * right side of the display, with blink animation and per-arrow caching.
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "settings.h"
#include "packet_parser.h"   // Direction enum, DIR_FRONT/SIDE/REAR

// Draw large direction arrow (t4s3 style)
// flashBits indicates which arrows should blink (from image1 & ~image2)
void V1Display::drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits, uint16_t frontColorOverride) {
    // Cache to avoid redrawing unchanged arrows
    static bool lastShowFront = false;
    static bool lastShowSide = false;
    static bool lastShowRear = false;
    static bool lastMuted = false;
    static uint16_t lastFrontCol = 0;
    static uint16_t lastSideCol = 0;
    static uint16_t lastRearCol = 0;
    static bool cacheValid = false;
    
    // Check for forced invalidation (after screen clear)
    if (dirty.arrow) {
        cacheValid = false;
        dirty.arrow = false;
    }
    
    // Local blink timer - V1 blinks at ~5Hz, we match that
    static unsigned long lastBlinkTime = 0;
    static bool blinkOn = true;
    const unsigned long BLINK_INTERVAL_MS = 100;  // ~5Hz blink rate
    
    unsigned long now = millis();
    if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
        blinkOn = !blinkOn;
        lastBlinkTime = now;
    }
    
    // Determine which arrows to actually show
    // If an arrow is in flashBits and blink is OFF, hide it
    bool showFront = (dir & DIR_FRONT) != 0;
    bool showSide = (dir & DIR_SIDE) != 0;
    bool showRear = (dir & DIR_REAR) != 0;
    
    // Apply blink: if flashing bit is set and we're in OFF phase, hide that arrow
    if (!blinkOn) {
        if (flashBits & 0x20) showFront = false;  // Front flash bit
        if (flashBits & 0x40) showSide = false;   // Side flash bit
        if (flashBits & 0x80) showRear = false;   // Rear flash bit
    }
    
    // Stylized stacked arrows sized/positioned to match the real V1 display
    int cx = SCREEN_WIDTH - 70;           // right anchor
    int cy = SCREEN_HEIGHT / 2;           // vertically centered

#if defined(DISPLAY_WAVESHARE_349)
    // Position arrows to fit ABOVE frequency display at bottom
    // With multi-alert always enabled, use raised layout as default
    if (dirty.multiAlert) {
        cy = 85;  // Raised but allow full-size arrows
        cx -= 6;
    } else {
        cy = 95;
        cx -= 6;
    }
#endif
    
    // Use slightly smaller arrows to give profile indicator more room
    float scale = 0.98f;
    
    // Top arrow (FRONT): Taller triangle pointing up - matches V1 proportions
    // Wider/shallower angle to match V1 reference
    const int topW = (int)(125 * scale);      // Width at base
    const int topH = (int)(62 * scale);       // Height
    const int topNotchW = (int)(63 * scale);  // Notch width at bottom
    const int topNotchH = (int)(8 * scale);   // Notch height

    // Bottom arrow (REAR): Shorter/squatter triangle pointing down
    const int bottomW = (int)(125 * scale);   // Same width as top
    const int bottomH = (int)(40 * scale);    // Shorter height
    const int bottomNotchW = (int)(63 * scale);
    const int bottomNotchH = (int)(8 * scale);

    // Calculate positions for equal gaps between arrows
    const int sideBarH = (int)(22 * scale);
    const int gap = (int)(13 * scale);  // gap between arrows
    
    // Top arrow center: above side arrow with gap
    int topArrowCenterY = cy - sideBarH/2 - gap - topH/2;
    // Bottom arrow center: below side arrow with gap  
    int bottomArrowCenterY = cy + sideBarH/2 + gap + bottomH/2;

    const V1Settings& s = settingsManager.get();
    // Get individual arrow colors (use muted color if muted)
    uint16_t frontCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowFront;
    if (!muted && frontColorOverride != 0) {
        frontCol = frontColorOverride;
    }
    uint16_t sideCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowSide;
    uint16_t rearCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowRear;
    uint16_t offCol = 0x1082;  // Very dark grey for inactive arrows (matches PALETTE_GRAY)

    // Check if anything changed - if so, redraw ALL arrows to avoid clearing overlap issues
    bool anyChanged = !cacheValid || 
                      (showFront != lastShowFront) || 
                      (showSide != lastShowSide) || 
                      (showRear != lastShowRear) ||
                      (muted != lastMuted) ||
                      (frontCol != lastFrontCol) ||
                      (sideCol != lastSideCol) ||
                      (rearCol != lastRearCol);
    
    // If nothing changed, skip redraw entirely
    if (!anyChanged) {
        return;
    }
    
    // Calculate clear regions for each arrow
    const int maxW = (topW > bottomW) ? topW : bottomW;
    int clearLeft = cx - maxW/2 - 10;
    int clearWidth = maxW + 20;
    int maxClearRight = SCREEN_WIDTH - 42;
    if (clearLeft + clearWidth > maxClearRight) {
        clearWidth = maxClearRight - clearLeft;
    }

    auto drawTriangleArrow = [&](int centerY, bool down, bool active, int triW, int triH, int notchW, int notchH, uint16_t activeCol, bool needsClear) {
        // Clear just this arrow's region if needed
        if (needsClear) {
            int arrowTop = centerY - triH/2 - 2;
            int arrowHeight = triH + 4;
            FILL_RECT(clearLeft, arrowTop, clearWidth, arrowHeight, PALETTE_BG);
        }
        
        uint16_t fillCol = active ? activeCol : offCol;
        uint16_t outlineCol = TFT_BLACK;  // Black outline like V1
        
        // Triangle points
        int tipX = cx;
        int tipY = centerY + (down ? triH / 2 : -triH / 2);
        int baseLeftX = cx - triW / 2;
        int baseRightX = cx + triW / 2;
        int baseY = centerY + (down ? -triH / 2 : triH / 2);

        // Fill the main triangle
        FILL_TRIANGLE(tipX, tipY, baseLeftX, baseY, baseRightX, baseY, fillCol);

        // Notch cutout at the base (opposite of tip)
        int notchY = down ? (baseY - notchH) : baseY;
        FILL_RECT(cx - notchW / 2, notchY, notchW, notchH, fillCol);
        
        // Draw outline - triangle edges
        DRAW_LINE(tipX, tipY, baseLeftX, baseY, outlineCol);
        DRAW_LINE(tipX, tipY, baseRightX, baseY, outlineCol);
        // Base line with notch gap
        DRAW_LINE(baseLeftX, baseY, cx - notchW/2, baseY, outlineCol);
        DRAW_LINE(cx + notchW/2, baseY, baseRightX, baseY, outlineCol);
        // Notch outline
        if (down) {
            DRAW_LINE(cx - notchW/2, baseY, cx - notchW/2, baseY - notchH, outlineCol);
            DRAW_LINE(cx - notchW/2, baseY - notchH, cx + notchW/2, baseY - notchH, outlineCol);
            DRAW_LINE(cx + notchW/2, baseY - notchH, cx + notchW/2, baseY, outlineCol);
        } else {
            DRAW_LINE(cx - notchW/2, baseY, cx - notchW/2, baseY + notchH, outlineCol);
            DRAW_LINE(cx - notchW/2, baseY + notchH, cx + notchW/2, baseY + notchH, outlineCol);
            DRAW_LINE(cx + notchW/2, baseY + notchH, cx + notchW/2, baseY, outlineCol);
        }
    };

    auto drawSideArrow = [&](bool active, bool needsClear) {
        // Clear just the side arrow region if needed
        if (needsClear) {
            [[maybe_unused]] const int headW = (int)(28 * scale);
            [[maybe_unused]] const int headH = (int)(22 * scale);
            int sideTop = cy - headH - 2;
            int sideHeight = headH * 2 + 4;
            FILL_RECT(clearLeft, sideTop, clearWidth, sideHeight, PALETTE_BG);
        }
        
        uint16_t fillCol = active ? sideCol : offCol;
        uint16_t outlineCol = TFT_BLACK;  // Black outline like V1
        const int barW = (int)(66 * scale);   // Center bar width
        const int barH = sideBarH;
        [[maybe_unused]] const int headW = (int)(28 * scale);  // Arrow head width
        [[maybe_unused]] const int headH = (int)(22 * scale);  // Arrow head height
        const int halfH = barH / 2;

        // Fill center bar
        FILL_RECT(cx - barW / 2, cy - halfH, barW, barH, fillCol);
        
        // Fill left arrow head
        FILL_TRIANGLE(cx - barW / 2 - headW, cy, cx - barW / 2, cy - headH, cx - barW / 2, cy + headH, fillCol);
        // Fill right arrow head
        FILL_TRIANGLE(cx + barW / 2 + headW, cy, cx + barW / 2, cy - headH, cx + barW / 2, cy + headH, fillCol);
        
        // Outline - top edge
        DRAW_LINE(cx - barW/2, cy - halfH, cx + barW/2, cy - halfH, outlineCol);
        // Outline - bottom edge
        DRAW_LINE(cx - barW/2, cy + halfH, cx + barW/2, cy + halfH, outlineCol);
        // Outline - left arrow head
        DRAW_LINE(cx - barW/2, cy - headH, cx - barW/2 - headW, cy, outlineCol);
        DRAW_LINE(cx - barW/2 - headW, cy, cx - barW/2, cy + headH, outlineCol);
        // Outline - right arrow head
        DRAW_LINE(cx + barW/2, cy - headH, cx + barW/2 + headW, cy, outlineCol);
        DRAW_LINE(cx + barW/2 + headW, cy, cx + barW/2, cy + headH, outlineCol);
    };

    // Clear entire arrow region once, then redraw all
    [[maybe_unused]] const int headH = (int)(22 * scale);
    int totalTop = topArrowCenterY - topH/2 - 2;
    int totalBottom = bottomArrowCenterY + bottomH/2 + 2;
    FILL_RECT(clearLeft, totalTop, clearWidth, totalBottom - totalTop, PALETTE_BG);
    
    // Draw all three arrows
    drawTriangleArrow(topArrowCenterY, false, showFront, topW, topH, topNotchW, topNotchH, frontCol, false);
    drawSideArrow(showSide, false);
    drawTriangleArrow(bottomArrowCenterY, true, showRear, bottomW, bottomH, bottomNotchW, bottomNotchH, rearCol, false);
    
    // Update cache
    lastShowFront = showFront;
    lastShowSide = showSide;
    lastShowRear = showRear;
    lastMuted = muted;
    lastFrontCol = frontCol;
    lastSideCol = sideCol;
    lastRearCol = rearCol;
    cacheValid = true;
}
