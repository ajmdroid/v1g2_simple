/**
 * Display Layout Constants for V1 Gen2 Display
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640x172 LCD)
 *
 * Centralizes all layout-related constants to ensure consistency
 * across display.cpp and tests. Derived from config.h screen dimensions.
 *
 * Layout Overview (640x172 landscape):
 * ┌────────────────────────────────────────────────────────────────────────────┐
 * │ [Status Bar: indicators]                                      [WiFi]      │ Y=0-20
 * ├──────────┬───────────────────────────────────────────┬────────────────────┤
 * │          │                                           │  Signal Bars       │
 * │  Band    │   Frequency / Alert Content               │  Direction Arrow   │ Y=20-95
 * │  Labels  │   (Primary Zone - 95px)                   │  Volume/RSSI       │
 * │          │                                           │                    │
 * ├──────────┴───────────────────────────────────────────┴────────────────────┤
 * │  [Secondary Card 1]     [Secondary Card 2]            [Battery]           │ Y=118-172
 * │  (145x54px each)                                                        │
 * └────────────────────────────────────────────────────────────────────────────┘
 */

#pragma once

// ============================================================================
// Screen Dimensions (from platformio.ini build flags)
// ============================================================================
// SCREEN_WIDTH and SCREEN_HEIGHT are defined via -D flags in platformio.ini
// Default values if not defined:
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif

// ============================================================================
// Primary Layout Zones
// ============================================================================

namespace DisplayLayout {

// Main display zone (shows frequency, status during alerts)
constexpr int PRIMARY_ZONE_HEIGHT = 95;   // Fixed height for primary alert display
constexpr int PRIMARY_ZONE_Y = 20;        // Below status bar

// Secondary row (alert cards)
constexpr int SECONDARY_ROW_HEIGHT = 54;  // Height for secondary alert cards
constexpr int SECONDARY_ROW_Y = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Y=118 on 172px display

// Verify zones don't overlap (compile-time check)
static_assert(PRIMARY_ZONE_Y + PRIMARY_ZONE_HEIGHT <= SECONDARY_ROW_Y,
              "Primary zone overlaps with secondary row");

// ============================================================================
// Band Indicator Column (Left Side)
// ============================================================================

constexpr int BAND_COLUMN_WIDTH = 120;    // Width reserved for band labels (X, K, Ka, L)

// ============================================================================
// Signal/Info Column (Right Side)
// ============================================================================

constexpr int SIGNAL_COLUMN_WIDTH = 200;  // Width reserved for signal bars, arrows, battery

// ============================================================================
// Frequency/Content Area (Center)
// ============================================================================

// Content area margins define the frequency display region
constexpr int CONTENT_LEFT_MARGIN = BAND_COLUMN_WIDTH;     // 120px - after band indicators
constexpr int CONTENT_RIGHT_MARGIN = SIGNAL_COLUMN_WIDTH;  // 200px - before signal column
constexpr int CONTENT_AVAILABLE_WIDTH = SCREEN_WIDTH - CONTENT_LEFT_MARGIN - CONTENT_RIGHT_MARGIN;  // 320px

// ============================================================================
// Top Counter Area
// ============================================================================

constexpr int TOP_COUNTER_FONT_SIZE = 60; // Matches DisplayFontManager constant
constexpr int TOP_COUNTER_FIELD_X = 16;
constexpr int TOP_COUNTER_FIELD_Y = 6;
constexpr int TOP_COUNTER_FIELD_W = 55;
constexpr int TOP_COUNTER_FIELD_H = TOP_COUNTER_FONT_SIZE + 8;
constexpr int TOP_COUNTER_TEXT_Y = 8;
constexpr int TOP_COUNTER_PAD_RIGHT = 2;
constexpr int TOP_COUNTER_FALLBACK_WIDTH = 28;

// ============================================================================
// Flush Strip Regions
// ============================================================================
// Three full-height vertical strips covering the entire screen.
// All use h=SCREEN_HEIGHT so flushRegion() hits the fast path
// (phys_pw == CANVAS_WIDTH → single contiguous blit).

constexpr int STRIP_LEFT_X  = 0;
constexpr int STRIP_LEFT_W  = BAND_COLUMN_WIDTH;         // 120

constexpr int STRIP_CENTER_X = BAND_COLUMN_WIDTH;         // 120
constexpr int STRIP_CENTER_W = CONTENT_AVAILABLE_WIDTH;    // 320

constexpr int STRIP_RIGHT_X  = SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH; // 440
constexpr int STRIP_RIGHT_W  = SIGNAL_COLUMN_WIDTH;        // 200

// Full height for all strips — required for flushRegion fast path
constexpr int STRIP_H = SCREEN_HEIGHT;                     // 172
constexpr int STRIP_Y = 0;

} // namespace DisplayLayout

// Convenience: effective screen height for primary zone rendering
inline int getEffectiveScreenHeight() {
    return DisplayLayout::PRIMARY_ZONE_HEIGHT;
}

