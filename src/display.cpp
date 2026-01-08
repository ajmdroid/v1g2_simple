/**
 * Display Driver for V1 Gen2 Display
 * Supports multiple hardware platforms
 */

#include "display.h"
#include "../include/config.h"
#include "../include/color_themes.h"
#include "v1simple_logo.h"  // Splash screen image (640x172)
#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
#include <esp_heap_caps.h>
#include "../include/FreeSansBold24pt7b.h"  // Custom font for band labels

// OpenFontRender for antialiased TrueType rendering
#include "OpenFontRender.h"
#include "../include/MontserratBold.h"       // Montserrat Bold TTF (subset: 0-9, -, ., LASER, SCAN)

// Global OpenFontRender instance for Modern style
static OpenFontRender ofr;
static bool ofrInitialized = false;

// Multi-alert mode tracking (extern from V1Display class)
static bool g_multiAlertMode = false;
static constexpr int MULTI_ALERT_OFFSET = 40;  // Pixels to shift up when cards are shown
static constexpr uint8_t STRONG_SIGNAL_UNMUTE_THRESHOLD = 5;  // Bars (out of 6) to override mute

// Helper to get effective screen height (reduced when multi-alert cards are shown)
static inline int getEffectiveScreenHeight() {
    return g_multiAlertMode ? (SCREEN_HEIGHT - MULTI_ALERT_OFFSET) : SCREEN_HEIGHT;
}

// Utility: dim a 565 color by a percentage (default 60%) for subtle icons
static inline uint16_t dimColor(uint16_t c, uint8_t scalePercent = 60) {
    uint8_t r = (c >> 11) & 0x1F;
    uint8_t g = (c >> 5) & 0x3F;
    uint8_t b = c & 0x1F;
    r = (r * scalePercent) / 100;
    g = (g * scalePercent) / 100;
    b = (b * scalePercent) / 100;
    return (r << 11) | (g << 5) | b;
}

// Helper macro to handle pointer vs object access for tft
// Arduino_GFX uses pointer (tft->), TFT_eSPI uses object (tft.)
#if defined(DISPLAY_USE_ARDUINO_GFX)
    #define TFT_CALL(method) tft->method
    #define TFT_PTR tft
    
    // Store current text datum for Arduino_GFX compatibility
    static uint8_t _gfxCurrentTextDatum = TL_DATUM;
    
    // TFT_BL alias for backlight pin
    #define TFT_BL LCD_BL
    
#else
    #define TFT_CALL(method) tft.method
    #define TFT_PTR &tft
#endif

// ============================================================================
// Coordinate transformation - rotation handles orientation
// ============================================================================
#define TX(vx, vy) (vx)
#define TY(vx, vy) (vy)
#define TW(vw, vh) (vw)
#define TH(vw, vh) (vh)

// ============================================================================
// Drawing wrapper macros with coordinate transformation
// ============================================================================
#define FILL_RECT(x, y, w, h, color) TFT_CALL(fillRect)(TX(x,y), TY(x,y), TW(w,h), TH(w,h), (color))
#define DRAW_RECT(x, y, w, h, color) TFT_CALL(drawRect)(TX(x,y), TY(x,y), TW(w,h), TH(w,h), (color))
#define FILL_ROUND_RECT(x, y, w, h, r, color) TFT_CALL(fillRoundRect)(TX(x,y), TY(x,y), TW(w,h), TH(w,h), (r), (color))
#define DRAW_ROUND_RECT(x, y, w, h, r, color) TFT_CALL(drawRoundRect)(TX(x,y), TY(x,y), TW(w,h), TH(w,h), (r), (color))
#define FILL_CIRCLE(x, y, r, color) TFT_CALL(fillCircle)(TX(x,y), TY(x,y), (r), (color))
#define DRAW_CIRCLE(x, y, r, color) TFT_CALL(drawCircle)(TX(x,y), TY(x,y), (r), (color))
#define FILL_TRIANGLE(x0, y0, x1, y1, x2, y2, color) TFT_CALL(fillTriangle)(TX(x0,y0), TY(x0,y0), TX(x1,y1), TY(x1,y1), TX(x2,y2), TY(x2,y2), (color))
#define DRAW_LINE(x0, y0, x1, y1, color) TFT_CALL(drawLine)(TX(x0,y0), TY(x0,y0), TX(x1,y1), TY(x1,y1), (color))
#define DRAW_PIXEL(x, y, color) TFT_CALL(drawPixel)(TX(x,y), TY(x,y), (color))
#define FILL_SCREEN(color) TFT_CALL(fillScreen)(color)

// Global display instance reference for access to color palette (set by V1Display)
V1Display* g_displayInstance = nullptr;

// Helper to get current color palette
inline const ColorPalette& getColorPalette() {
    if (g_displayInstance) {
        return g_displayInstance->getCurrentPalette();
    }
    return ColorThemes::STANDARD();
}

// Color macros that use the current theme palette
#define PALETTE_BG getColorPalette().bg
#define PALETTE_TEXT getColorPalette().text
#define PALETTE_KA getColorPalette().colorKA
#define PALETTE_K getColorPalette().colorK
#define PALETTE_X getColorPalette().colorX
#define PALETTE_GRAY getColorPalette().colorGray
#define PALETTE_MUTED getColorPalette().colorMuted
#define PALETTE_LASER getColorPalette().colorLaser
#define PALETTE_ARROW getColorPalette().colorArrow
#define PALETTE_SIGNAL_BAR getColorPalette().colorSignalBar

// ============================================================================
// Cross-platform text drawing helpers
// TFT_eSPI has setTextDatum() and drawString() 
// Arduino_GFX uses setCursor() and print()
// ============================================================================

#if defined(DISPLAY_USE_ARDUINO_GFX)

// Arduino_GFX implementation of setTextDatum (store for later use)
#define GFX_setTextDatum(d) do { _gfxCurrentTextDatum = (d); } while(0)

// Arduino_GFX implementation of drawString with datum support (with coordinate transform)
static inline void GFX_drawString(Arduino_Canvas* canvas, const char* str, int16_t x, int16_t y) {
    int16_t x1, y1;
    uint16_t w, h;
    canvas->getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
    
    int16_t drawX = x, drawY = y;
    
    // Apply horizontal alignment based on current datum
    switch (_gfxCurrentTextDatum) {
        case TC_DATUM: case MC_DATUM: case BC_DATUM:
            drawX = x - w / 2;
            break;
        case TR_DATUM: case MR_DATUM: case BR_DATUM:
            drawX = x - w;
            break;
        default: // TL, ML, BL - left aligned (default)
            break;
    }
    
    // Apply vertical alignment based on current datum  
    switch (_gfxCurrentTextDatum) {
        case ML_DATUM: case MC_DATUM: case MR_DATUM:
            drawY = y - h / 2;
            break;
        case BL_DATUM: case BC_DATUM: case BR_DATUM:
            drawY = y - h;
            break;
        default: // TL, TC, TR - top aligned (default)
            break;
    }
    
    canvas->setCursor(drawX, drawY);
    canvas->print(str);
}

#else

// TFT_eSPI - native methods
#define GFX_setTextDatum(d) tft.setTextDatum(d)
static inline void GFX_drawString(TFT_eSPI& canvas, const char* str, int16_t x, int16_t y) {
    canvas.drawString(str, x, y);
}

#endif

namespace {
struct SegMetrics {
    int segLen;
    int segThick;
    int digitW;
    int digitH;
    int spacing;
    int dot;
};

SegMetrics segMetrics(float scale) {
    // Base values tuned to mimic the chunky seven-seg look from the reference panel
    int segLen = static_cast<int>(8 * scale + 0.5f);
    int segThick = static_cast<int>(3 * scale + 0.5f);
    if (segLen < 2) segLen = 2;
    if (segThick < 1) segThick = 1;
    return {
        segLen,
        segThick,
        segLen + 2 * segThick,
        2 * segLen + 3 * segThick,
        segThick,
        segThick
    };
}

constexpr bool DIGIT_SEGMENTS[10][7] = {
    // a, b, c, d, e, f, g
    {true,  true,  true,  true,  true,  true,  false}, // 0
    {false, true,  true,  false, false, false, false}, // 1
    {true,  true,  false, true,  true,  false, true }, // 2
    {true,  true,  true,  true,  false, false, true }, // 3
    {false, true,  true,  false, false, true,  true }, // 4
    {true,  false, true,  true,  false, true,  true }, // 5
    {true,  false, true,  true,  true,  true,  true }, // 6
    {true,  true,  true,  false, false, false, false}, // 7
    {true,  true,  true,  true,  true,  true,  true }, // 8
    {true,  true,  true,  true,  false, true,  true }  // 9
};

// 14-segment display encoding
// Segments: 0=top, 1=top-right, 2=bottom-right, 3=bottom, 4=bottom-left, 5=top-left,
//           6=middle-left, 7=middle-right, 8=diag-top-left, 9=diag-top-right,
//           10=center-top, 11=center-bottom, 12=diag-bottom-left, 13=diag-bottom-right
struct Char14Seg {
    char ch;
    uint16_t segs; // bit flags for segments 0-13
};

// Segment bit definitions
#define S14_TOP         (1<<0)
#define S14_TR          (1<<1)   // top-right vertical
#define S14_BR          (1<<2)   // bottom-right vertical
#define S14_BOT         (1<<3)
#define S14_BL          (1<<4)   // bottom-left vertical
#define S14_TL          (1<<5)   // top-left vertical
#define S14_ML          (1<<6)   // middle-left horizontal
#define S14_MR          (1<<7)   // middle-right horizontal
#define S14_DTL         (1<<8)   // diagonal top-left
#define S14_DTR         (1<<9)   // diagonal top-right
#define S14_CT          (1<<10)  // center-top vertical
#define S14_CB          (1<<11)  // center-bottom vertical
#define S14_DBL         (1<<12)  // diagonal bottom-left
#define S14_DBR         (1<<13)  // diagonal bottom-right

constexpr Char14Seg CHAR14_MAP[] = {
    {'0', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_BL | S14_TL},
    {'1', S14_TR | S14_BR},
    {'2', S14_TOP | S14_TR | S14_ML | S14_MR | S14_BL | S14_BOT},
    {'3', S14_TOP | S14_TR | S14_MR | S14_BR | S14_BOT},
    {'4', S14_TL | S14_ML | S14_MR | S14_TR | S14_BR},
    {'5', S14_TOP | S14_TL | S14_ML | S14_MR | S14_BR | S14_BOT},
    {'6', S14_TOP | S14_TL | S14_ML | S14_MR | S14_BR | S14_BOT | S14_BL},
    {'7', S14_TOP | S14_TR | S14_BR},
    {'8', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_BL | S14_TL | S14_ML | S14_MR},
    {'9', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_TL | S14_ML | S14_MR},
    {'A', S14_TOP | S14_TL | S14_TR | S14_ML | S14_MR | S14_BL | S14_BR},
    {'C', S14_TOP | S14_TL | S14_BL | S14_BOT},
    {'D', S14_TOP | S14_TR | S14_BR | S14_BOT | S14_CT | S14_CB},
    {'E', S14_TOP | S14_TL | S14_ML | S14_BL | S14_BOT},
    {'L', S14_TL | S14_BL | S14_BOT},
    {'M', S14_TL | S14_TR | S14_BL | S14_BR | S14_DTL | S14_DTR},
    {'N', S14_TL | S14_BL | S14_TR | S14_BR | S14_DTL | S14_DBR},
    {'R', S14_TOP | S14_TL | S14_TR | S14_ML | S14_MR | S14_BL | S14_DBR},
    {'S', S14_TOP | S14_TL | S14_ML | S14_MR | S14_BR | S14_BOT},
    {'T', S14_TOP | S14_CT | S14_CB},
    {'U', S14_TL | S14_TR | S14_BL | S14_BR | S14_BOT},
    {'-', S14_ML | S14_MR},
    {'.', 0}, // dot handled separately
};
constexpr int CHAR14_MAP_SIZE = sizeof(CHAR14_MAP) / sizeof(CHAR14_MAP[0]);

uint16_t get14SegPattern(char c) {
    char upper = (c >= 'a' && c <= 'z') ? (c - 32) : c;
    for (int i = 0; i < CHAR14_MAP_SIZE; i++) {
        if (CHAR14_MAP[i].ch == upper) return CHAR14_MAP[i].segs;
    }
    return 0;
}
} // namespace

V1Display::V1Display() {
    // Initialize with standard theme by default
    currentPalette = ColorThemes::STANDARD();
    // Set global instance for color palette access
    g_displayInstance = this;

    currentScreen = ScreenMode::Unknown;
    paletteRevision = 0;
    lastRestingPaletteRevision = 0;
    lastRestingProfileSlot = -1;
}

V1Display::~V1Display() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    if (tft) delete tft;
    if (gfxPanel) delete gfxPanel;
    if (bus) delete bus;
#endif
}

bool V1Display::begin() {
    Serial.println("Display init start...");
    Serial.print("Board: ");
    Serial.println(DISPLAY_NAME);
    
#if PIN_POWER_ON >= 0
    // Power was held low in setup(); bring it up now
    digitalWrite(PIN_POWER_ON, HIGH);
    Serial.println("Power ON");
    delay(200);
#endif
    
    // Initialize display
    Serial.println("Calling display init...");

#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Arduino_GFX initialization for Waveshare 3.49"
    Serial.println("Initializing Arduino_GFX for Waveshare 3.49...");
    Serial.printf("Pins: CS=%d, SCK=%d, D0=%d, D1=%d, D2=%d, D3=%d, RST=%d, BL=%d\n",
                  LCD_CS, LCD_SCLK, LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3, LCD_RST, LCD_BL);
    
    // Configure backlight pin
    Serial.println("Configuring backlight...");
    // Waveshare 3.49" has INVERTED backlight PWM:
    // 0 = full brightness, 255 = off
    pinMode(LCD_BL, OUTPUT);
    analogWrite(LCD_BL, 255);  // Start with backlight off (inverted: 255=off)
    Serial.println("Backlight configured, set to 255 (off, inverted)");
    
    // Manual RST toggle with Waveshare timing BEFORE creating bus
    // This is critical - Waveshare examples do: HIGH(30ms) -> LOW(250ms) -> HIGH(30ms)
    Serial.println("Manual RST toggle (Waveshare timing)...");
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_RST, HIGH);
    delay(30);
    digitalWrite(LCD_RST, LOW);
    delay(250);
    digitalWrite(LCD_RST, HIGH);
    delay(30);
    Serial.println("RST toggle complete");
    
    // Create QSPI bus
    Serial.println("Creating QSPI bus...");
    bus = new Arduino_ESP32QSPI(
        LCD_CS,    // CS
        LCD_SCLK,  // SCK
        LCD_DATA0, // D0
        LCD_DATA1, // D1
        LCD_DATA2, // D2
        LCD_DATA3  // D3
    );
    if (!bus) {
        Serial.println("ERROR: Failed to create bus!");
        return false;
    }
    Serial.println("QSPI bus created");
    
    // Create AXS15231B panel - native 172x640 portrait
    // Pass GFX_NOT_DEFINED for RST since we already did manual reset
    Serial.println("Creating AXS15231B panel...");
#ifdef WINDOWS_BUILD
    // GFX Library 1.4.9 - simpler constructor without init_operations
    gfxPanel = new Arduino_AXS15231B(
        bus,               // bus
        GFX_NOT_DEFINED,   // RST - we already did manual reset
        0,                 // rotation (0 = no panel rotation)
        false,             // IPS
        172,               // width (Waveshare 3.49" is 172 wide)
        640,               // height
        0,                 // col_offset1
        0,                 // row_offset1
        0,                 // col_offset2
        0                  // row_offset2
    );
#else
    // GFX Library 1.6.4 - full constructor with init_operations
    gfxPanel = new Arduino_AXS15231B(
        bus,               // bus
        GFX_NOT_DEFINED,   // RST - we already did manual reset
        0,                 // rotation (0 = no panel rotation)
        false,             // IPS
        172,               // width (Waveshare 3.49" is 172 wide)
        640,               // height
        0,                 // col_offset1
        0,                 // row_offset1
        0,                 // col_offset2
        0,                 // row_offset2
        axs15231b_180640_init_operations,   // init operations for this panel type
        sizeof(axs15231b_180640_init_operations)
    );
#endif
    if (!gfxPanel) {
        Serial.println("ERROR: Failed to create panel!");
        return false;
    }
    Serial.println("AXS15231B panel created with init_operations");
    
    // Create canvas as 172x640 native with rotation=1 for landscape (90°)
    Serial.println("Creating canvas 172x640 with rotation=1 (landscape)...");
    tft = new Arduino_Canvas(172, 640, gfxPanel, 0, 0, 1);
    
    if (!tft) {
        Serial.println("ERROR: Failed to create canvas!");
        return false;
    }
    Serial.println("Canvas created");
    
    Serial.println("Calling tft->begin()...");
    if (!tft->begin()) {
        Serial.println("ERROR: tft->begin() failed!");
        return false;
    }
    Serial.println("tft->begin() succeeded");
    Serial.printf("Canvas size: width=%d, height=%d\n", tft->width(), tft->height());
    
    Serial.println("Filling screen with black...");
    tft->fillScreen(COLOR_BLACK);
    tft->flush();
    Serial.println("Screen filled and flushed");
    
    // Turn on backlight (inverted: 0 = full brightness)
    Serial.println("Turning on backlight (inverted PWM)...");
    analogWrite(LCD_BL, 0);  // Full brightness (inverted: 0=on)
    delay(100);
    Serial.println("Backlight ON");
    
#else
    // TFT_eSPI initialization
    TFT_CALL(init)();
    delay(200);
    TFT_CALL(setRotation)(DISPLAY_ROTATION);
    TFT_CALL(fillScreen)(PALETTE_BG); // First clear
    delay(10);
    TFT_CALL(fillScreen)(PALETTE_BG); // Second clear to ensure no white flash
#endif

    delay(50); // Give hardware time to settle
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->setTextColor(PALETTE_TEXT);
    tft->setTextSize(2);
#else
    TFT_CALL(setTextColor)(PALETTE_TEXT, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM); // Middle center
    TFT_CALL(setTextSize)(2);
#endif

    Serial.println("Display initialized successfully!");
    Serial.print("Screen: ");
    Serial.print(SCREEN_WIDTH);
    Serial.print("x");
    Serial.println(SCREEN_HEIGHT);
    
    // Initialize OpenFontRender for antialiased Modern font
    Serial.println("Initializing OpenFontRender...");
    Serial.printf("Font data size: %d bytes\n", sizeof(MontserratBold));
    ofr.setSerial(Serial);  // Enable debug output
    ofr.showFreeTypeVersion();
    ofr.setDrawer(*tft);  // Use Arduino_GFX canvas for drawing (dereference pointer)
    FT_Error ftErr = ofr.loadFont(MontserratBold, sizeof(MontserratBold));
    if (ftErr) {
        Serial.printf("ERROR: Failed to load Montserrat font! FT_Error: 0x%02X\n", ftErr);
        ofrInitialized = false;
    } else {
        Serial.println("OpenFontRender initialized with Montserrat Bold");
        ofrInitialized = true;
    }
    
    // Load color theme from settings
    updateColorTheme();
    
    return true;
}

void V1Display::setBrightness(uint8_t level) {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // PWM brightness control for Arduino_GFX
    // Waveshare 3.49" has INVERTED backlight: 0=full on, 255=off
    #ifdef LCD_BL
    analogWrite(LCD_BL, 255 - level);  // Invert the level
    #endif
#else
    // Simple on/off control for TFT_eSPI (pin doesn't support PWM on all boards)
    #ifdef TFT_BL
    digitalWrite(TFT_BL, level > 0 ? HIGH : LOW);
    #endif
#endif
}

// Brightness slider overlay for BOOT button adjustment
void V1Display::showBrightnessSlider(uint8_t currentLevel) {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Clear screen to dark background
    tft->fillScreen(0x0000);
    
    // Layout: 640x172 landscape
    // Slider: horizontal bar spanning most of width, centered vertically
    const int sliderMargin = 40;
    const int sliderY = 86;  // Centered vertically
    const int sliderHeight = 12;
    const int sliderWidth = SCREEN_WIDTH - (sliderMargin * 2);  // 560 pixels
    const int sliderX = sliderMargin;
    
    // Title at top
    tft->setTextColor(0xFFFF);  // White
    tft->setTextSize(2);
    tft->setCursor((SCREEN_WIDTH - 170) / 2, 20);
    tft->print("BRIGHTNESS");
    
    // Draw slider track (dark gray outline)
    tft->drawRect(sliderX - 2, sliderY - 2, sliderWidth + 4, sliderHeight + 4, 0x4208);  // Dark gray border
    tft->fillRect(sliderX, sliderY, sliderWidth, sliderHeight, 0x2104);  // Dark gray track
    
    // Draw filled portion based on current level
    // Fill grows from LEFT to RIGHT: left = dim (0%), right = bright (100%)
    // Level range: 80 (dim) to 255 (bright)
    int fillWidth = ((currentLevel - 80) * sliderWidth) / 175;  // 0 at 80, full at 255
    tft->fillRect(sliderX, sliderY, fillWidth, sliderHeight, 0x07E0);  // Green fill from left
    
    // Draw thumb/handle at the right edge of fill (moves right as brightness increases)
    int thumbX = sliderX + fillWidth - 4;
    if (thumbX < sliderX) thumbX = sliderX;
    if (thumbX > sliderX + sliderWidth - 4) thumbX = sliderX + sliderWidth - 4;
    tft->fillRect(thumbX, sliderY - 6, 8, sliderHeight + 12, 0xFFFF);  // White thumb
    
    // Show current value as percentage (0% at min 80, 100% at max 255)
    char valueStr[8];
    int percent = ((currentLevel - 80) * 100) / 175;
    snprintf(valueStr, sizeof(valueStr), "%d%%", percent);
    tft->setTextSize(2);
    int textWidth = strlen(valueStr) * 12;
    tft->setCursor((SCREEN_WIDTH - textWidth) / 2, 120);
    tft->print(valueStr);
    
    // Instructions at bottom
    tft->setTextSize(1);
    tft->setTextColor(0x8410);  // Gray
    tft->setCursor((SCREEN_WIDTH - 260) / 2, 150);
    tft->print("Touch to adjust - BOOT to save");
    
    tft->flush();
#endif
}

void V1Display::updateBrightnessSlider(uint8_t level) {
    // Re-render the slider with new level
    // Also apply the brightness in real-time for visual feedback
    setBrightness(level);
    showBrightnessSlider(level);
}

void V1Display::hideBrightnessSlider() {
    // Just clear - caller will refresh normal display
    clear();
}

void V1Display::clear() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->fillScreen(PALETTE_BG);
    tft->flush();
#else
    TFT_CALL(fillScreen)(PALETTE_BG);
#endif
    bleProxyDrawn = false;
}

void V1Display::setBLEProxyStatus(bool proxyEnabled, bool clientConnected) {
#if defined(DISPLAY_WAVESHARE_349)
    if (bleProxyDrawn &&
        proxyEnabled == bleProxyEnabled &&
        clientConnected == bleProxyClientConnected) {
        return;  // No visual change needed
    }

    bleProxyEnabled = proxyEnabled;
    bleProxyClientConnected = clientConnected;
    drawBLEProxyIndicator();
    flush();
#endif
}

void V1Display::drawBaseFrame() {
    // Clean black background (t4s3-style)
    TFT_CALL(fillScreen)(PALETTE_BG);
    bleProxyDrawn = false;  // Force indicator redraw after full clears
    secondaryCardsNeedRedraw = true;  // Force secondary cards redraw after screen clear
}

void V1Display::drawSevenSegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    bool segments[7] = {false, false, false, false, false, false, false};
    // Segment layout:   0=top, 1=upper-right, 2=lower-right, 3=bottom, 4=lower-left, 5=upper-left, 6=middle

    if (c >= '0' && c <= '9') {
        for (int i = 0; i < 7; ++i) {
            segments[i] = DIGIT_SEGMENTS[c - '0'][i];
        }
    } else if (c == '-') {
        segments[6] = true; // Middle bar only
    } else if (c == '=') {
        // Three horizontal bars for laser alert (top, middle, bottom)
        segments[0] = segments[6] = segments[3] = true;
    } else if (c == 'A' || c == 'a') {
        // A = all but bottom segment
        segments[0] = segments[1] = segments[2] = segments[4] = segments[5] = segments[6] = true;
    } else if (c == 'L') {
        // Full L: bottom + lower-left + upper-left
        segments[3] = segments[4] = segments[5] = true;
    } else if (c == 'l') {
        // Logic (lowercase) L: bottom + lower-left only
        segments[3] = segments[4] = true;
    } else if (c == 'S' || c == 's') {
        // S = top, upper-left, middle, lower-right, bottom (like 5)
        segments[0] = segments[5] = segments[6] = segments[2] = segments[3] = true;
    } else if (c == 'E' || c == 'e') {
        // E = top, upper-left, middle, lower-left, bottom
        segments[0] = segments[5] = segments[6] = segments[4] = segments[3] = true;
    } else if (c == 'R' || c == 'r') {
        // r = middle, lower-left (lowercase r style)
        segments[6] = segments[4] = true;
    }

    auto drawSeg = [&](int sx, int sy, int w, int h, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        FILL_ROUND_RECT(sx, sy, w, h, scale, col);
    };

    int ax = x + m.segThick;
    int ay = y;
    int bx = x + m.segLen + m.segThick;
    int byTop = y + m.segThick;
    int byBottom = y + m.segLen + 2 * m.segThick;
    int dx = ax;
    int dy = y + 2 * m.segLen + 2 * m.segThick;
    int gx = ax;
    int gy = y + m.segLen + m.segThick;

    drawSeg(ax, ay, m.segLen, m.segThick, segments[0]);       // Top
    drawSeg(bx, byTop, m.segThick, m.segLen, segments[1]);    // Upper right
    drawSeg(bx, byBottom, m.segThick, m.segLen, segments[2]); // Lower right
    drawSeg(dx, dy, m.segLen, m.segThick, segments[3]);       // Bottom
    drawSeg(x, byBottom, m.segThick, m.segLen, segments[4]);  // Lower left
    drawSeg(x, byTop, m.segThick, m.segLen, segments[5]);     // Upper left
    drawSeg(gx, gy, m.segLen, m.segThick, segments[6]);       // Middle

    if (addDot) {
        int dotR = m.dot / 2 + 1;
        int dotX = x + m.digitW + dotR;
        int dotY = y + m.digitH - dotR;
        FILL_CIRCLE(dotX, dotY, dotR, onColor);
    }
}

int V1Display::measureSevenSegmentText(const char* text, float scale) const {
    SegMetrics m = segMetrics(scale);
    int width = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '.') continue;
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        width += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot) ++i;
    }
    if (width > 0) {
        width -= m.spacing; // remove trailing spacing
    }
    return width;
}

int V1Display::drawSevenSegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    int cursor = x;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '.') continue; // handled alongside previous digit
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        drawSevenSegmentDigit(cursor, y, scale, c, hasDot, onColor, offColor);
        cursor += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot) ++i;
    }
    return cursor - x - m.spacing;
}

// 14-segment digit drawing for proper alphanumeric display
void V1Display::draw14SegmentDigit(int x, int y, float scale, char c, bool addDot, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    uint16_t pattern = get14SegPattern(c);
    
    auto drawHSeg = [&](int sx, int sy, int w, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        FILL_ROUND_RECT(sx, sy, w, m.segThick, scale, col);
    };
    
    auto drawVSeg = [&](int sx, int sy, int h, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        FILL_ROUND_RECT(sx, sy, m.segThick, h, scale, col);
    };
    
    auto drawDiag = [&](int x1, int y1, int x2, int y2, bool on) {
        uint16_t col = on ? onColor : offColor;
        if (!on && offColor == PALETTE_BG) return;
        // Draw thick diagonal line
        for (int t = -m.segThick/2; t <= m.segThick/2; t++) {
            DRAW_LINE(x1+t, y1, x2+t, y2, col);
            DRAW_LINE(x1, y1+t, x2, y2+t, col);
        }
    };
    
    int halfW = m.segLen / 2;
    int centerX = x + m.segThick + halfW;
    int midY = y + m.segLen + m.segThick;
    
    // Horizontal segments
    drawHSeg(x + m.segThick, y, m.segLen, pattern & S14_TOP);                           // Top
    drawHSeg(x + m.segThick, y + 2*m.segLen + 2*m.segThick, m.segLen, pattern & S14_BOT); // Bottom
    drawHSeg(x + m.segThick, midY, halfW - m.segThick/2, pattern & S14_ML);              // Middle-left
    drawHSeg(centerX + m.segThick/2, midY, halfW - m.segThick/2, pattern & S14_MR);      // Middle-right
    
    // Vertical segments - outer
    drawVSeg(x, y + m.segThick, m.segLen, pattern & S14_TL);                             // Top-left
    drawVSeg(x, y + m.segLen + 2*m.segThick, m.segLen, pattern & S14_BL);                // Bottom-left
    drawVSeg(x + m.segLen + m.segThick, y + m.segThick, m.segLen, pattern & S14_TR);     // Top-right
    drawVSeg(x + m.segLen + m.segThick, y + m.segLen + 2*m.segThick, m.segLen, pattern & S14_BR); // Bottom-right
    
    // Center vertical segments
    drawVSeg(centerX, y + m.segThick, m.segLen - m.segThick, pattern & S14_CT);          // Center-top
    drawVSeg(centerX, midY + m.segThick, m.segLen - m.segThick, pattern & S14_CB);       // Center-bottom
    
    // Diagonal segments
    int diagInset = m.segThick;
    drawDiag(x + diagInset, y + m.segThick + diagInset, 
             centerX - diagInset, midY - diagInset, pattern & S14_DTL);                   // Diag top-left
    drawDiag(centerX + diagInset, y + m.segThick + diagInset, 
             x + m.segLen + m.segThick - diagInset, midY - diagInset, pattern & S14_DTR); // Diag top-right
    drawDiag(x + diagInset, y + 2*m.segLen + m.segThick - diagInset, 
             centerX - diagInset, midY + m.segThick + diagInset, pattern & S14_DBL);      // Diag bottom-left
    drawDiag(centerX + diagInset, midY + m.segThick + diagInset, 
             x + m.segLen + m.segThick - diagInset, y + 2*m.segLen + m.segThick - diagInset, pattern & S14_DBR); // Diag bottom-right
    
    if (addDot) {
        int dotR = m.dot / 2 + 1;
        int dotX = x + m.digitW + dotR;
        int dotY = y + m.digitH - dotR;
        FILL_CIRCLE(dotX, dotY, dotR, onColor);
    }
}

int V1Display::draw14SegmentText(const char* text, int x, int y, float scale, uint16_t onColor, uint16_t offColor) {
    SegMetrics m = segMetrics(scale);
    int cursor = x;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c == '.') continue;
        bool hasDot = (i + 1 < len && text[i + 1] == '.');
        draw14SegmentDigit(cursor, y, scale, c, hasDot, onColor, offColor);
        cursor += m.digitW + m.spacing + (hasDot ? m.dot / 2 : 0);
        if (hasDot) ++i;
    }
    return cursor - x - m.spacing;
}

// Classic 7-segment bogey counter (original V1 style)
void V1Display::drawTopCounterClassic(char symbol, bool muted, bool showDot) {
#if defined(DISPLAY_WAVESHARE_349)
    const float scale = 2.2f;  // Match frequency counter size
#else
    const float scale = 2.0f;
#endif
    SegMetrics m = segMetrics(scale);
    int x = 12;
    int y = 10;

    // Clear the area and render the single-digit counter or mode letter
    FILL_RECT(x - 2, y - 2, m.digitW + m.dot + 12, m.digitH + 8, PALETTE_BG);

    char buf[3] = {symbol, 0, 0};
    if (showDot) {
        buf[1] = '.';
    }
    
    // Use bogey color for digits, muted color if muted, otherwise bogey color
    const V1Settings& s = settingsManager.get();
    bool isDigit = (symbol >= '0' && symbol <= '9');
    uint16_t color;
    if (isDigit) {
        color = s.colorBogey;
    } else {
        color = muted ? PALETTE_MUTED : s.colorBogey;
    }
    drawSevenSegmentText(buf, x, y, scale, color, PALETTE_BG);
}

// Modern Montserrat Bold bogey counter
void V1Display::drawTopCounterModern(char symbol, bool muted, bool showDot) {
    const V1Settings& s = settingsManager.get();
    
    // Special case: lowercase 'l' (logic mode) - draw as bottom half of 'L' like V1 display
    // Render full 'L' then mask off the top half for consistent styling
    if (symbol == 'l') {
        const int fontSize = 60;
        
        char buf[3] = {'L', 0, 0};
        if (showDot) {
            buf[1] = '.';
        }
        
        ofr.setFontSize(fontSize);
        
        // Convert RGB565 background to RGB888 for antialiasing blend
        uint8_t bgR = (PALETTE_BG >> 11) << 3;
        uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        ofr.setBackgroundColor(bgR, bgG, bgB);
        
        uint16_t color = muted ? PALETTE_MUTED : s.colorBogey;
        ofr.setFontColor((color >> 11) << 3, ((color >> 5) & 0x3F) << 2, (color & 0x1F) << 3);
        
        FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, buf);
        int textW = bbox.xMax - bbox.xMin;
        int textH = bbox.yMax - bbox.yMin;
        
        int x = 12;
        int y = textH - 50;  // Same baseline position as other characters
        
        // Clear full area first
        FILL_RECT(x - 2, 0, textW + 8, textH + 8, PALETTE_BG);
        
        // Render the full 'L.' 
        ofr.setCursor(x, y);
        ofr.printf("%s", buf);
        
        // Now mask off the top half by drawing a background rect over it
        // Top of text starts at screen Y=0 (since y = textH - 50 and text renders upward)
        int maskHeight = textH / 2;  // Cover top half of the 'L'
        FILL_RECT(x - 2, 0, textW + 8, maskHeight, PALETTE_BG);
        
        return;
    }
    
    // Convert lowercase mode letters to uppercase (font only has uppercase LASER)
    char upperSymbol = symbol;
    if (symbol >= 'a' && symbol <= 'z') {
        upperSymbol = symbol - 32;  // Convert to uppercase
    }
    
    char buf[3] = {upperSymbol, 0, 0};
    if (showDot) {
        buf[1] = '.';
    }
    
    // Check if symbol is in the OFR font subset (0-9, -, ., L, A, S, E, R)
    // Note: '=' (laser 3 bars) is NOT in the font, will use bitmap fallback
    bool charInOfrFont = (upperSymbol >= '0' && upperSymbol <= '9') || 
                         upperSymbol == '-' || upperSymbol == '.' ||
                         upperSymbol == 'L' || upperSymbol == 'A' || upperSymbol == 'S' || 
                         upperSymbol == 'E' || upperSymbol == 'R';
    
    // Fall back to Classic style if OFR not initialized or char not in font
    if (!ofrInitialized || !charInOfrFont) {
        // Use Classic 7-segment/14-segment rendering as fallback
        drawTopCounterClassic(symbol, muted, showDot);
        return;
    }
    
    // OpenFontRender antialiased rendering - same font as frequency for consistency
    const int fontSize = 60;  // Proportional to frequency (66)
    
    ofr.setFontSize(fontSize);
    
    // Convert RGB565 background to RGB888 for antialiasing blend
    uint8_t bgR = (PALETTE_BG >> 11) << 3;
    uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
    uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
    ofr.setBackgroundColor(bgR, bgG, bgB);
    
    bool isDigit = (symbol >= '0' && symbol <= '9');
    uint16_t color = isDigit ? s.colorBogey : (muted ? PALETTE_MUTED : s.colorBogey);
    ofr.setFontColor((color >> 11) << 3, ((color >> 5) & 0x3F) << 2, (color & 0x1F) << 3);
    
    FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, buf);
    int textW = bbox.xMax - bbox.xMin;
    int textH = bbox.yMax - bbox.yMin;
    
    int x = 12;
    int y = textH - 50;  // Moved up 50 pixels total from baseline
    
    // Clear area (text renders above baseline)
    FILL_RECT(x - 2, y - textH - 2, textW + 8, textH + 8, PALETTE_BG);
    
    ofr.setCursor(x, y);
    ofr.printf("%s", buf);
}

// Router: calls appropriate bogey counter draw method based on display style
void V1Display::drawTopCounter(char symbol, bool muted, bool showDot) {
    const V1Settings& s = settingsManager.get();
    if (s.displayStyle == DISPLAY_STYLE_MODERN) {
        drawTopCounterModern(symbol, muted, showDot);
    } else {
        drawTopCounterClassic(symbol, muted, showDot);
    }
}

void V1Display::drawMuteIcon(bool muted) {
    // Draw centered badge above frequency display
#if defined(DISPLAY_WAVESHARE_349)
    const float freqScale = 2.2f; 
#else
    const float freqScale = 1.7f;
#endif
    SegMetrics mFreq = segMetrics(freqScale);

    // Frequency Y position (from drawFrequency) - use effective height for multi-alert
    int freqY = getEffectiveScreenHeight() - mFreq.digitH - 8;
    const int rightMargin = 120;
    int maxWidth = SCREEN_WIDTH - rightMargin;
    
    // Badge dimensions (slightly smaller to avoid crowding top UI)
    int w = 92;
    int h = 26;
    int x = (maxWidth - w) / 2;
    // Keep badge low enough to avoid the top/bogey region when frequency is raised
    int y = freqY - h - 4;
    if (y < 40) y = 40;  // Minimum y to avoid overlapping mode/bogey counter
    
    if (muted) {
        // Draw badge with muted styling
        uint16_t outline = PALETTE_MUTED;
        uint16_t fill = PALETTE_MUTED;
        
        FILL_ROUND_RECT(x, y, w, h, 6, fill);
        DRAW_ROUND_RECT(x, y, w, h, 6, outline);
        
        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);  // Boost readability in compact badge
        TFT_CALL(setTextColor)(PALETTE_BG, fill);
        int cx = x + w / 2;
        int cy = y + h / 2 + 1;
        // Pseudo-bold: draw twice with slight offset
        GFX_drawString(tft, "MUTED", cx, cy);
        GFX_drawString(tft, "MUTED", cx + 1, cy);
    } else {
        // Clear the badge area when not muted
        FILL_RECT(x, y, w, h, PALETTE_BG);
    }
}

void V1Display::drawProfileIndicator(int slot) {
    // Get custom slot names and colors from settings
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();

    // Track current slot and timestamp for flash feature
    if (slot != lastProfileSlot) {
        lastProfileSlot = slot;
        profileChangedTime = millis();  // Record when profile changed
    }
    currentProfileSlot = slot;

    // Check if we're in the "flash" period after a profile change
    bool inFlashPeriod = (millis() - profileChangedTime) < HIDE_TIMEOUT_MS;
    
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
#if defined(DISPLAY_WAVESHARE_349)
    const float freqScale = 2.2f;
#else
    const float freqScale = 1.7f;
#endif
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
    setBLEProxyStatus(bleProxyEnabled, bleProxyClientConnected);
}

void V1Display::drawBatteryIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern BatteryManager batteryManager;
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();
    
    // Don't draw anything if no battery is present
    if (!batteryManager.hasBattery()) {
        return;
    }
    
    // Battery icon position - bottom left (use actual screen height, not effective)
    const int battX = 12;   // Align with bogey counter left edge
    const int battW = 24;   // Battery body width
    const int battH = 14;   // Battery body height
    const int battY = SCREEN_HEIGHT - battH - 8;  // Stay at actual bottom, not raised area
    
    // Check if user explicitly hides the battery icon
    if (s.hideBatteryIcon) {
        const int capW = 3;
        FILL_RECT(battX - 2, battY - 2, battW + capW + 6, battH + 4, PALETTE_BG);
        return;
    }
    
    const int capW = 3;     // Positive terminal cap width
    const int capH = 6;     // Positive terminal cap height
    const int padding = 2;  // Padding inside battery
    const int sections = 5; // Number of charge sections
    
    // Get battery percentage
    uint8_t pct = batteryManager.getPercentage();
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
    
    // Clear area
    FILL_RECT(battX - 2, battY - 2, battW + capW + 6, battH + 4, PALETTE_BG);
    
    uint16_t outlineColor = dimColor(PALETTE_TEXT);

    // Draw battery outline (dimmed)
    DRAW_RECT(battX, battY, battW, battH, outlineColor);  // Main body
    FILL_RECT(battX + battW, battY + (battH - capH) / 2, capW, capH, outlineColor);  // Positive cap
    
    // Draw charge sections
    int sectionW = (battW - 2 * padding - (sections - 1)) / sections;  // Width of each section with 1px gap
    for (int i = 0; i < sections; i++) {
        int sx = battX + padding + i * (sectionW + 1);
        int sy = battY + padding;
        int sh = battH - 2 * padding;
        
        if (i < filledSections) {
            FILL_RECT(sx, sy, sectionW, sh, dimColor(fillColor));
        }
    }
#endif
}

void V1Display::drawBLEProxyIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    // Stack above WiFi indicator to keep the left column compact
    // Use actual SCREEN_HEIGHT so icons stay at bottom, not raised area
    const int battH = 14;
    const int battY = SCREEN_HEIGHT - battH - 8;
    const int wifiSize = 20;
    const int wifiY = battY - wifiSize - 6;

    const int iconSize = 20;  // Match WiFi icon size
    const int bleX = 14;
    const int bleY = wifiY - iconSize - 6;

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

    // Icon color from settings: connected vs disconnected
    uint16_t btColor = bleProxyClientConnected ? dimColor(s.colorBleConnected, 85)
                                               : dimColor(s.colorBleDisconnected, 85);

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

void V1Display::drawWiFiIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern WiFiManager wifiManager;
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();
    
    // WiFi icon position - above battery icon, bottom left
    // Use actual SCREEN_HEIGHT so icons stay at bottom, not raised area
    const int wifiX = 14;
    const int wifiSize = 20;
    const int battY = SCREEN_HEIGHT - 14 - 8;
    const int wifiY = battY - wifiSize - 6;
    
    // Check if user explicitly hides the WiFi icon
    if (s.hideWifiIcon) {
        FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
        return;
    }
    
    // Show WiFi icon when Setup Mode (AP) is active
    bool isSetupMode = wifiManager.isSetupModeActive();
    
    if (!isSetupMode) {
        // Clear the WiFi icon area when Setup Mode is off
        FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
        return;
    }
    
    // Get WiFi icon color from settings (default cyan 0x07FF)
    uint16_t wifiColor = dimColor(s.colorWiFiIcon);
    
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

void V1Display::flush() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    if (tft) {
        tft->flush();
    }
#endif
}

void V1Display::showDisconnected() {
    drawBaseFrame();
    drawStatusText("Disconnected", PALETTE_KA);
    drawWiFiIndicator();
    drawBatteryIndicator();
}

void V1Display::showResting() {
    // Align layout with multi-alert positioning if enabled
    const V1Settings& s = settingsManager.get();
    g_multiAlertMode = s.enableMultiAlert;
    multiAlertMode = false;

    // Avoid redundant full-screen clears/flushes when already resting and nothing changed
    bool paletteChanged = (lastRestingPaletteRevision != paletteRevision);
    bool screenChanged = (currentScreen != ScreenMode::Resting);
    int profileSlot = currentProfileSlot;
    bool profileChanged = (profileSlot != lastRestingProfileSlot);
    
    if (screenChanged || paletteChanged) {
        // Full redraw when coming from another screen or after theme change
        TFT_CALL(fillScreen)(PALETTE_BG);
        drawBaseFrame();
        
        // Draw idle state: dimmed UI elements showing V1 is ready
        // Top counter showing "0" (no bogeys)
        drawTopCounter('0', false, true);
        
        // Band indicators all dimmed (no active bands)
        drawBandIndicators(0, false);
        
        // Signal bars all empty
        drawVerticalSignalBars(0, 0, BAND_KA, false);
        
        // Direction arrows all dimmed
        drawDirectionArrow(DIR_NONE, false);
        
        // Frequency display showing dashes
        drawFrequency(0, false);
        
        // Mute indicator off
        drawMuteIcon(false);
        
        // Profile indicator
        drawProfileIndicator(profileSlot);

        lastRestingPaletteRevision = paletteRevision;
        lastRestingProfileSlot = profileSlot;
        currentScreen = ScreenMode::Resting;

#if defined(DISPLAY_USE_ARDUINO_GFX)
        tft->flush();
#endif
    } else if (profileChanged) {
        // Only the profile changed while already resting; redraw just the indicator
        drawProfileIndicator(profileSlot);
        lastRestingProfileSlot = profileSlot;
#if defined(DISPLAY_USE_ARDUINO_GFX)
        tft->flush();
#endif
    }

    // Reset lastState so next update() detects changes from this "resting" state
    lastState = DisplayState();  // All defaults: bands=0, arrows=0, bars=0, hasMode=false, modeChar=0
}

void V1Display::showScanning() {
    // Align layout with multi-alert default positioning if enabled
    const V1Settings& s = settingsManager.get();
    g_multiAlertMode = s.enableMultiAlert;

    // Clear and draw the base frame
    TFT_CALL(fillScreen)(PALETTE_BG);
    drawBaseFrame();
    
    // Draw idle state elements
    drawTopCounter('0', false, true);
    drawBandIndicators(0, false);
    drawVerticalSignalBars(0, 0, BAND_KA, false);
    drawDirectionArrow(DIR_NONE, false);
    drawMuteIcon(false);
    drawProfileIndicator(currentProfileSlot);
    
    // Draw "SCAN" in frequency area - match display style
    if (s.displayStyle == DISPLAY_STYLE_MODERN && ofrInitialized) {
        // Modern style: use Montserrat Bold via OFR
        const int fontSize = 66;
        ofr.setFontColor(s.colorBandKa, PALETTE_BG);
        ofr.setFontSize(fontSize);
        
        const char* text = "SCAN";
        FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
        int textWidth = bbox.xMax - bbox.xMin;
        int textHeight = bbox.yMax - bbox.yMin;
        
        // Center in frequency area (left of right panel)
        const int rightMargin = 120;
        int maxWidth = SCREEN_WIDTH - rightMargin;
        int x = (maxWidth - textWidth) / 2;
        int y = getEffectiveScreenHeight() - 72;  // Match frequency positioning
        
        FILL_RECT(x - 4, y - textHeight - 4, textWidth + 8, textHeight + 12, PALETTE_BG);
        ofr.setCursor(x, y);
        ofr.printf("%s", text);
    } else {
        // Classic style: use 14-segment display
#if defined(DISPLAY_WAVESHARE_349)
        const float scale = 2.2f;
#else
        const float scale = 1.7f;
#endif
        SegMetrics m = segMetrics(scale);
        int y = getEffectiveScreenHeight() - m.digitH - 8;
        
        const char* text = "SCAN";
        int width = measureSevenSegmentText("00.000", scale);
        const int rightMargin = 120;
        int maxWidth = SCREEN_WIDTH - rightMargin;
        int x = (maxWidth - width) / 2;
        if (x < 0) x = 0;
        
        FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
        draw14SegmentText(text, x, y, scale, s.colorBandKa, PALETTE_BG);
    }
    
    // Reset lastState
    lastState = DisplayState();
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->flush();
#endif

    currentScreen = ScreenMode::Scanning;
    lastRestingProfileSlot = -1;
}

void V1Display::showDemo() {
        TFT_CALL(fillScreen)(PALETTE_BG); // Clear screen to prevent artifacts
    clear();

    // Simulate KA alert at 35.505 GHz from front and rear to show stacked arrows
    AlertData demoAlert;
    demoAlert.band = BAND_KA;
    demoAlert.direction = DIR_FRONT; // demo a front alert only; others stay greyed
    demoAlert.frontStrength = 6;  // Strong signal (max)
    demoAlert.rearStrength = 0;
    demoAlert.frequency = 35500;  // MHz (35.500 GHz)
    demoAlert.isValid = true;

    // Draw the alert
    update(demoAlert, false); // keep colors bright on demo (will draw bogie 1 internally)
    lastState.signalBars = 1; // keep internal state consistent with the demo counter
    
    // Also draw profile indicator and WiFi icon during demo so user can see hide toggle effect
    drawProfileIndicator(0);  // Show slot 0 profile indicator (unless hidden)
    drawWiFiIndicator();      // Show WiFi icon (unless hidden)
}

void V1Display::showBootSplash() {
    TFT_CALL(fillScreen)(PALETTE_BG); // Clear screen to prevent artifacts
    drawBaseFrame();

    // Draw the V1 Simple logo at 1:1 (image is pre-sized to 640x172)
    for (int sy = 0; sy < V1SIMPLE_LOGO_HEIGHT; sy++) {
        for (int sx = 0; sx < V1SIMPLE_LOGO_WIDTH; sx++) {
            uint16_t pixel = pgm_read_word(&v1simple_logo_rgb565[sy * V1SIMPLE_LOGO_WIDTH + sx]);
            TFT_CALL(drawPixel)(sx, sy, pixel);
        }
    }

#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Flush canvas to display before enabling backlight
    tft->flush();
#endif

    // Turn on backlight now that splash is drawn
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Waveshare 3.49" has INVERTED backlight: 0=full on, 255=off
    analogWrite(LCD_BL, 0);  // Full brightness (inverted)
#else
    digitalWrite(TFT_BL, HIGH);
#endif
    Serial.println("Backlight ON (post-splash, inverted)");
}

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
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->flush();
#endif
}

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
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->flush();
#endif
}

void V1Display::drawStatusText(const char* text, uint16_t color) {
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextSize)(2);
    GFX_drawString(tft, text, SCREEN_WIDTH / 2, SCREEN_HEIGHT / 2);
}

Band V1Display::pickDominantBand(uint8_t bandMask) {
    if (bandMask & BAND_KA) return BAND_KA;
    if (bandMask & BAND_K) return BAND_K;
    if (bandMask & BAND_X) return BAND_X;
    if (bandMask & BAND_LASER) return BAND_LASER;
    return BAND_NONE;
}

void V1Display::drawBandLabel(Band band, bool muted) {
    const char* label = (band == BAND_NONE) ? "--" : bandToString(band);
    GFX_setTextDatum(TL_DATUM);
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(muted ? PALETTE_MUTED : PALETTE_ARROW, PALETTE_BG);
    GFX_drawString(tft, label, 10, SCREEN_HEIGHT / 2 - 26);
}

void V1Display::update(const DisplayState& state) {
    static bool firstUpdate = true;
    static bool wasInFlashPeriod = false;
    
    // Align layout with multi-alert positioning if enabled
    const V1Settings& s = settingsManager.get();
    g_multiAlertMode = s.enableMultiAlert;
    multiAlertMode = false;  // No cards to draw in resting state
    
    // Check if profile flash period just expired (needs redraw to clear)
    bool inFlashPeriod = (millis() - profileChangedTime) < HIDE_TIMEOUT_MS;
    bool flashJustExpired = wasInFlashPeriod && !inFlashPeriod;
    wasInFlashPeriod = inFlashPeriod;

    // Override mute when laser active or strong signal present
    bool effectiveMuted = state.muted;
    bool laserActive = (state.activeBands & BAND_LASER) != 0;
    if (laserActive) {
        effectiveMuted = false;
    } else if (effectiveMuted && state.signalBars >= STRONG_SIGNAL_UNMUTE_THRESHOLD) {
        effectiveMuted = false;
    }

    bool stateChanged =
        firstUpdate ||
        flashJustExpired ||
        state.activeBands != lastState.activeBands ||
        state.arrows != lastState.arrows ||
        state.signalBars != lastState.signalBars ||
        effectiveMuted != lastState.muted ||
        state.modeChar != lastState.modeChar ||
        state.hasMode != lastState.hasMode;

    if (stateChanged) {
        firstUpdate = false;
        drawBaseFrame();
        char topChar = state.hasMode ? state.modeChar : '0';
        drawTopCounter(topChar, effectiveMuted, true);  // Always show dot
        drawBandIndicators(state.activeBands, effectiveMuted);
        // BLE proxy status indicator
        
        // Check if laser is active from display state
        bool isLaser = (state.activeBands & BAND_LASER) != 0;
        drawFrequency(0, isLaser, effectiveMuted);
        
        // Determine primary band for signal bar coloring
        Band primaryBand = BAND_KA; // default
        if (state.activeBands & BAND_LASER) primaryBand = BAND_LASER;
        else if (state.activeBands & BAND_KA) primaryBand = BAND_KA;
        else if (state.activeBands & BAND_K) primaryBand = BAND_K;
        else if (state.activeBands & BAND_X) primaryBand = BAND_X;
        
        drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
        drawDirectionArrow(state.arrows, effectiveMuted);
        drawMuteIcon(effectiveMuted);
        drawProfileIndicator(currentProfileSlot);

#if defined(DISPLAY_WAVESHARE_349)
        tft->flush();  // Push canvas to display
#endif

        lastState = state;            // Preserve actual user mute state
    }
}

void V1Display::update(const AlertData& alert, bool mutedFlag) {
    if (!alert.isValid) {
        return;
    }
    
    // Align layout with multi-alert positioning if enabled (consistent with other screens)
    const V1Settings& s = settingsManager.get();
    g_multiAlertMode = s.enableMultiAlert;
    multiAlertMode = false;  // No cards to draw in legacy single-alert mode

    // Override mute when laser active or strong signal present during alert
    uint8_t strength = std::max(alert.frontStrength, alert.rearStrength);
    bool effectiveMuted = mutedFlag;
    if (alert.band == BAND_LASER) {
        effectiveMuted = false;
    } else if (effectiveMuted && strength >= STRONG_SIGNAL_UNMUTE_THRESHOLD) {
        effectiveMuted = false;
    }

    // Always redraw for clean display
    drawBaseFrame();

    uint8_t bandMask = alert.band;
    drawTopCounter('1', effectiveMuted, true); // bogey counter shows 1 during alert
    drawFrequency(alert.frequency, alert.band == BAND_LASER, effectiveMuted);
    drawBandIndicators(bandMask, effectiveMuted);
    drawVerticalSignalBars(alert.frontStrength, alert.rearStrength, alert.band, effectiveMuted);
    drawDirectionArrow(alert.direction, effectiveMuted);
    drawMuteIcon(effectiveMuted);
    drawProfileIndicator(currentProfileSlot);

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();  // Push canvas to display
#endif

    lastAlert = alert;
    lastState.activeBands = bandMask;
    lastState.arrows = alert.direction;
    lastState.signalBars = std::max(alert.frontStrength, alert.rearStrength);
    lastState.muted = mutedFlag;  // Preserve actual mute flag; UI used effectiveMuted
}

void V1Display::update(const AlertData& alert) {
    // Preserve legacy call sites by using the last known muted flag
    update(alert, lastState.muted);
}

// Persisted alert display - shows last alert in dark grey after V1 clears it
// Only draws frequency, band, and arrows - no signal bars, no mute badge
// Bogey counter shows V1 mode (from state), not "1"
void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    if (!alert.isValid) {
        update(state);  // Fall back to normal resting display
        return;
    }
    
    // Derive persisted color from muted - darken by ~50%
    auto darkenColor = [](uint16_t color) -> uint16_t {
        uint8_t r = ((color >> 11) & 0x1F) / 2;
        uint8_t g = ((color >> 5) & 0x3F) / 2;
        uint8_t b = (color & 0x1F) / 2;
        return (r << 11) | (g << 5) | b;
    };
    
    // Align layout with multi-alert positioning if enabled (consistent with alert screens)
    const V1Settings& s = settingsManager.get();
    g_multiAlertMode = s.enableMultiAlert;
    multiAlertMode = false;  // No cards to draw
    wasInMultiAlertMode = false;
    
    drawBaseFrame();
    
    // Bogey counter shows V1 mode (truth from V1), not alert count
    char topChar = state.hasMode ? state.modeChar : '0';
    drawTopCounter(topChar, true, true);  // muted=true for grey styling
    
    // Band indicator in dark grey
    uint8_t bandMask = alert.band;
    drawBandIndicators(bandMask, true);  // muted=true for grey styling
    
    // Frequency in dark grey (pass muted=true)
    drawFrequency(alert.frequency, alert.band == BAND_LASER, true);
    
    // No signal bars - just draw empty
    drawVerticalSignalBars(0, 0, alert.band, true);
    
    // Arrows in dark grey
    drawDirectionArrow(alert.direction, true);  // muted=true for grey
    
    // No mute badge
    // drawMuteIcon intentionally skipped
    
    // Profile indicator still shown
    drawProfileIndicator(currentProfileSlot);

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();
#endif
}

void V1Display::update(const AlertData& alert, const DisplayState& state, int alertCount) {
    if (!alert.isValid) {
        return;
    }

    // Track mode transitions - force redraw when switching from multi to single alert
    bool modeChanged = wasInMultiAlertMode;  // If we were in multi-mode, force redraw
    wasInMultiAlertMode = false;  // We're now in single-alert mode
    
    // Keep frequency raised when multi-alert is enabled (consistent position)
    const V1Settings& s = settingsManager.get();
    g_multiAlertMode = s.enableMultiAlert;  // Keep raised if multi-alert enabled
    multiAlertMode = false;  // But no cards to draw

    // Change detection: skip redraw if nothing meaningful changed
    // (signal strength fluctuates constantly, so exclude it)
    // Only compare values we actually draw - state.arrows, not alert.direction
    static AlertData lastSingleAlert;
    static DisplayState lastSingleState;
    static int lastSingleCount = 0;
    
    bool needsRedraw = modeChanged ||
        alert.frequency != lastSingleAlert.frequency ||
        alert.band != lastSingleAlert.band ||
        alertCount != lastSingleCount ||
        state.activeBands != lastSingleState.activeBands ||
        state.arrows != lastSingleState.arrows ||
        state.signalBars != lastSingleState.signalBars ||
        state.muted != lastSingleState.muted;
    
    if (!needsRedraw) {
        return;  // No change, skip redraw
    }
    
    // Store for next comparison
    lastSingleAlert = alert;
    lastSingleState = state;
    lastSingleCount = alertCount;

    drawBaseFrame();

    // Use activeBands from display state (all detected bands), not just priority alert band
    uint8_t bandMask = state.activeBands;
    
    // Show '=' (3 horizontal bars) for laser alerts, otherwise show bogey count (clamp to single digit, use '9' for 9+)
    // In Modern style, skip drawing the useless '=' for laser
    const V1Settings& settings = settingsManager.get();
    char countChar;
    bool skipTopCounter = false;
    if (alert.band == BAND_LASER) {
        if (settings.displayStyle == DISPLAY_STYLE_MODERN) {
            skipTopCounter = true;  // Don't draw anything for laser in modern
        } else {
            countChar = '=';  // 3 horizontal bars like official V1 (classic only)
        }
    } else {
        countChar = (alertCount > 9) ? '9' : ('0' + alertCount);
    }
    if (!skipTopCounter) {
        drawTopCounter(countChar, state.muted, true);
    }
    
    // Frequency from priority alert
    drawFrequency(alert.frequency, alert.band == BAND_LASER, state.muted);
    
    // Use bands from display state for the indicators
    drawBandIndicators(bandMask, state.muted);
    
    // Signal bars from display state (max across all alerts, calculated in packet_parser)
    drawVerticalSignalBars(state.signalBars, state.signalBars, alert.band, state.muted);
    
    // Direction from display state (shows all active arrows)
    drawDirectionArrow(state.arrows, state.muted);
    
    drawMuteIcon(state.muted);
    drawProfileIndicator(currentProfileSlot);

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();  // Push canvas to display
#endif

    lastAlert = alert;
    lastState = state;
}

// Multi-alert update: draws priority alert with secondary alert cards below
void V1Display::update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state) {
    if (!priority.isValid) {
        return;
    }
    
    // Check if multi-alert display is enabled in settings
    const V1Settings& s = settingsManager.get();
    
    // If only 1 alert or multi-alert disabled, use standard display (no cards row)
    if (alertCount <= 1 || !s.enableMultiAlert) {
        // Keep frequency raised when multi-alert is enabled (consistent position)
        g_multiAlertMode = s.enableMultiAlert;
        multiAlertMode = false;  // No cards to draw
        update(priority, state, alertCount);
        return;
    }

    // Enable multi-alert mode to shift main content up
    bool wasMultiAlertMode = g_multiAlertMode;
    g_multiAlertMode = true;
    multiAlertMode = true;
    
    // Track that we're in multi-alert mode for transition detection
    wasInMultiAlertMode = true;

    // Change detection: check if we need to redraw
    // Exclude arrows - they fluctuate rapidly with multiple alerts as V1 switches focus
    static AlertData lastPriority;
    static int lastAlertCount = 0;
    static DisplayState lastMultiState;
    
    bool needsRedraw = false;
    
    if (!wasMultiAlertMode) { needsRedraw = true; }
    else if (priority.frequency != lastPriority.frequency) { needsRedraw = true; }
    else if (priority.band != lastPriority.band) { needsRedraw = true; }
    else if (alertCount != lastAlertCount) { needsRedraw = true; }
    else if (state.activeBands != lastMultiState.activeBands) { needsRedraw = true; }
    else if (state.muted != lastMultiState.muted) { needsRedraw = true; }
    
    // Also check if any secondary alert changed (but not signal strength or direction - those fluctuate)
    static AlertData lastSecondary[4];
    if (!needsRedraw) {
        for (int i = 0; i < alertCount && i < 4; i++) {
            if (allAlerts[i].frequency != lastSecondary[i].frequency ||
                allAlerts[i].band != lastSecondary[i].band) {
                needsRedraw = true;
                break;
            }
        }
    }
    
    // Track arrow and signal bar changes separately for incremental update
    static uint8_t lastArrows = 0;
    static uint8_t lastSignalBars = 0;
    bool arrowsChanged = (state.arrows != lastArrows);
    bool signalBarsChanged = (state.signalBars != lastSignalBars);
    
    if (!needsRedraw && !arrowsChanged && !signalBarsChanged) {
        return;  // Nothing changed at all
    }
    
    if (!needsRedraw && (arrowsChanged || signalBarsChanged)) {
        // Only arrows and/or signal bars changed - do incremental update without full redraw
        if (arrowsChanged) {
            lastArrows = state.arrows;
            drawDirectionArrow(state.arrows, state.muted);
        }
        if (signalBarsChanged) {
            lastSignalBars = state.signalBars;
            drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
        }
#if defined(DISPLAY_WAVESHARE_349)
        tft->flush();
#endif
        return;
    }
    
    // Full redraw needed - store current state for next comparison
    lastPriority = priority;
    lastAlertCount = alertCount;
    lastMultiState = state;
    lastArrows = state.arrows;
    lastSignalBars = state.signalBars;
    for (int i = 0; i < alertCount && i < 4; i++) {
        lastSecondary[i] = allAlerts[i];
    }
    
    drawBaseFrame();

    // Use activeBands from display state
    uint8_t bandMask = state.activeBands;
    
    // Bogey counter
    const V1Settings& settings = settingsManager.get();
    char countChar;
    bool skipTopCounter = false;
    if (priority.band == BAND_LASER) {
        if (settings.displayStyle == DISPLAY_STYLE_MODERN) {
            skipTopCounter = true;
        } else {
            countChar = '=';
        }
    } else {
        countChar = (alertCount > 9) ? '9' : ('0' + alertCount);
    }
    if (!skipTopCounter) {
        drawTopCounter(countChar, state.muted, true);
    }
    
    // Main alert display (frequency, bands, arrows, signal bars)
    // Use state.signalBars which is the MAX across ALL alerts (calculated in packet_parser)
    drawFrequency(priority.frequency, priority.band == BAND_LASER, state.muted);
    drawBandIndicators(bandMask, state.muted);
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    drawDirectionArrow(state.arrows, state.muted);
    drawMuteIcon(state.muted);
    drawProfileIndicator(currentProfileSlot);
    
    // Draw secondary alert cards at bottom
    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
    
    // Keep g_multiAlertMode true while in multi-alert - only reset when going to single-alert mode

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();
#endif

    lastAlert = priority;
    lastState = state;
}

// Draw mini alert cards for secondary (non-priority) alerts
void V1Display::drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const int cardH = SECONDARY_ROW_HEIGHT;  // Full height, no margin
    const int cardY = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Flush to bottom
    const int maxCards = 2;  // Show up to 2 secondary cards (primary + 2 = 3 alerts total)
    const int cardW = 140;   // Fit between icons and signal bars
    const int cardSpacing = 6;
    const int startX = 50;   // Start after battery/WiFi/BLE icon area
    static constexpr unsigned long CARD_GRACE_MS = 2000;  // 2 second grace period
    
    // Static tracking for grace period - each slot remembers its last alert
    static struct {
        AlertData alert{};
        unsigned long lastSeen = 0;
    } cardSlots[3];
    
    // Helper lambda to match alerts (same band and similar frequency)
    auto alertsMatch = [](const AlertData& a, const AlertData& b) -> bool {
        if (a.band != b.band) return false;
        if (a.frequency > 0 && b.frequency > 0) {
            uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
            return diff < 50;
        }
        return true;
    };
    
    // Collect current secondary alerts (skip the priority alert)
    AlertData currentSecondary[maxCards];
    int currentCount = 0;
    
    for (int i = 0; i < alertCount && currentCount < maxCards; i++) {
        if (alerts[i].frequency == priority.frequency && 
            alerts[i].band == priority.band &&
            alerts[i].direction == priority.direction) {
            continue;
        }
        if (alerts[i].isValid && alerts[i].band != BAND_NONE) {
            currentSecondary[currentCount++] = alerts[i];
        }
    }
    
    unsigned long now = millis();
    
    // Update card slots: refresh lastSeen for alerts still present, keep grace period for dropped ones
    for (int slot = 0; slot < maxCards; slot++) {
        bool stillActive = false;
        
        // Check if this slot's alert is still in current alerts
        if (cardSlots[slot].alert.isValid && cardSlots[slot].alert.band != BAND_NONE) {
            for (int c = 0; c < currentCount; c++) {
                if (alertsMatch(cardSlots[slot].alert, currentSecondary[c])) {
                    stillActive = true;
                    cardSlots[slot].lastSeen = now;
                    break;
                }
            }
        }
        
        // Clear slot if grace period expired
        if (!stillActive && cardSlots[slot].lastSeen > 0 && (now - cardSlots[slot].lastSeen) > CARD_GRACE_MS) {
            cardSlots[slot].alert = AlertData();
            cardSlots[slot].lastSeen = 0;
        }
    }
    
    // Assign new alerts to empty slots
    for (int c = 0; c < currentCount; c++) {
        bool found = false;
        for (int slot = 0; slot < maxCards; slot++) {
            if (cardSlots[slot].alert.isValid && alertsMatch(cardSlots[slot].alert, currentSecondary[c])) {
                // Update existing slot with current data
                cardSlots[slot].alert = currentSecondary[c];
                cardSlots[slot].lastSeen = now;
                found = true;
                break;
            }
        }
        if (!found) {
            // Find empty slot
            for (int slot = 0; slot < maxCards; slot++) {
                if (!cardSlots[slot].alert.isValid || cardSlots[slot].alert.band == BAND_NONE) {
                    cardSlots[slot].alert = currentSecondary[c];
                    cardSlots[slot].lastSeen = now;
                    break;
                }
            }
        }
    }
    
    // Change detection for secondary cards - only redraw if visible cards changed
    static struct {
        uint32_t frequency;
        Band band;
        bool valid;
    } lastDrawnCards[3] = {};
    static int lastDrawnCount = 0;
    static bool lastMuted = false;
    
    // Count how many valid cards we'll draw
    int willDrawCount = 0;
    for (int slot = 0; slot < maxCards; slot++) {
        if (cardSlots[slot].alert.isValid && cardSlots[slot].alert.band != BAND_NONE && cardSlots[slot].lastSeen > 0) {
            willDrawCount++;
        }
    }
    
    // Check if anything changed (or if screen was cleared)
    bool cardsChanged = secondaryCardsNeedRedraw || (willDrawCount != lastDrawnCount) || (muted != lastMuted);
    if (!cardsChanged) {
        int checkIdx = 0;
        for (int slot = 0; slot < maxCards && !cardsChanged; slot++) {
            if (cardSlots[slot].alert.isValid && cardSlots[slot].alert.band != BAND_NONE && cardSlots[slot].lastSeen > 0) {
                if (checkIdx >= 3 ||
                    cardSlots[slot].alert.frequency != lastDrawnCards[checkIdx].frequency ||
                    cardSlots[slot].alert.band != lastDrawnCards[checkIdx].band) {
                    cardsChanged = true;
                }
                checkIdx++;
            }
        }
    }
    
    if (!cardsChanged) {
        return;  // Secondary cards unchanged, skip redraw
    }
    
    // Clear the force-redraw flag
    secondaryCardsNeedRedraw = false;
    
    // Update last drawn state
    lastDrawnCount = willDrawCount;
    lastMuted = muted;
    int storeIdx = 0;
    for (int slot = 0; slot < maxCards; slot++) {
        if (cardSlots[slot].alert.isValid && cardSlots[slot].alert.band != BAND_NONE && cardSlots[slot].lastSeen > 0) {
            if (storeIdx < 3) {
                lastDrawnCards[storeIdx].frequency = cardSlots[slot].alert.frequency;
                lastDrawnCards[storeIdx].band = cardSlots[slot].alert.band;
                lastDrawnCards[storeIdx].valid = true;
                storeIdx++;
            }
        }
    }
    for (; storeIdx < 3; storeIdx++) {
        lastDrawnCards[storeIdx].valid = false;
    }
    
    // Clear the secondary row area (start after icon column, end before signal bars)
    const int iconAreaWidth = 48;  // Battery + margin
    const int signalBarsX = SCREEN_WIDTH - 228 - 2;  // signal bars startX with padding
    const int clearWidth = signalBarsX - iconAreaWidth;
    if (clearWidth > 0) {
        FILL_RECT(iconAreaWidth, cardY, clearWidth, SECONDARY_ROW_HEIGHT, PALETTE_BG);
    }
    
    // Draw cards for all valid slots (live or within grace period)
    int drawnCount = 0;
    for (int slot = 0; slot < maxCards && drawnCount < maxCards; slot++) {
        if (!cardSlots[slot].alert.isValid || cardSlots[slot].alert.band == BAND_NONE) continue;
        if (cardSlots[slot].lastSeen == 0) continue;
        
        const AlertData& alert = cardSlots[slot].alert;
        bool isGraced = (now - cardSlots[slot].lastSeen) > 0;  // Any age means it's being held
        bool drawMuted = muted || (isGraced && (now - cardSlots[slot].lastSeen) > 100);  // Grace cards go grey after 100ms
        
        int cardX = startX + drawnCount * (cardW + cardSpacing);
        drawnCount++;
        
        // Card background with band color tint (grey when muted)
        uint16_t bandCol = drawMuted ? PALETTE_MUTED : getBandColor(alert.band);
        uint8_t r = ((bandCol >> 11) & 0x1F) * (drawMuted ? 2 : 3) / 10;
        uint8_t g = ((bandCol >> 5) & 0x3F) * (drawMuted ? 2 : 3) / 10;
        uint8_t b = (bandCol & 0x1F) * (drawMuted ? 2 : 3) / 10;
        uint16_t bgCol = (r << 11) | (g << 5) | b;
        
        FILL_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bgCol);
        DRAW_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bandCol);
        
        // Draw direction arrow
        int arrowX = cardX + 18;
        int arrowCY = cardY + cardH / 2;
        uint16_t arrowCol = drawMuted ? PALETTE_MUTED : TFT_WHITE;
        
        if (alert.direction & DIR_FRONT) {
            int aw = 14, ah = 16;
            tft->fillTriangle(arrowX, arrowCY - ah/2, 
                             arrowX - aw/2, arrowCY + ah/2,
                             arrowX + aw/2, arrowCY + ah/2, arrowCol);
        } else if (alert.direction & DIR_REAR) {
            int aw = 14, ah = 16;
            tft->fillTriangle(arrowX, arrowCY + ah/2,
                             arrowX - aw/2, arrowCY - ah/2,
                             arrowX + aw/2, arrowCY - ah/2, arrowCol);
        } else if (alert.direction & DIR_SIDE) {
            FILL_RECT(arrowX - 8, arrowCY - 3, 16, 6, arrowCol);
        }
        
        // Frequency
        char freqStr[10];
        if (alert.band == BAND_LASER) {
            snprintf(freqStr, sizeof(freqStr), "LASER");
        } else if (alert.frequency > 0) {
            snprintf(freqStr, sizeof(freqStr), "%.3f", alert.frequency / 1000.0f);
        } else {
            snprintf(freqStr, sizeof(freqStr), "---");
        }
        tft->setTextColor(drawMuted ? PALETTE_MUTED : TFT_WHITE);
        tft->setTextSize(2);
        tft->setCursor(cardX + 32, cardY + (cardH - 16) / 2);
        tft->print(freqStr);
        
        // Band label
        tft->setTextColor(bandCol);
        tft->setTextSize(2);
        const char* bandStr = bandToString(alert.band);
        int bandW = strlen(bandStr) * 12;
        tft->setCursor(cardX + cardW - bandW - 6, cardY + (cardH - 16) / 2);
        tft->print(bandStr);
    }
#endif
}

void V1Display::drawBandBadge(Band band) {
    if (band == BAND_NONE) {
        return;
    }
    // Small rounded badge at top-left with active band text
    int bx = 14;
    int by = 10;
    int bw = 60;
    int bh = 22;
    uint16_t col = getBandColor(band);
    FILL_ROUND_RECT(bx, by, bw, bh, 4, col);
    DRAW_ROUND_RECT(bx, by, bw, bh, 4, TFT_WHITE);
    GFX_setTextDatum(MC_DATUM);
    TFT_CALL(setTextColor)(TFT_WHITE, col);
    TFT_CALL(setTextSize)(2);
    const char* txt = bandToString(band);
    GFX_drawString(tft, txt, bx + bw/2, by + bh/2 + 1);
}

void V1Display::drawBandIndicators(uint8_t bandMask, bool muted) {
    // Vertical L/Ka/K/X stack using FreeSansBold 24pt font for crisp look
#if defined(DISPLAY_WAVESHARE_349)
    const int x = 82;
    const int textSize = 1;   // No scaling - native 24pt for crisp rendering
    const int spacing = 43;   // Increased spacing to spread labels vertically
    const int startY = 55;    // Start position for L (moved down for 24pt)
#else
    const int x = 82;
    const int textSize = 1;   // No scaling
    const int spacing = 30;   // Tighten spacing
    const int startY = 30;    // Start position
#endif

    const V1Settings& s = settingsManager.get();
    struct BandCell {
        const char* label;
        uint8_t mask;
        uint16_t color;
    } cells[4] = {
        {"L",  BAND_LASER, s.colorBandL},
        {"Ka", BAND_KA,   s.colorBandKa},
        {"K",  BAND_K,    s.colorBandK},
        {"X",  BAND_X,    s.colorBandX}
    };

    // Use 24pt font for crisp band labels (no scaling)
    TFT_CALL(setFont)(&FreeSansBold24pt7b);
    TFT_CALL(setTextSize)(textSize);
    GFX_setTextDatum(ML_DATUM);
    
    for (int i = 0; i < 4; ++i) {
        bool active = (bandMask & cells[i].mask) != 0;
        uint16_t col = active ? (muted ? PALETTE_MUTED : cells[i].color) : TFT_DARKGREY;
        TFT_CALL(setTextColor)(col, PALETTE_BG);
        GFX_drawString(tft, cells[i].label, x, startY + i * spacing);
    }
    
    // Reset to default font for other text
    TFT_CALL(setFont)(NULL);
    TFT_CALL(setTextSize)(1);
}

void V1Display::drawSignalBars(uint8_t bars) {
    if (bars > MAX_SIGNAL_BARS) {
        bars = MAX_SIGNAL_BARS;
    }
    
    // Keep standard sizing/placement even in raised layout to avoid visual shifts
    int barWidth = BAR_WIDTH;
    int barHeight = BAR_HEIGHT;
    int barSpacing = BAR_SPACING;
    
    int startX = (SCREEN_WIDTH - (MAX_SIGNAL_BARS * (barWidth + barSpacing))) / 2;
    // Keep bars at the standard baseline (no vertical shift in multi-alert)
    int barsY = BARS_Y;
    
    for (uint8_t i = 0; i < MAX_SIGNAL_BARS; i++) {
        int x = startX + i * (barWidth + barSpacing);
        int height = barHeight * (i + 1) / MAX_SIGNAL_BARS;
        int y = barsY + (barHeight - height);
        
        if (i < bars) {
            // Draw filled bar
            FILL_RECT(x, y, barWidth, height, PALETTE_SIGNAL_BAR);
        } else {
            // Draw empty bar outline
            DRAW_RECT(x, y, barWidth, height, TFT_DARKGREY);
            FILL_RECT(x + 1, y + 1, barWidth - 2, height - 2, PALETTE_BG);
        }
    }
}

// Classic 7-segment frequency display (original V1 style)
void V1Display::drawFrequencyClassic(uint32_t freqMHz, bool isLaser, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const float scale = 2.2f; // Larger for wider screen
#else
    const float scale = 1.7f; // ~15% smaller than the counter digits
#endif
    SegMetrics m = segMetrics(scale);
    
    // Position frequency at bottom with proper margin (shifted up in multi-alert mode)
    int y = getEffectiveScreenHeight() - m.digitH - 8;
    
    if (isLaser) {
        // Draw "LASER" centered with margin for arrows on right - use 14-segment for proper 'R'
        const char* laserStr = "LASER";
        int width = measureSevenSegmentText(laserStr, scale);  // measurement is same for both
        const int rightMargin = 120;
        int maxWidth = SCREEN_WIDTH - rightMargin;
        int x = (maxWidth - width) / 2;
        if (x < 0) x = 0;
        
        // Clear area before drawing
        FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
        // Use muted grey for LASER when muted; otherwise custom laser color
        const V1Settings& set = settingsManager.get();
        draw14SegmentText(laserStr, x, y, scale, muted ? PALETTE_MUTED : set.colorBandL, PALETTE_BG);
        return;
    }

    bool hasFreq = freqMHz > 0;
    char freqStr[16];
    if (hasFreq) {
        float freqGhz = freqMHz / 1000.0f;
        snprintf(freqStr, sizeof(freqStr), "%05.3f", freqGhz); // XX.XXX layout
    } else {
        snprintf(freqStr, sizeof(freqStr), "--.---");
    }

    int width = measureSevenSegmentText(freqStr, scale);
    const int rightMargin = 120; // leave room on the right for the arrow stack
    int maxWidth = SCREEN_WIDTH - rightMargin;
    int x = (maxWidth - width) / 2;
    if (x < 0) x = 0;
    
    // Clear area before drawing
    const V1Settings& s = settingsManager.get();
    FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
    uint16_t freqColor = muted ? PALETTE_MUTED : (hasFreq ? s.colorFrequency : PALETTE_GRAY);
    drawSevenSegmentText(freqStr, x, y, scale, freqColor, PALETTE_BG);
}

// Modern frequency display - Antialiased with OpenFontRender
void V1Display::drawFrequencyModern(uint32_t freqMHz, bool isLaser, bool muted) {
    const V1Settings& s = settingsManager.get();
    
    // Fall back to Classic style if OFR not initialized or resting state (show dim 7-seg dashes)
    if (!ofrInitialized || (freqMHz == 0 && !isLaser)) {
        drawFrequencyClassic(freqMHz, isLaser, muted);
        return;
    }
    
    // OpenFontRender antialiased rendering
    const int fontSize = 66;  // Larger font size
    const int rightMargin = 120;  // Match Classic - leave room for arrow stack
    const int effectiveHeight = getEffectiveScreenHeight();
    const int freqY = effectiveHeight - 72;  // Position based on effective height
    
    ofr.setFontSize(fontSize);
    ofr.setBackgroundColor(0, 0, 0);  // Black background
    
    // Clear bottom area for frequency - minimal height to avoid covering band labels
    int maxWidth = SCREEN_WIDTH - rightMargin;
    FILL_RECT(0, effectiveHeight - 5, maxWidth, 5, PALETTE_BG);
    
    if (isLaser) {
        uint16_t color = muted ? PALETTE_MUTED : s.colorBandL;
        ofr.setFontColor((color >> 11) << 3, ((color >> 5) & 0x3F) << 2, (color & 0x1F) << 3);
        
        // Get text width for centering
        FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, "LASER");
        int textW = bbox.xMax - bbox.xMin;
        int x = (maxWidth - textW) / 2;  // Center in left portion like Classic
        
        ofr.setCursor(x, freqY);
        ofr.printf("LASER");
        return;
    }
    
    char freqStr[16];
    if (freqMHz > 0) {
        snprintf(freqStr, sizeof(freqStr), "%.3f", freqMHz / 1000.0f);
    } else {
        snprintf(freqStr, sizeof(freqStr), "--.---");
    }
    
    uint16_t freqColor = muted ? PALETTE_MUTED : (freqMHz > 0 ? s.colorFrequency : PALETTE_GRAY);
    ofr.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);
    
    // Get text width for centering
    FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, freqStr);
    int textW = bbox.xMax - bbox.xMin;
    int x = (maxWidth - textW) / 2;  // Center in left portion like Classic
    
    ofr.setCursor(x, freqY);
    ofr.printf("%s", freqStr);
}

// Router: calls appropriate frequency draw method based on display style setting
void V1Display::drawFrequency(uint32_t freqMHz, bool isLaser, bool muted) {
    const V1Settings& s = settingsManager.get();
    if (s.displayStyle == DISPLAY_STYLE_MODERN) {
        drawFrequencyModern(freqMHz, isLaser, muted);
    } else {
        drawFrequencyClassic(freqMHz, isLaser, muted);
    }
}


// Draw large direction arrow (t4s3 style)
void V1Display::drawDirectionArrow(Direction dir, bool muted) {
    // Stylized stacked arrows sized/positioned to match the real V1 display
    int cx = SCREEN_WIDTH - 70;           // right anchor
    int cy = SCREEN_HEIGHT / 2;           // vertically centered

#if defined(DISPLAY_WAVESHARE_349)
    // Position arrows to fit ABOVE frequency display at bottom
    // With multi-alert always enabled, use raised layout as default
    if (g_multiAlertMode) {
        cy = 85;  // Raised but allow full-size arrows
        cx -= 6;
    } else {
        cy = 95;
        cx -= 6;
    }
#endif
    
    // Use full-size arrows in both layouts
    float scale = 1.0f;
    
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
    uint16_t frontCol = muted ? PALETTE_MUTED : s.colorArrowFront;
    uint16_t sideCol = muted ? PALETTE_MUTED : s.colorArrowSide;
    uint16_t rearCol = muted ? PALETTE_MUTED : s.colorArrowRear;
    uint16_t offCol = 0x1082;  // Very dark grey for inactive arrows (matches PALETTE_GRAY)

    // Clear the entire arrow region using the max dimensions
    const int maxW = (topW > bottomW) ? topW : bottomW;
    const int maxH = (topH > bottomH) ? topH : bottomH;
    int clearTop = topArrowCenterY - topH/2 - 15;
    int clearBottom = bottomArrowCenterY + bottomH/2 + 15;
    FILL_RECT(cx - maxW/2 - 10, clearTop, maxW + 24, clearBottom - clearTop, PALETTE_BG);

    auto drawTriangleArrow = [&](int centerY, bool down, bool active, int triW, int triH, int notchW, int notchH, uint16_t activeCol) {
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

    auto drawSideArrow = [&](bool active) {
        uint16_t fillCol = active ? sideCol : offCol;
        uint16_t outlineCol = TFT_BLACK;  // Black outline like V1
        const int barW = (int)(66 * scale);   // Center bar width
        const int barH = sideBarH;
        const int headW = (int)(28 * scale);  // Arrow head width
        const int headH = (int)(22 * scale);  // Arrow head height
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

    // Up, side, down arrows - using calculated center positions with individual colors
    drawTriangleArrow(topArrowCenterY, false, dir & DIR_FRONT, topW, topH, topNotchW, topNotchH, frontCol);
    drawSideArrow(dir & DIR_SIDE);
    drawTriangleArrow(bottomArrowCenterY, true, dir & DIR_REAR, bottomW, bottomH, bottomNotchW, bottomNotchH, rearCol);
}

// Draw vertical signal bars on right side (t4s3 style)
void V1Display::drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band, bool muted) {
    const int barCount = 6;

    // Use the stronger side so rear-only alerts still light bars
    uint8_t strength = std::max(frontStrength, rearStrength);

    // Clamp strength to valid range (already mapped from 0-8 to 0-6)
    if (strength > 6) strength = 6;

    bool hasSignal = (strength > 0);
    
    // Get signal bar colors from settings (one per bar level)
    const V1Settings& s = settingsManager.get();
    uint16_t barColors[6] = {
        s.colorBar1, s.colorBar2, s.colorBar3,
        s.colorBar4, s.colorBar5, s.colorBar6
    };

#if defined(DISPLAY_WAVESHARE_349)
    // Scale from Lilygo 320x170 to Waveshare 640x172
    // Width is 2x, height is similar - make bars wider but keep height similar
    const int barWidth = 56;   // 26 * 2 scaled for wider screen
    const int barHeight = 14;  // Similar to original
    const int barSpacing = 12; // More spacing to fill vertical space
#else
    const int barWidth = 26;
    const int barHeight = 10;
    const int barSpacing = 6;
#endif
    const int totalH = barCount * (barHeight + barSpacing) - barSpacing;

    // Place bars to the right of the band stack and vertically centered
#if defined(DISPLAY_WAVESHARE_349)
    int startX = SCREEN_WIDTH - 228;  // Further from arrows on wider screen
#else
    int startX = SCREEN_WIDTH - 90;   // Relative position for narrower screen
#endif
    // Keep bars vertically centered; prefer the classic center, but clamp above the secondary row
    int availableH = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT; // avoid drawing over secondary row
    int desiredStartY = (SCREEN_HEIGHT - totalH) / 2;      // original center alignment
    int startY = desiredStartY;
    if (startY + totalH > availableH) {
        startY = availableH - totalH; // push up only if we would overlap the secondary row
    }
    if (startY < 8) startY = 8; // keep some padding from top icons

    // Clear area once
    int clearH = totalH + 4;
    if (startY - 2 + clearH > availableH) {
        clearH = availableH - (startY - 2); // clamp clear to stay above secondary row
    }
    FILL_RECT(startX - 2, startY - 2, barWidth + 4, clearH, PALETTE_BG);

    for (int i = 0; i < barCount; i++) {
        // Draw from bottom to top
        int visualIndex = barCount - 1 - i;
        int y = startY + visualIndex * (barHeight + barSpacing);
        
        bool lit = hasSignal && (i < strength);
        
        uint16_t fillColor;
        if (!lit) {
            fillColor = 0x1082; // very dark grey (resting/off)
        } else if (muted) {
            fillColor = PALETTE_MUTED; // muted grey for lit bars
        } else {
            fillColor = barColors[i];  // Use individual bar color
        }
        
        // Draw filled bar
        FILL_ROUND_RECT(startX, y, barWidth, barHeight, 2, fillColor);
    }
}

const char* V1Display::bandToString(Band band) {
    switch (band) {
        case BAND_LASER: return "Laser";
        case BAND_KA: return "Ka";
        case BAND_K: return "K";
        case BAND_X: return "X";
        default: return "None";
    }
}

uint16_t V1Display::getBandColor(Band band) {
    const V1Settings& s = settingsManager.get();
    switch (band) {
        case BAND_LASER: return s.colorBandL;
        case BAND_KA: return s.colorBandKa;
        case BAND_K: return s.colorBandK;
        case BAND_X: return s.colorBandX;
        default: return PALETTE_TEXT;
    }
}

uint16_t V1Display::getArrowColor(Direction dir) {
    const V1Settings& s = settingsManager.get();
    switch (dir) {
        case DIR_FRONT: return s.colorArrowFront;
        case DIR_SIDE: return s.colorArrowSide;
        case DIR_REAR: return s.colorArrowRear;
        default: return TFT_DARKGREY;
    }
}
void V1Display::updateColorTheme() {
    // Load the current theme from settings and update palette
    ColorTheme theme = settingsManager.get().colorTheme;
    currentPalette = ColorThemes::getPalette(theme);
    Serial.printf("Color theme updated: %s\n", ColorThemes::getThemeName(theme));
}
