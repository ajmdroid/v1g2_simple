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
#include "../include/display_element_caches.h"
#include "../include/display_palette.h"
#include "settings.h"
#include "packet_parser.h"   // Direction enum, DIR_FRONT/SIDE/REAR

// File-scoped blink timer for drawDirectionArrow
static unsigned long s_arrowLastBlinkTime = 0;
static bool s_arrowBlinkOn = true;

// Draw large direction arrow (t4s3 style)
// flashBits indicates which arrows should blink (from image1 & ~image2)
void V1Display::drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits, uint16_t frontColorOverride) {
    const bool forceFullRedraw = !g_elementCaches.arrow.valid;

    // Local blink timer - V1 blinks at ~5Hz, we match that
    const unsigned long BLINK_INTERVAL_MS = 100;  // ~5Hz blink rate

    unsigned long now = millis();
    if (now - s_arrowLastBlinkTime >= BLINK_INTERVAL_MS) {
        s_arrowBlinkOn = !s_arrowBlinkOn;
        s_arrowLastBlinkTime = now;
    }

    // Determine which arrows to actually show
    // If an arrow is in flashBits and blink is OFF, hide it
    bool showFront = (dir & DIR_FRONT) != 0;
    bool showSide = (dir & DIR_SIDE) != 0;
    bool showRear = (dir & DIR_REAR) != 0;

    // Apply blink: if flashing bit is set and we're in OFF phase, hide that arrow
    if (!s_arrowBlinkOn) {
        if (flashBits & 0x20) showFront = false;  // Front flash bit
        if (flashBits & 0x40) showSide = false;   // Side flash bit
        if (flashBits & 0x80) showRear = false;   // Rear flash bit
    }

    // Stylized stacked arrows sized/positioned to match the real V1 display
    int cx = SCREEN_WIDTH - 70;           // right anchor
    int cy = SCREEN_HEIGHT / 2;           // vertically centered

    // Position arrows to fit ABOVE frequency display at bottom
    // With multi-alert always enabled, use raised layout as default
    const bool raisedLayout = dirty.multiAlert;
    if (raisedLayout) {
        cy = 94;  // Raised but allow full-size arrows
        cx -= 6;
    } else {
        cy = 104;
        cx -= 6;
    }

    // Use slightly smaller arrows to give profile indicator more room
    float scale = 0.98f;

    // Top arrow (FRONT): Taller triangle pointing up - matches V1 proportions
    // Wider/shallower angle to match V1 reference
    const int topW = (int)(125 * scale);      // Width at base
    const int topH = (int)(70 * scale);       // Height
    const int topNotchW = (int)(63 * scale);  // Notch width at bottom
    const int topNotchH = (int)(8 * scale);   // Notch height

    // Bottom arrow (REAR): Shorter/squatter triangle pointing down
    const int bottomW = (int)(125 * scale);   // Same width as top
    const int bottomH = (int)(30 * scale);    // Shorter height
    const int bottomNotchW = (int)(63 * scale);
    const int bottomNotchH = (int)(8 * scale);

    // Calculate positions for equal gaps between arrows
    const int sideBarH = (int)(22 * scale);
    const int gap = (int)(15 * scale);  // gap between arrows

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
    uint16_t offCol = TFT_DARKGREY;  // Very dark grey for inactive arrows (matches PALETTE_GRAY)

    const bool frontVisibilityChanged = g_elementCaches.arrow.valid && (showFront != g_elementCaches.arrow.showFront);
    const bool sideVisibilityChanged = g_elementCaches.arrow.valid && (showSide != g_elementCaches.arrow.showSide);
    const bool rearVisibilityChanged = g_elementCaches.arrow.valid && (showRear != g_elementCaches.arrow.showRear);
    const bool mutedChanged = g_elementCaches.arrow.valid && (muted != g_elementCaches.arrow.muted);
    const bool colorsChanged = g_elementCaches.arrow.valid &&
                               ((frontCol != g_elementCaches.arrow.frontCol) ||
                                (sideCol != g_elementCaches.arrow.sideCol) ||
                                (rearCol != g_elementCaches.arrow.rearCol));
    const bool layoutChanged = g_elementCaches.arrow.valid && (raisedLayout != g_elementCaches.arrow.raisedLayout);
    const int visibilityChangeCount = static_cast<int>(frontVisibilityChanged) +
                                      static_cast<int>(sideVisibilityChanged) +
                                      static_cast<int>(rearVisibilityChanged);
    bool anyChanged = !g_elementCaches.arrow.valid ||
                      frontVisibilityChanged ||
                      sideVisibilityChanged ||
                      rearVisibilityChanged ||
                      mutedChanged ||
                      colorsChanged ||
                      layoutChanged;

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

    const bool canTargetSingleArrow = !forceFullRedraw &&
                                      g_elementCaches.arrow.valid &&
                                      !mutedChanged &&
                                      !colorsChanged &&
                                      !layoutChanged &&
                                      visibilityChangeCount == 1;

    if (canTargetSingleArrow) {
        if (frontVisibilityChanged) {
            drawTriangleArrow(topArrowCenterY,
                              false,
                              showFront,
                              topW,
                              topH,
                              topNotchW,
                              topNotchH,
                              frontCol,
                              true);
        } else if (sideVisibilityChanged) {
            drawSideArrow(showSide, true);
        } else {
            drawTriangleArrow(bottomArrowCenterY,
                              true,
                              showRear,
                              bottomW,
                              bottomH,
                              bottomNotchW,
                              bottomNotchH,
                              rearCol,
                              true);
        }
    } else {
        // Clear entire arrow region once, then redraw all
        [[maybe_unused]] const int headH = (int)(22 * scale);
        int totalTop = topArrowCenterY - topH/2 - 2;
        int totalBottom = bottomArrowCenterY + bottomH/2 + 2;
        FILL_RECT(clearLeft, totalTop, clearWidth, totalBottom - totalTop, PALETTE_BG);

        // Draw all three arrows
        drawTriangleArrow(topArrowCenterY,
                          false,
                          showFront,
                          topW,
                          topH,
                          topNotchW,
                          topNotchH,
                          frontCol,
                          false);
        drawSideArrow(showSide, false);
        drawTriangleArrow(bottomArrowCenterY,
                          true,
                          showRear,
                          bottomW,
                          bottomH,
                          bottomNotchW,
                          bottomNotchH,
                          rearCol,
                          false);
    }

    // Update cache
    g_elementCaches.arrow.showFront = showFront;
    g_elementCaches.arrow.showSide = showSide;
    g_elementCaches.arrow.showRear = showRear;
    g_elementCaches.arrow.muted = muted;
    g_elementCaches.arrow.frontCol = frontCol;
    g_elementCaches.arrow.sideCol = sideCol;
    g_elementCaches.arrow.rearCol = rearCol;
    g_elementCaches.arrow.raisedLayout = raisedLayout;
    g_elementCaches.arrow.valid = true;
}
