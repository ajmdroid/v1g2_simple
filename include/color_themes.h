/**
 * Color Theme Definitions for V1 Gen2 Display
 * Three themes: Standard, High Contrast, and Stealth
 * RGB565 format: RRRRR GGGGGG BBBBB (5 bits red, 6 bits green, 5 bits blue)
 */

#ifndef COLOR_THEMES_H
#define COLOR_THEMES_H

#include <cstdint>

// Color theme options
enum ColorTheme {
    THEME_STANDARD = 0,     // Standard colors (red/blue/green)
    THEME_HIGH_CONTRAST = 1, // High contrast (bright white/yellow/cyan)
    THEME_STEALTH = 2       // Dark mode (muted colors on black)
};

// Color palette structure
struct ColorPalette {
    uint16_t bg;            // Background
    uint16_t text;          // Text/foreground
    uint16_t colorKA;       // K/Ka band (red)
    uint16_t colorK;        // K band (blue)
    uint16_t colorX;        // X band (green)
    uint16_t colorGray;     // Resting/inactive (dark gray)
    uint16_t colorMuted;    // Muted alert (distinct grey)
    uint16_t colorLaser;    // Laser indicator (blue)
    uint16_t colorArrow;    // Arrow indicators (red)
    uint16_t colorSignalBar;// Signal bar fill (red)
};

namespace ColorThemes {
    // Standard theme - classic red/blue/green on black
    constexpr ColorPalette STANDARD = {
        .bg = 0x0000,      // Black
        .text = 0xFFFF,    // White
        .colorKA = 0xF800, // Red
        .colorK = 0x001F,  // Blue
        .colorX = 0x07E0,  // Green
        .colorGray = 0x1082, // Dark gray (resting)
        .colorMuted = 0x3186, // Dark grey (muted) - ~18% brightness
        .colorLaser = 0x001F, // Blue
        .colorArrow = 0xF800, // Red
        .colorSignalBar = 0xF800 // Red
    };
    
    // High Contrast theme - bright colors on black for visibility
    constexpr ColorPalette HIGH_CONTRAST = {
        .bg = 0x0000,      // Black
        .text = 0xFFFF,    // White
        .colorKA = 0xF800, // Bright Red
        .colorK = 0x001F,  // Bright Blue
        .colorX = 0x07E0,  // Bright Green
        .colorGray = 0x4208, // Medium gray (resting)
        .colorMuted = 0x739C, // Darker muted grey
        .colorLaser = 0xF81F, // Magenta (highly visible)
        .colorArrow = 0xFFC0, // Bright Yellow
        .colorSignalBar = 0xFFC0 // Bright Yellow
    };
    
    // Stealth theme - muted colors for low light, subtle appearance
    constexpr ColorPalette STEALTH = {
        .bg = 0x0000,      // Black
        .text = 0x8410,    // Dark gray text (less bright)
        .colorKA = 0x8000, // Dark red
        .colorK = 0x0010,  // Dark blue
        .colorX = 0x0400,  // Dark green
        .colorGray = 0x2104, // Very dark gray (resting)
        .colorMuted = 0x39E7, // Darker muted grey for stealth
        .colorLaser = 0x0010, // Dark blue
        .colorArrow = 0x8000, // Dark red
        .colorSignalBar = 0x8000 // Dark red
    };
    
    // Get palette by theme
    inline const ColorPalette& getPalette(ColorTheme theme) {
        switch (theme) {
            case THEME_HIGH_CONTRAST:
                return HIGH_CONTRAST;
            case THEME_STEALTH:
                return STEALTH;
            case THEME_STANDARD:
            default:
                return STANDARD;
        }
    }
    
    // Get theme name for display
    inline const char* getThemeName(ColorTheme theme) {
        switch (theme) {
            case THEME_HIGH_CONTRAST:
                return "High Contrast";
            case THEME_STEALTH:
                return "Stealth";
            case THEME_STANDARD:
            default:
                return "Standard";
        }
    }
}

#endif // COLOR_THEMES_H
