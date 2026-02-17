/**
 * Display Layout Constants for V1 Gen2 Display
 * Waveshare ESP32-S3-Touch-LCD-3.49 (640x172 AMOLED)
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

#ifndef DISPLAY_LAYOUT_H
#define DISPLAY_LAYOUT_H

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
constexpr int BAND_COLUMN_X = 0;

// ============================================================================
// Signal/Info Column (Right Side)
// ============================================================================

constexpr int SIGNAL_COLUMN_WIDTH = 200;  // Width reserved for signal bars, arrows, battery
constexpr int SIGNAL_COLUMN_X = SCREEN_WIDTH - SIGNAL_COLUMN_WIDTH;  // X=440

// ============================================================================
// Frequency/Content Area (Center)
// ============================================================================

// Content area margins define the frequency display region
constexpr int CONTENT_LEFT_MARGIN = BAND_COLUMN_WIDTH;     // 120px - after band indicators
constexpr int CONTENT_RIGHT_MARGIN = SIGNAL_COLUMN_WIDTH;  // 200px - before signal column
constexpr int CONTENT_AVAILABLE_WIDTH = SCREEN_WIDTH - CONTENT_LEFT_MARGIN - CONTENT_RIGHT_MARGIN;  // 320px

// ============================================================================
// Secondary Alert Cards
// ============================================================================

constexpr int CARD_WIDTH = 145;           // Card width (fits freq + band label)
constexpr int CARD_HEIGHT = SECONDARY_ROW_HEIGHT;  // Same as row height (54px)
constexpr int CARD_SPACING = 10;          // Gap between cards
constexpr int CARD_BORDER_RADIUS = 5;     // Rounded corner radius

// Card layout calculations
constexpr int CARDS_TOTAL_WIDTH = (CARD_WIDTH * 2) + CARD_SPACING;  // 300px for 2 cards
constexpr int CARDS_START_X = CONTENT_LEFT_MARGIN + 
    (CONTENT_AVAILABLE_WIDTH - CARDS_TOTAL_WIDTH) / 2;  // Center cards in available space

// Card internal layout
constexpr int CARD_CONTENT_CENTER_Y = 18; // Y offset from card top for center content
constexpr int CARD_BOTTOM_ROW_Y = 38;     // Y offset from card top for bottom row
constexpr int CARD_SIGNAL_METER_Y = 34;   // Y offset from card top for signal meter
constexpr int CARD_SIGNAL_METER_MARGIN = 10;  // Left/right margin for signal meter

// ============================================================================
// Status Bar (Top)
// ============================================================================

constexpr int STATUS_BAR_HEIGHT = 20;     // Height of top status bar
constexpr int STATUS_BAR_Y = 0;

constexpr int STATUS_ICON_SIZE = 14;      // Size of status icons.
constexpr int STATUS_ICON_GAP = 6;        // Gap between status icons
constexpr int STATUS_LEFT_MARGIN = 8;     // Left margin for status icons

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

} // namespace DisplayLayout

// Convenience: effective screen height for primary zone rendering
inline int getEffectiveScreenHeight() {
    return DisplayLayout::PRIMARY_ZONE_HEIGHT;
}

#endif // DISPLAY_LAYOUT_H
