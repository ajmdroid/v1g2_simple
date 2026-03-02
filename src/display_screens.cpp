/**
 * Screen-mode renderers — extracted from display.cpp (Phase 3A)
 *
 * Contains showDisconnected, showResting, showScanning, showDemo,
 * showBootSplash, showShutdown, showLowBattery, forceNextRedraw,
 * and resetChangeTracking.
 */

#include "display.h"
#include "../include/config.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "../include/display_segments.h"
#include "../include/display_log.h"
#include "../include/display_flush.h"
#include "display_font_manager.h"
#include "v1simple_logo.h"
#include "settings.h"
#include "perf_metrics.h"

using namespace DisplaySegments;
using DisplayLayout::PRIMARY_ZONE_HEIGHT;

// ============================================================================
// showDisconnected
// ============================================================================

void V1Display::showDisconnected() {
    drawBaseFrame();
    drawStatusText("Disconnected", 0xF800);  // Red
    drawWiFiIndicator();
    drawBatteryIndicator();
}

// ============================================================================
// showResting
// ============================================================================

void V1Display::showResting(bool forceRedraw) {
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    multiAlertMode = false;

    // Save the last known bogey counter before potentially resetting
    // This preserves the mode indicator (A/L/c) when V1 is connected
    char savedBogeyChar = lastState.bogeyCounterChar;
    bool savedBogeyDot = lastState.bogeyCounterDot;
    
    // Avoid redundant full-screen clears/flushes when already resting and nothing changed
    bool paletteChanged = (lastRestingPaletteRevision != paletteRevision);
    bool screenChanged = (currentScreen != ScreenMode::Resting);
    int profileSlot = currentProfileSlot;
    bool profileChanged = (profileSlot != lastRestingProfileSlot);
    
    if (forceRedraw || screenChanged || paletteChanged) {
        // Full redraw when forced, coming from another screen, or after theme change
        TFT_CALL(fillScreen)(PALETTE_BG);
        drawBaseFrame();
        
        // Draw idle state: if V1 is connected, show last known mode; otherwise show "0"
        char topChar = '0';
        bool topDot = true;
        if (bleCtx_.v1Connected && savedBogeyChar != 0) {
            topChar = savedBogeyChar;
            topDot = savedBogeyDot;
        }
        drawTopCounter(topChar, false, topDot);
        // Volume indicator not shown in resting state (no DisplayState available)
        
        // Band indicators all dimmed (no active bands)
        drawBandIndicators(0, false);
        
        // Signal bars all empty
        drawVerticalSignalBars(0, 0, BAND_KA, false);
        
        // Direction arrows all dimmed
        drawDirectionArrow(DIR_NONE, false);
        
        // Frequency display
        drawFrequency(0, BAND_NONE);
        
        // Mute indicator off
        drawMuteIcon(false);
        drawLockoutIndicator();
        drawGpsIndicator();
        
        // Profile indicator
        drawProfileIndicator(profileSlot);
        
        // Reset secondary alert card state, then draw resting telemetry cards.
        AlertData emptyPriority;
        drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);
        drawRestTelemetryCards(true);

        lastRestingPaletteRevision = paletteRevision;
        lastRestingProfileSlot = profileSlot;
        
        // Log screen mode transition for debugging display refresh issues
        if (currentScreen != ScreenMode::Resting) {
            DISPLAY_LOG("[DISP] Screen mode: %d -> Resting (showResting)\n", (int)currentScreen);
            perfRecordDisplayScreenTransition(
                static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
                PerfDisplayScreen::Resting,
                millis());
        }
        currentScreen = ScreenMode::Resting;

    DISPLAY_FLUSH();
    } else if (profileChanged) {
        // Only the profile changed while already resting; redraw just the indicator
        drawProfileIndicator(profileSlot);
        lastRestingProfileSlot = profileSlot;
        // Push only the regions touched by profile/WiFi/BLE/battery indicators
        const int profileFlushY = 8;
        const int profileFlushH = 36;
        flushRegion(100, profileFlushY, SCREEN_WIDTH - 160, profileFlushH);

        const int leftColWidth = 64;
        const int leftColHeight = 96;
        flushRegion(0, SCREEN_HEIGHT - leftColHeight, leftColWidth, leftColHeight);
    }

    // Reset lastState so next update() detects changes from this "resting" state
    lastState = DisplayState();  // All defaults: bands=0, arrows=0, bars=0, hasMode=false, modeChar=0
}

// ============================================================================
// forceNextRedraw / resetChangeTracking
// ============================================================================

void V1Display::forceNextRedraw() {
    // Reset lastState to force next update() to detect all changes and redraw
    lastState = DisplayState();
    // Set screen mode to Unknown so any next update/showResting detects a screen change
    currentScreen = ScreenMode::Unknown;
    // Reset all static change tracking variables (volume, mode, arrows, etc.)
    // This ensures the next update() does a full redraw with fresh data
    resetChangeTracking();
}

void V1Display::resetChangeTracking() {
    dirty.resetTracking = true;
}

// ============================================================================
// showScanning
// ============================================================================

void V1Display::showScanning() {
    // Always use multi-alert layout positioning
    dirty.multiAlert = true;
    
    // Get settings for display style
    const V1Settings& s = settingsManager.get();

    // Clear and draw the base frame
    TFT_CALL(fillScreen)(PALETTE_BG);
    drawBaseFrame();
    
    // Draw idle state elements
    drawTopCounter('0', false, true);
    // Volume indicator not shown in scanning state (no DisplayState available)
    drawBandIndicators(0, false);
    drawVerticalSignalBars(0, 0, BAND_KA, false);
    drawDirectionArrow(DIR_NONE, false);
    drawMuteIcon(false);
    drawLockoutIndicator();
    drawGpsIndicator();
    drawProfileIndicator(currentProfileSlot);
    
    // Draw "SCAN" in frequency area - match display style
    if (s.displayStyle == DISPLAY_STYLE_SERPENTINE) {
        fontMgr.ensureSerpentineLoaded(tft);
    }
    if (s.displayStyle == DISPLAY_STYLE_SERPENTINE && fontMgr.serpentineReady) {
        // Serpentine style font
        const int fontSize = 65;
        fontMgr.serpentine.setFontColor(s.colorBandKa, PALETTE_BG);
        fontMgr.serpentine.setFontSize(fontSize);
        
        const char* text = "SCAN";
        FT_BBox bbox = fontMgr.serpentine.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
        int textWidth = bbox.xMax - bbox.xMin;
        int textHeight = bbox.yMax - bbox.yMin;
        
        const int leftMargin = 120;
        const int rightMargin = 200;
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - textWidth) / 2;
        int y = getEffectiveScreenHeight() - 72;
        
        FILL_RECT(x - 4, y - textHeight - 4, textWidth + 8, textHeight + 12, PALETTE_BG);
        fontMgr.serpentine.setCursor(x, y);
        fontMgr.serpentine.printf("%s", text);
    } else if (fontMgr.segment7Ready) {
        // Classic style: use Segment7 TTF font
        const int fontSize = 65;
        const int leftMargin = 135;  // Match frequency positioning
        const int rightMargin = 200;
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 8;
        
        const char* text = "SCAN";
        int approxWidth = 4 * 32;  // 4 chars ~32px each
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - approxWidth) / 2;
        
        FILL_RECT(x - 5, y - 5, approxWidth + 10, fontSize + 10, PALETTE_BG);
        
        // Convert color for OpenFontRender
        uint8_t bgR = (PALETTE_BG >> 11) << 3;
        uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        fontMgr.segment7.setBackgroundColor(bgR, bgG, bgB);
        fontMgr.segment7.setFontSize(fontSize);
        fontMgr.segment7.setFontColor((s.colorBandKa >> 11) << 3, ((s.colorBandKa >> 5) & 0x3F) << 2, (s.colorBandKa & 0x1F) << 3);
        fontMgr.segment7.setCursor(x, y);
        fontMgr.segment7.printf("%s", text);
    } else {
        // Fallback: software 14-segment display
        const float scale = 2.3f;  // Match frequency scale
        SegMetrics m = segMetrics(scale);
        
        // Position to match frequency display (centered between mute area and bottom)
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - m.digitH) / 2 + 5;
        
        const char* text = "SCAN";
        int width = measureSevenSegmentText(text, scale);  // Same measurement for 14-seg
        
        // Center between band indicators and signal bars
        const int leftMargin = 120;   // After band indicators
        const int rightMargin = 200;  // Before signal bars
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - width) / 2;
        if (x < leftMargin) x = leftMargin;
        
        FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
        draw14SegmentText(text, x, y, scale, s.colorBandKa, PALETTE_BG);
    }
    
    // Reset lastState
    lastState = DisplayState();
    
    DISPLAY_FLUSH();

    if (currentScreen != ScreenMode::Scanning) {
        perfRecordDisplayScreenTransition(
            static_cast<PerfDisplayScreen>(static_cast<uint8_t>(currentScreen)),
            PerfDisplayScreen::Scanning,
            millis());
    }
    currentScreen = ScreenMode::Scanning;
    lastRestingProfileSlot = -1;
}

// ============================================================================
// showDemo
// ============================================================================

void V1Display::showDemo() {
    TFT_CALL(fillScreen)(PALETTE_BG); // Clear screen to prevent artifacts
    clear();

    // Show a MUTED K-band alert to demonstrate the muted color
    AlertData demoAlert;
    demoAlert.band = BAND_K;
    demoAlert.direction = DIR_FRONT;
    demoAlert.frontStrength = 4;
    demoAlert.rearStrength = 0;
    demoAlert.frequency = 24150;  // MHz (24.150 GHz)
    demoAlert.isValid = true;

    // Create a demo display state
    DisplayState demoState;
    demoState.activeBands = BAND_K;
    demoState.arrows = DIR_FRONT;
    demoState.signalBars = 4;
    demoState.muted = true;

    // Draw the alert in MUTED state using multi-alert display
    update(demoAlert, &demoAlert, 1, demoState);
    lastState.signalBars = 1;
    
    // Also draw profile indicator and WiFi icon during demo so user can see hide toggle effect
    drawProfileIndicator(0);  // Show slot 0 profile indicator (unless hidden)
    drawWiFiIndicator();      // Show WiFi icon (unless hidden)
    
    // Flush to display
    flush();
}

// ============================================================================
// showBootSplash
// ============================================================================

void V1Display::showBootSplash() {
    const unsigned long splashStartMs = millis();
    TFT_CALL(fillScreen)(PALETTE_BG); // Clear screen to prevent artifacts
    drawBaseFrame();

    // Draw the V1 Simple logo at 1:1 (image is pre-sized to 640x172)
    // Use row-level bulk blit on Arduino_GFX to reduce draw call overhead.
    const unsigned long logoStartMs = millis();
    uint16_t rowBuffer[V1SIMPLE_LOGO_WIDTH];
    for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
        const int rowOffset = sy * V1SIMPLE_LOGO_WIDTH;
        for (int sx = 0; sx < V1SIMPLE_LOGO_WIDTH; sx++) {
            rowBuffer[sx] = pgm_read_word(&v1simple_logo_rgb565[rowOffset + sx]);
        }
        TFT_CALL(draw16bitRGBBitmap)(0, sy, rowBuffer, V1SIMPLE_LOGO_WIDTH, 1);
    }
    const unsigned long logoMs = millis() - logoStartMs;
    
    // Draw version number in bottom-right corner
    GFX_setTextDatum(BR_DATUM);  // Bottom-right alignment
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x7BEF, PALETTE_BG);  // Gray text (mid-gray RGB565)
    GFX_drawString(tft, "v" FIRMWARE_VERSION, SCREEN_WIDTH - 8, SCREEN_HEIGHT - 6);

    // Flush canvas to display before enabling backlight
    const unsigned long flushStartMs = millis();
    DISPLAY_FLUSH();
    const unsigned long flushMs = millis() - flushStartMs;

    // Turn on backlight now that splash is drawn
    // Waveshare 3.49" has INVERTED backlight: 0=full on, 255=off
    analogWrite(LCD_BL, 0);  // Full brightness (inverted)
    Serial.println("Backlight ON (post-splash, inverted)");
    Serial.printf("[BootTiming] splash total=%lu logo=%lu flush=%lu\n",
                  millis() - splashStartMs,
                  logoMs,
                  flushMs);
}

// ============================================================================
// showShutdown
// ============================================================================

void V1Display::showShutdown() {
    // Clear screen
    TFT_CALL(fillScreen)(PALETTE_BG);
    
    // Draw "GOODBYE" message centered
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(3);
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    GFX_drawString(tft, "GOODBYE", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 - 20);
    
    // Draw smaller "Powering off..." below
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(PALETTE_GRAY, PALETTE_BG);
    GFX_drawString(tft, "Powering off...", SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2 + 20);
    
    // Flush to display
    DISPLAY_FLUSH();
}

// ============================================================================
// showLowBattery
// ============================================================================

void V1Display::showLowBattery() {
    // Clear screen
    TFT_CALL(fillScreen)(PALETTE_BG);
    
    // Draw large battery outline in center
    const int battW = 120;
    const int battH = 60;
    const int battX = (SCREEN_WIDTH - battW) / 2;
    const int battY = (SCREEN_HEIGHT - battH) / 2 - 20;
    const int capW = 12;
    const int capH = 24;
    
    // Draw battery outline in red
    uint16_t redColor = 0xF800;
    DRAW_RECT(battX, battY, battW, battH, redColor);
    FILL_RECT(battX + battW, battY + (battH - capH) / 2, capW, capH, redColor);
    
    // Draw single bar (low)
    const int padding = 8;
    FILL_RECT(battX + padding, battY + padding, 20, battH - 2 * padding, redColor);
    
    // Draw "LOW BATTERY" text below
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(redColor, PALETTE_BG);
    GFX_drawString(tft, "LOW BATTERY", SCREEN_WIDTH / 2, battY + battH + 30);
    
    // Flush to display
    DISPLAY_FLUSH();
}
