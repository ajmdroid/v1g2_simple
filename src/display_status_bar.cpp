/**
 * Status-bar / peripheral indicators — extracted from display.cpp (Phase 2N)
 *
 * Contains drawVolumeIndicator, drawRssiIndicator, drawProfileIndicator,
 * drawBatteryIndicator, drawBLEProxyIndicator, and drawWiFiIndicator.
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_draw.h"
#include "../include/display_dirty_flags.h"
#include "../include/display_palette.h"
#include "../include/display_text.h"
#include "../include/display_segments.h"  // SegMetrics, segMetrics() for non-349 profile path
#include "display_font_manager.h"
#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
#include <cstring>

using namespace DisplaySegments;

// ============================================================================
// Volume indicator
// ============================================================================

void V1Display::drawVolumeIndicator(uint8_t mainVol, uint8_t muteVol) {
    // Draw volume indicator below bogey counter: "5V  0M" format
    const V1Settings& s = settingsManager.get();
    const int x = 8;
    // Evenly spaced layout from bogey(67) to bottom(172)
    const int y = 75;
    const int clearW = 75;
    const int clearH = 16;
    
    // Clear the area first - only clear what we need, BLE icon is at y=98
    FILL_RECT(x, y, clearW, clearH, PALETTE_BG);
    
    // Draw main volume in blue, mute volume in yellow (user-configurable colors)
    GFX_setTextDatum(TL_DATUM);  // Top-left alignment
    TFT_CALL(setTextSize)(2);  // Size 2 = ~16px height
    
    // Draw main volume "5V" in main volume color
    char mainBuf[5];  // allow up to three digits plus suffix and null
    snprintf(mainBuf, sizeof(mainBuf), "%dV", mainVol);
    TFT_CALL(setTextColor)(s.colorVolumeMain, PALETTE_BG);
    GFX_drawString(tft, mainBuf, x, y);
    
    // Draw mute volume "0M" in mute volume color, offset to the right
    char muteBuf[5];  // allow up to three digits plus suffix and null
    snprintf(muteBuf, sizeof(muteBuf), "%dM", muteVol);
    TFT_CALL(setTextColor)(s.colorVolumeMute, PALETTE_BG);
    GFX_drawString(tft, muteBuf, x + 36, y);  // Aligned with RSSI number
}

// ============================================================================
// RSSI indicator
// ============================================================================

void V1Display::drawRssiIndicator(int rssi) {
    // Draw BLE RSSI below volume indicator
    // Shows V1 RSSI and app RSSI (if connected) stacked vertically
    const int x = 8;
    // Evenly spaced: volume at y=75, height 16, gap 8 -> y=99
    const int y = 99;
    const int lineHeight = 22;  // Increased spacing between V and P lines
    const int clearW = 70;
    const int clearH = lineHeight * 2;  // Room for two lines

    // Check if RSSI indicator is hidden
    const V1Settings& s = settingsManager.get();
    if (s.hideRssiIndicator || s.hideVolumeIndicator) {
        FILL_RECT(x, y, clearW, clearH, PALETTE_BG);
        return;  // Don't draw anything
    }

    if (!hasFreshBleContext(millis())) {
        FILL_RECT(x, y, clearW, clearH, PALETTE_BG);
        return;
    }

    // Clear the area first
    FILL_RECT(x, y, clearW, clearH, PALETTE_BG);
    
    // Get both RSSIs
    int v1Rssi = rssi;  // V1 RSSI passed in
    int appRssi = bleCtx_.proxyRssi;  // App RSSI
    
    GFX_setTextDatum(TL_DATUM);
    TFT_CALL(setTextSize)(2);  // Match volume text size
    
    // Draw V1 RSSI if connected
    if (v1Rssi != 0) {
        // Draw "V " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiV1, PALETTE_BG);
        GFX_drawString(tft, "V ", x, y);
        
        // Color code RSSI value: green >= -75, yellow -75 to -90, red < -90
        // Calibrated for ESP-IDF 5.3.x BLE controller on ESP32-S3 which reports
        // ~20-30 dB lower than ESP-IDF 4.4.x at the same physical distance.
        uint16_t rssiColor;
        if (v1Rssi >= -75) {
            rssiColor = COLOR_GREEN;
        } else if (v1Rssi >= -90) {
            rssiColor = COLOR_YELLOW;
        } else {
            rssiColor = COLOR_RED;
        }
        
        TFT_CALL(setTextColor)(rssiColor, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", v1Rssi);
        GFX_drawString(tft, buf, x + 24, y);  // Offset for "V " width
    }
    
    // Draw app RSSI below V1 RSSI if connected
    if (appRssi != 0) {
        // Draw "P " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiProxy, PALETTE_BG);
        GFX_drawString(tft, "P ", x, y + lineHeight);
        
        // Color code RSSI value (same calibration as V1 RSSI above)
        uint16_t rssiColor;
        if (appRssi >= -75) {
            rssiColor = COLOR_GREEN;
        } else if (appRssi >= -90) {
            rssiColor = COLOR_YELLOW;
        } else {
            rssiColor = COLOR_RED;
        }
        
        TFT_CALL(setTextColor)(rssiColor, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", appRssi);
        GFX_drawString(tft, buf, x + 24, y + lineHeight);  // Offset for "P " width
    }
}

// ============================================================================
// Profile indicator
// ============================================================================

void V1Display::setProfileIndicatorSlot(int slot) {
    if (slot != lastProfileSlot) {
        lastProfileSlot = slot;
        profileChangedTime = millis();
    }
    currentProfileSlot = slot;
}

void V1Display::drawProfileIndicator(int slot) {
    // Get custom slot names and colors from settings
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();

    setProfileIndicatorSlot(slot);

    // Check if we're in the "flash" period after a profile change
    bool inFlashPeriod = (millis() - profileChangedTime) < HIDE_TIMEOUT_MS;
    
#if defined(DISPLAY_WAVESHARE_349)
    // On Waveshare: draw profile indicator under arrows (for autopush profiles)
    // Arrow center X = 564, profile goes below rear arrow
    int cx = SCREEN_WIDTH - 70 - 6;  // Same as arrow cx
    int y = 152;  // Below arrows
    int clearW = 130;  // Wide enough for profile names at size 2
    int clearH = 20;   // Tall enough for size 2 font
    
    // If user explicitly hides the indicator via web UI, only show during flash period
    if (s.hideProfileIndicator && !inFlashPeriod) {
        FILL_RECT(cx - clearW/2, y, clearW, clearH, PALETTE_BG);
        drawWiFiIndicator();
        drawBatteryIndicator();
        return;
    }

    // Use custom names, fallback to defaults (limited to 20 chars)
    const char* name;
    uint16_t color;
    switch (slot % 3) {
        case 0:
            name = s.slot0Name.length() > 0 ? s.slot0Name.c_str() : "DEFAULT";
            color = s.slot0Color;
            break;
        case 1:
            name = s.slot1Name.length() > 0 ? s.slot1Name.c_str() : "HIGHWAY";
            color = s.slot1Color;
            break;
        default:  // case 2
            name = s.slot2Name.length() > 0 ? s.slot2Name.c_str() : "COMFORT";
            color = s.slot2Color;
            break;
    }

    // Clear area under arrows
    FILL_RECT(cx - clearW/2, y, clearW, clearH, PALETTE_BG);

    // Use built-in font (OFR font subset doesn't have all letters for profile names)
    TFT_CALL(setTextSize)(2);  // Size 2 = ~12px per char
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    int16_t nameWidth = strlen(name) * 12;  // size 2 = ~12px per char
    int textX = cx - nameWidth / 2;
    GFX_setTextDatum(TL_DATUM);
    GFX_drawString(tft, name, textX, y);

    drawWiFiIndicator();
    drawBatteryIndicator();
    setBLEProxyStatus(bleProxyEnabled, bleProxyClientConnected, bleReceivingData);
    
#else
    // Original position for smaller displays (top area)
    // If user explicitly hides the indicator via web UI, only show during flash period
    if (s.hideProfileIndicator && !inFlashPeriod) {
        int y = 14;
        int clearStart = 120;  // Don't overlap band indicators
        int clearWidth = SCREEN_WIDTH - clearStart - 240;  // stop before signal bars
        FILL_RECT(clearStart, y - 2, clearWidth, 28, PALETTE_BG);

        drawWiFiIndicator();
        drawBatteryIndicator();
        return;
    }

    // Dimensions for clearing/profile centering
    const float freqScale = 1.7f;
    SegMetrics mFreq = segMetrics(freqScale);
    int freqWidth = measureSevenSegmentText("35.500", freqScale);
    const int rightMargin = 120;
    int maxWidth = SCREEN_WIDTH - rightMargin;
    int freqX = (maxWidth - freqWidth) / 2;
    if (freqX < 0) freqX = 0;
    int dotCenterX = freqX + 2 * mFreq.digitW + 2 * mFreq.spacing + mFreq.dot / 2;

    // Use custom names, fallback to defaults (limited to 20 chars)
    const char* name;
    uint16_t color;
    switch (slot % 3) {
        case 0:
            name = s.slot0Name.length() > 0 ? s.slot0Name.c_str() : "DEFAULT";
            color = s.slot0Color;
            break;
        case 1:
            name = s.slot1Name.length() > 0 ? s.slot1Name.c_str() : "HIGHWAY";
            color = s.slot1Color;
            break;
        default:  // case 2
            name = s.slot2Name.length() > 0 ? s.slot2Name.c_str() : "COMFORT";
            color = s.slot2Color;
            break;
    }

    // Measure the profile name text width
    GFX_setTextDatum(TL_DATUM);  // Top-left
    TFT_CALL(setTextSize)(2);    // Larger text for readability
    int16_t nameWidth = strlen(name) * 12;  // Approximate: size 2 = ~12px per char

    // Center the name over the dot position
    int x = dotCenterX - nameWidth / 2;
    if (x < 120) x = 120;  // Don't overlap with band indicators on left (L/Ka/K/X)

    int y = 14;

    // Clear area for profile name - use fixed wide area to prevent artifacts
    int clearEndX = SCREEN_WIDTH - 240;  // Stop before signal bars (at ~412)
    FILL_RECT(120, y - 2, clearEndX - 120, 28, PALETTE_BG);

    // Draw the profile name centered over the dot
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_drawString(tft, name, x, y);

    // Draw WiFi indicator (if connected to STA network)
    drawWiFiIndicator();

    // Draw battery indicator after profile name (if on battery)
    drawBatteryIndicator();

    // Draw BLE proxy indicator using the latest status
    setBLEProxyStatus(bleProxyEnabled, bleProxyClientConnected, bleReceivingData);
#endif
}

// ============================================================================
// Battery indicator
// ============================================================================

void V1Display::drawBatteryIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern BatteryManager batteryManager;
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();
    
    // Battery icon position - VERTICAL at bottom-right
    // Position to the right of profile indicator area, avoiding direction arrows
    const int battW = 14;   // Battery body width (was height when horizontal)
    const int battH = 28;   // Battery body height (was width when horizontal)
    const int battX = SCREEN_WIDTH - battW - 8;  // Right edge with margin
    const int battY = SCREEN_HEIGHT - battH - 8; // Bottom with margin (cap above)
    const int capW = 8;     // Positive terminal cap width (horizontal bar at top)
    const int capH = 3;     // Positive terminal cap height
    
    // Hide battery when on USB power (voltage near max)
    // Use hysteresis to prevent flickering: hide above 4125, show below 4095
    static bool showBatteryOnUSB = true;
    uint16_t voltage = batteryManager.getVoltageMillivolts();
    if (voltage > 4125) {
        showBatteryOnUSB = false;  // On USB or fully charged
    } else if (voltage < 4095) {
        showBatteryOnUSB = true;   // On battery, not full
    }
    // Between 4095-4125: keep previous state (hysteresis)
    
    // Get battery percentage for display
    uint8_t pct = batteryManager.getPercentage();
    
    // If percent is enabled, ONLY show percent (never icon)
    if (s.showBatteryPercent && !s.hideBatteryIcon && batteryManager.hasBattery()) {
        // Cache percent drawing to avoid heavy FreeType work every frame
        static int lastPctDrawn = -1;
        static bool lastPctVisible = false;
        static uint16_t lastPctColor = 0;
        static unsigned long lastPctDrawMs = 0;
        const unsigned long PCT_FORCE_REDRAW_MS = 60000;  // 60s safety refresh

        // Clear battery icon area (we never show icon when percent is enabled)
        FILL_RECT(battX - 2, battY - capH - 4, battW + 4, battH + capH + 6, PALETTE_BG);

        // Only draw percent if not on USB
        if (!showBatteryOnUSB) {
            // Clear percent area when not visible
            if (lastPctVisible) {
                FILL_RECT(SCREEN_WIDTH - 50, 0, 48, 30, PALETTE_BG);
                lastPctVisible = false;
                lastPctDrawn = -1;
            }
            return;  // No percent on USB/fully charged
        }

        // Choose color based on level
        uint16_t textColor;
        if (pct <= 20) {
            textColor = 0xF800;  // Red - critical
        } else if (pct <= 40) {
            textColor = 0xFD20;  // Orange - low
        } else {
            textColor = 0x07E0;  // Green - good
        }
        textColor = dimColor(textColor);

        // Decide if we actually need to redraw
        unsigned long nowMs = millis();
        bool needsRedraw = dirty.battery ||  // Screen was cleared
                           (!lastPctVisible) ||
                           (pct != lastPctDrawn) ||
                           (textColor != lastPctColor) ||
                           ((nowMs - lastPctDrawMs) >= PCT_FORCE_REDRAW_MS);

        if (!needsRedraw) {
            return;  // Skip expensive render when nothing changed
        }
        
        // Clear force flag - we're handling it
        dirty.battery = false;

        // Format percentage string (no % to save space)
        char pctStr[4];
        snprintf(pctStr, sizeof(pctStr), "%d", pct);

        // Always clear the text area before drawing (covers shorter numbers)
        // Positioned to avoid top arrow which extends to roughly SCREEN_WIDTH - 14
        FILL_RECT(SCREEN_WIDTH - 50, 0, 48, 30, PALETTE_BG);

        // Right-aligned built-in font near the top-right corner.
        GFX_setTextDatum(TR_DATUM);
        TFT_CALL(setTextSize)(2);  // Larger for better visibility
        TFT_CALL(setTextColor)(textColor, PALETTE_BG);
        GFX_drawString(tft, pctStr, SCREEN_WIDTH - 4, 12);

        // Update cache
        lastPctDrawn = pct;
        lastPctColor = textColor;
        lastPctVisible = true;
        lastPctDrawMs = nowMs;
        return;  // Never draw icon when percent is enabled
    }
    
    // Percent is disabled, show icon instead
    // Clear percent area (in case it was previously showing)
    FILL_RECT(SCREEN_WIDTH - 50, 0, 48, 30, PALETTE_BG);
    
    // Don't draw icon if no battery, user hides it, or on USB
    if (!batteryManager.hasBattery() || s.hideBatteryIcon || !showBatteryOnUSB) {
        FILL_RECT(battX - 2, battY - capH - 4, battW + 4, battH + capH + 6, PALETTE_BG);
        return;
    }
    
    const int padding = 2;  // Padding inside battery
    const int sections = 5; // Number of charge sections
    
    int filledSections = (pct + 10) / 20;  // 0-20%=1, 21-40%=2, etc. (min 1 if >0)
    if (pct == 0) filledSections = 0;
    if (filledSections > sections) filledSections = sections;
    
    // Choose color based on level
    uint16_t fillColor;
    if (pct <= 20) {
        fillColor = 0xF800;  // Red - critical
    } else if (pct <= 40) {
        fillColor = 0xFD20;  // Orange - low
    } else {
        fillColor = 0x07E0;  // Green - good
    }
    
    // Clear area (including cap above)
    FILL_RECT(battX - 2, battY - capH - 4, battW + 4, battH + capH + 6, PALETTE_BG);
    
    uint16_t outlineColor = dimColor(PALETTE_TEXT);

    // Draw battery outline (dimmed) - vertical orientation
    DRAW_RECT(battX, battY, battW, battH, outlineColor);  // Main body
    // Positive cap at top, centered
    FILL_RECT(battX + (battW - capW) / 2, battY - capH, capW, capH, outlineColor);
    
    // Draw charge sections (vertical - bottom to top, filled from bottom)
    int sectionH = (battH - 2 * padding - (sections - 1)) / sections;  // Height of each section with 1px gap
    for (int i = 0; i < sections; i++) {
        // Draw from bottom up: section 0 at bottom, section 4 at top
        int sx = battX + padding;
        int sy = battY + battH - padding - (i + 1) * sectionH - i;  // Bottom-up
        int sw = battW - 2 * padding;
        
        if (i < filledSections) {
            FILL_RECT(sx, sy, sw, sectionH, dimColor(fillColor));
        }
    }
#endif
}

// ============================================================================
// BLE proxy indicator
// ============================================================================

void V1Display::drawBLEProxyIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    // Position to the right of WiFi indicator (side-by-side at bottom-left)
    // Evenly spaced with volume and RSSI above
    const int iconSize = 20;
    const int iconY = 145;  // Moved up 2px from 147
    const int iconGap = 6;  // Gap between icons
    
    const int wifiX = 14;
    const int bleX = wifiX + iconSize + iconGap;  // Right of WiFi icon
    const int bleY = iconY;

    // Always clear the area before drawing
    FILL_RECT(bleX - 2, bleY - 2, iconSize + 4, iconSize + 4, PALETTE_BG);

    if (!bleProxyEnabled) {
        bleProxyDrawn = false;
        return;
    }

    // Check if BLE icon should be hidden
    const V1Settings& s = settingsManager.get();
    if (s.hideBleIcon) {
        bleProxyDrawn = false;
        return;
    }

    const bool bleContextFresh = hasFreshBleContext(millis());

    // Icon color from settings: connected vs disconnected
    // When connected but not receiving data, dim further to show "stale" state
    uint16_t btColor;
    if (bleProxyClientConnected) {
        // Connected: bright green when receiving, dimmed when stale
        const bool receivingData = bleReceivingData && bleContextFresh;
        btColor = receivingData ? dimColor(s.colorBleConnected, 85)
                                : dimColor(s.colorBleConnected, 40);  // Much dimmer when no data
    } else {
        btColor = dimColor(s.colorBleDisconnected, 85);
    }

    // Draw Bluetooth rune - the bind rune of ᛒ (Berkanan) and ᚼ (Hagall)
    // Center point of the icon
    int cx = bleX + iconSize / 2;
    int cy = bleY + iconSize / 2;
    
    int h = iconSize - 2;      // Total height
    int top = cy - h / 2;
    int bot = cy + h / 2;
    int mid = cy;
    
    // Right chevron points - where the arrows reach on the right
    int rightX = cx + 5;
    int topChevronY = mid - 4;   // Upper right point
    int botChevronY = mid + 4;   // Lower right point
    
    // Left arrow endpoints
    int leftX = cx - 5;
    int topArrowY = mid - 4;     // Upper left point  
    int botArrowY = mid + 4;     // Lower left point
    
    // Vertical center line (thicker for visibility)
    FILL_RECT(cx - 1, top, 2, h, btColor);
    
    // === RIGHT SIDE: Two chevrons forming the "B" ===
    // Top chevron: top of line → right point → center (draw 3 lines for thickness)
    DRAW_LINE(cx - 1, top, rightX - 1, topChevronY, btColor);
    DRAW_LINE(cx, top, rightX, topChevronY, btColor);
    DRAW_LINE(cx + 1, top, rightX + 1, topChevronY, btColor);
    DRAW_LINE(rightX - 1, topChevronY, cx - 1, mid, btColor);
    DRAW_LINE(rightX, topChevronY, cx, mid, btColor);
    DRAW_LINE(rightX + 1, topChevronY, cx + 1, mid, btColor);
    
    // Bottom chevron: center → right point → bottom of line (draw 3 lines for thickness)
    DRAW_LINE(cx - 1, mid, rightX - 1, botChevronY, btColor);
    DRAW_LINE(cx, mid, rightX, botChevronY, btColor);
    DRAW_LINE(cx + 1, mid, rightX + 1, botChevronY, btColor);
    DRAW_LINE(rightX - 1, botChevronY, cx - 1, bot, btColor);
    DRAW_LINE(rightX, botChevronY, cx, bot, btColor);
    DRAW_LINE(rightX + 1, botChevronY, cx + 1, bot, btColor);
    
    // === LEFT SIDE: Two arrows forming the "X" through center ===
    // Upper-left arrow (draw 3 lines for thickness)
    DRAW_LINE(leftX - 1, topArrowY, cx - 1, mid, btColor);
    DRAW_LINE(leftX, topArrowY, cx, mid, btColor);
    DRAW_LINE(leftX + 1, topArrowY, cx + 1, mid, btColor);
    
    // Lower-left arrow (draw 3 lines for thickness)
    DRAW_LINE(leftX - 1, botArrowY, cx - 1, mid, btColor);
    DRAW_LINE(leftX, botArrowY, cx, mid, btColor);
    DRAW_LINE(leftX + 1, botArrowY, cx + 1, mid, btColor);

    bleProxyDrawn = true;
#endif
}

// ============================================================================
// WiFi indicator
// ============================================================================

void V1Display::drawWiFiIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern WiFiManager wifiManager;
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();
    
    // WiFi icon position - evenly spaced below RSSI
    const int wifiX = 8;
    const int wifiSize = 20;
    const int wifiY = 145;  // Moved up 2px from 147
    
    // Check if user explicitly hides the WiFi icon
    if (s.hideWifiIcon) {
        FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
        return;
    }
    
    const bool wifiServiceActive = wifiManager.isWifiServiceActive();
    const bool staConnected = wifiManager.isConnected();
    const bool apActive = wifiManager.isSetupModeActive();
    const bool showWifiIcon = wifiServiceActive || staConnected;

    if (!showWifiIcon) {
        // Clear the WiFi icon area when WiFi is fully inactive.
        FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
        return;
    }
    
    // Check if any clients are connected to the AP (only when AP is enabled).
    bool hasApClients = apActive && (WiFi.softAPgetStationNum() > 0);
    
    // WiFi icon color: connected vs disconnected (like BLE icon)
    uint16_t wifiColor = (staConnected || hasApClients)
                             ? dimColor(s.colorWiFiConnected, 85)
                             : dimColor(s.colorWiFiIcon, 85);
    
    // Clear area first
    FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
    
    // Center point for arcs (bottom center of icon area)
    int cx = wifiX + wifiSize / 2;
    int cy = wifiY + wifiSize - 3;
    
    // Draw center dot (the WiFi source point)
    FILL_RECT(cx - 2, cy - 2, 5, 5, wifiColor);
    
    // Draw 3 concentric arcs above the dot
    // Arc 1 (inner) - small arc
    for (int angle = -45; angle <= 45; angle += 15) {
        float rad = angle * 3.14159 / 180.0;
        int px = cx + (int)(5 * sin(rad));
        int py = cy - 5 - (int)(5 * cos(rad));
        FILL_RECT(px, py, 2, 2, wifiColor);
    }
    
    // Arc 2 (middle)
    for (int angle = -50; angle <= 50; angle += 12) {
        float rad = angle * 3.14159 / 180.0;
        int px = cx + (int)(9 * sin(rad));
        int py = cy - 5 - (int)(9 * cos(rad));
        FILL_RECT(px, py, 2, 2, wifiColor);
    }
    
    // Arc 3 (outer)
    for (int angle = -55; angle <= 55; angle += 10) {
        float rad = angle * 3.14159 / 180.0;
        int px = cx + (int)(13 * sin(rad));
        int py = cy - 5 - (int)(13 * cos(rad));
        FILL_RECT(px, py, 2, 2, wifiColor);
    }
#endif
}
