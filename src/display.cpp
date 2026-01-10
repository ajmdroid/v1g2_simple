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

// Force card redraw flag - set by update() when full screen is cleared
static bool forceCardRedraw = false;

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
#define PALETTE_GRAY getColorPalette().colorGray
#define PALETTE_MUTED settingsManager.get().colorMuted  // User-configurable muted color
#define PALETTE_PERSISTED settingsManager.get().colorPersisted  // User-configurable persisted alert color
// Helper macro: returns PALETTE_PERSISTED when in persisted mode, else PALETTE_MUTED
#define PALETTE_MUTED_OR_PERSISTED (g_displayInstance && g_displayInstance->isPersistedMode() ? PALETTE_PERSISTED : PALETTE_MUTED)

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
        color = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBogey;
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
        
        uint16_t color = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBogey;
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
    uint16_t color = isDigit ? s.colorBogey : (muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBogey);
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
    setBLEProxyStatus(bleProxyEnabled, bleProxyClientConnected);
    
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
    setBLEProxyStatus(bleProxyEnabled, bleProxyClientConnected);
#endif
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

void V1Display::flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Constrain region to framebuffer bounds
    if (!tft || !gfxPanel) return;
    int16_t maxW = tft->width();
    int16_t maxH = tft->height();
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return;
    if (x >= maxW || y >= maxH) return;
    if (x + w > maxW) w = maxW - x;
    if (y + h > maxH) h = maxH - y;

    uint16_t* fb = tft->getFramebuffer();
    if (!fb) {
        tft->flush();
        return;
    }

    int16_t stride = tft->width();
    for (int16_t row = 0; row < h; ++row) {
        uint16_t* rowPtr = fb + (y + row) * stride + x;
        gfxPanel->draw16bitRGBBitmap(x, y + row, rowPtr, w, 1);
    }
#else
    (void)x; (void)y; (void)w; (void)h;
#endif
}

void V1Display::showDisconnected() {
    drawBaseFrame();
    drawStatusText("Disconnected", 0xF800);  // Red
    drawWiFiIndicator();
    drawBatteryIndicator();
}

void V1Display::showResting() {
    // Always use multi-alert layout positioning
    g_multiAlertMode = true;
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
        drawFrequency(0, BAND_NONE);
        
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
        // Push only the regions touched by profile/WiFi/BLE/battery indicators
        const int profileFlushY = 8;
        const int profileFlushH = 36;
        flushRegion(100, profileFlushY, SCREEN_WIDTH - 160, profileFlushH);

        const int leftColWidth = 64;
        const int leftColHeight = 96;
        flushRegion(0, SCREEN_HEIGHT - leftColHeight, leftColWidth, leftColHeight);
    #else
        flush();
#endif
    }

    // Reset lastState so next update() detects changes from this "resting" state
    lastState = DisplayState();  // All defaults: bands=0, arrows=0, bars=0, hasMode=false, modeChar=0
}

void V1Display::showScanning() {
    // Always use multi-alert layout positioning
    g_multiAlertMode = true;
    
    // Get settings for display style
    const V1Settings& s = settingsManager.get();

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

// Static flag to signal display change tracking reset on next update
static bool s_resetChangeTrackingFlag = false;

void V1Display::resetChangeTracking() {
    s_resetChangeTrackingFlag = true;
}

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
    const V1Settings& s = settingsManager.get();
    TFT_CALL(setTextColor)(muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandKa, PALETTE_BG);  // Use Ka color for band label
    GFX_drawString(tft, label, 10, SCREEN_HEIGHT / 2 - 26);
}

void V1Display::update(const DisplayState& state) {
    // Track if we're transitioning FROM persisted mode (need full redraw)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode
    
    // Track screen mode - this is "resting with bands" (between alerts)
    // Set to Resting so that entering Live mode triggers full redraw
    currentScreen = ScreenMode::Resting;
    
    static bool firstUpdate = true;
    static bool wasInFlashPeriod = false;
    
    // Always use multi-alert layout positioning
    g_multiAlertMode = true;
    multiAlertMode = false;  // No cards to draw in resting state
    
    // Check if profile flash period just expired (needs redraw to clear)
    bool inFlashPeriod = (millis() - profileChangedTime) < HIDE_TIMEOUT_MS;
    bool flashJustExpired = wasInFlashPeriod && !inFlashPeriod;
    wasInFlashPeriod = inFlashPeriod;

    // Band debouncing: keep bands visible for a short grace period to prevent flicker
    static unsigned long restingBandLastSeen[4] = {0, 0, 0, 0};  // L, Ka, K, X
    static uint8_t restingDebouncedBands = 0;
    const unsigned long BAND_GRACE_MS = 100;  // Reduced from 200ms for snappier response
    unsigned long now = millis();
    
    if (state.activeBands & BAND_LASER) restingBandLastSeen[0] = now;
    if (state.activeBands & BAND_KA)    restingBandLastSeen[1] = now;
    if (state.activeBands & BAND_K)     restingBandLastSeen[2] = now;
    if (state.activeBands & BAND_X)     restingBandLastSeen[3] = now;
    
    restingDebouncedBands = state.activeBands;
    if ((now - restingBandLastSeen[0]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_LASER;
    if ((now - restingBandLastSeen[1]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_KA;
    if ((now - restingBandLastSeen[2]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_K;
    if ((now - restingBandLastSeen[3]) < BAND_GRACE_MS) restingDebouncedBands |= BAND_X;

    // Override mute when laser active or strong signal present
    bool effectiveMuted = state.muted;
    bool laserActive = (restingDebouncedBands & BAND_LASER) != 0;
    if (laserActive) {
        effectiveMuted = false;
    } else if (effectiveMuted && state.signalBars >= STRONG_SIGNAL_UNMUTE_THRESHOLD) {
        effectiveMuted = false;
    }

    // Track last debounced bands for change detection
    static uint8_t lastRestingDebouncedBands = 0;
    static uint8_t lastRestingSignalBars = 0;
    static uint8_t lastRestingArrows = 0;
    
    // Separate full redraw triggers from incremental updates
    bool needsFullRedraw =
        firstUpdate ||
        flashJustExpired ||
        wasPersistedMode ||  // Force full redraw when leaving persisted mode
        restingDebouncedBands != lastRestingDebouncedBands ||
        effectiveMuted != lastState.muted ||
        state.modeChar != lastState.modeChar ||
        state.hasMode != lastState.hasMode;
    
    bool arrowsChanged = (state.arrows != lastRestingArrows);
    bool signalBarsChanged = (state.signalBars != lastRestingSignalBars);
    
    if (!needsFullRedraw && !arrowsChanged && !signalBarsChanged) {
        return;  // Nothing changed
    }
    
    if (!needsFullRedraw && (arrowsChanged || signalBarsChanged)) {
        // Incremental update - only redraw what changed
        if (arrowsChanged) {
            lastRestingArrows = state.arrows;
            drawDirectionArrow(state.arrows, effectiveMuted, state.flashBits);
        }
        if (signalBarsChanged) {
            lastRestingSignalBars = state.signalBars;
            Band primaryBand = BAND_KA;
            if (restingDebouncedBands & BAND_LASER) primaryBand = BAND_LASER;
            else if (restingDebouncedBands & BAND_KA) primaryBand = BAND_KA;
            else if (restingDebouncedBands & BAND_K) primaryBand = BAND_K;
            else if (restingDebouncedBands & BAND_X) primaryBand = BAND_X;
            drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
        }
#if defined(DISPLAY_WAVESHARE_349)
        tft->flush();
#endif
        lastState = state;
        return;
    }

    // Full redraw needed
    firstUpdate = false;
    lastRestingDebouncedBands = restingDebouncedBands;
    lastRestingArrows = state.arrows;
    lastRestingSignalBars = state.signalBars;
    
    drawBaseFrame();
    char topChar = state.hasMode ? state.modeChar : '0';
    drawTopCounter(topChar, effectiveMuted, true);  // Always show dot
    drawBandIndicators(restingDebouncedBands, effectiveMuted);
    // BLE proxy status indicator
    
    // Determine primary band for frequency and signal bar coloring
    Band primaryBand = BAND_NONE;
    if (restingDebouncedBands & BAND_LASER) primaryBand = BAND_LASER;
    else if (restingDebouncedBands & BAND_KA) primaryBand = BAND_KA;
    else if (restingDebouncedBands & BAND_K) primaryBand = BAND_K;
    else if (restingDebouncedBands & BAND_X) primaryBand = BAND_X;
    
    drawFrequency(0, primaryBand, effectiveMuted);
    
    drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
    drawDirectionArrow(state.arrows, effectiveMuted, state.flashBits);
    drawMuteIcon(effectiveMuted);
    drawProfileIndicator(currentProfileSlot);
    
    // Clear any persisted card slots when entering resting state
    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();  // Push canvas to display
#endif

    lastState = state;
}

// Persisted alert display - shows last alert in dark grey after V1 clears it
// Only draws frequency, band, and arrows - no signal bars, no mute badge
// Bogey counter shows V1 mode (from state), not "1"
void V1Display::updatePersisted(const AlertData& alert, const DisplayState& state) {
    if (!alert.isValid) {
        persistedMode = false;
        update(state);  // Fall back to normal resting display
        return;
    }
    
    // Enable persisted mode so draw functions use PALETTE_PERSISTED instead of PALETTE_MUTED
    persistedMode = true;
    
    // Track screen mode - persisted is NOT Live, so transition to Live will trigger full redraw
    currentScreen = ScreenMode::Resting;
    
    // Always use multi-alert layout positioning
    g_multiAlertMode = true;
    multiAlertMode = false;  // No cards to draw
    wasInMultiAlertMode = false;
    
    drawBaseFrame();
    
    // Bogey counter shows V1 mode (truth from V1) - NOT greyed, always visible
    char topChar = state.hasMode ? state.modeChar : '0';
    drawTopCounter(topChar, false, true);  // muted=false to keep it visible
    
    // Band indicator in persisted color
    uint8_t bandMask = alert.band;
    drawBandIndicators(bandMask, true);  // muted=true triggers PALETTE_MUTED_OR_PERSISTED
    
    // Frequency in persisted color (pass muted=true)
    drawFrequency(alert.frequency, alert.band, true);
    
    // No signal bars - just draw empty
    drawVerticalSignalBars(0, 0, alert.band, true);
    
    // Arrows in dark grey
    drawDirectionArrow(alert.direction, true);  // muted=true for grey
    
    // No mute badge
    // drawMuteIcon intentionally skipped
    
    // Profile indicator still shown
    drawProfileIndicator(currentProfileSlot);
    
    // Clear card area AND expire all tracked card slots (no cards during persisted state)
    // This prevents stale cards from reappearing when returning to live alerts
    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, true);

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();
#endif
}

// Multi-alert update: draws priority alert with secondary alert cards below
void V1Display::update(const AlertData& priority, const AlertData* allAlerts, int alertCount, const DisplayState& state) {
    // Check if we're transitioning FROM persisted mode (need full redraw to restore colors)
    bool wasPersistedMode = persistedMode;
    persistedMode = false;  // Not in persisted mode
    
    // Track screen mode transitions - force redraw when entering live mode from resting/scanning
    bool enteringLiveMode = (currentScreen != ScreenMode::Live);
    currentScreen = ScreenMode::Live;
    
    // Always use multi-alert mode (raised layout for cards)
    g_multiAlertMode = true;
    multiAlertMode = true;
    
    // Get settings reference for priorityArrowOnly
    const V1Settings& s = settingsManager.get();
    
    // If no valid priority alert, return (caller should use updatePersisted or update(state) instead)
    if (!priority.isValid || priority.band == BAND_NONE) {
        return;
    }

    // Band debouncing: keep bands visible for a short grace period to prevent flicker
    // when signal fluctuates on the edge of detection
    // when signal fluctuates on the edge of detection
    static unsigned long bandLastSeen[4] = {0, 0, 0, 0};  // L, Ka, K, X
    static uint8_t debouncedBandMask = 0;
    const unsigned long BAND_GRACE_MS = 100;  // Reduced from 200ms for snappier response
    unsigned long now = millis();
    
    // Update last-seen times for currently active bands
    if (state.activeBands & BAND_LASER) bandLastSeen[0] = now;
    if (state.activeBands & BAND_KA)    bandLastSeen[1] = now;
    if (state.activeBands & BAND_K)     bandLastSeen[2] = now;
    if (state.activeBands & BAND_X)     bandLastSeen[3] = now;
    
    // Build debounced mask: include bands that are active OR were recently active
    debouncedBandMask = state.activeBands;
    if ((now - bandLastSeen[0]) < BAND_GRACE_MS) debouncedBandMask |= BAND_LASER;
    if ((now - bandLastSeen[1]) < BAND_GRACE_MS) debouncedBandMask |= BAND_KA;
    if ((now - bandLastSeen[2]) < BAND_GRACE_MS) debouncedBandMask |= BAND_K;
    if ((now - bandLastSeen[3]) < BAND_GRACE_MS) debouncedBandMask |= BAND_X;

    // Change detection: check if we need to redraw
    static AlertData lastPriority;
    static int lastAlertCount = 0;
    static DisplayState lastMultiState;
    static bool firstRun = true;
    static AlertData lastSecondary[4];
    static uint8_t lastArrows = 0;
    static uint8_t lastSignalBars = 0;
    static uint8_t lastDebouncedBands = 0;
    
    // Check if reset was requested (e.g., on V1 disconnect)
    if (s_resetChangeTrackingFlag) {
        lastPriority = AlertData();
        lastAlertCount = 0;
        lastMultiState = DisplayState();
        firstRun = true;
        for (int i = 0; i < 4; i++) lastSecondary[i] = AlertData();
        lastArrows = 0;
        lastSignalBars = 0;
        lastDebouncedBands = 0;
        // Reset band debounce timestamps
        for (int i = 0; i < 4; i++) bandLastSeen[i] = 0;
        s_resetChangeTrackingFlag = false;
    }
    
    bool needsRedraw = false;
    
    // Always redraw on first run, entering live mode, or when transitioning from persisted mode
    if (firstRun) { needsRedraw = true; firstRun = false; }
    else if (enteringLiveMode) { needsRedraw = true; }
    else if (wasPersistedMode) { needsRedraw = true; }
    else if (priority.frequency != lastPriority.frequency) { needsRedraw = true; }
    else if (priority.band != lastPriority.band) { needsRedraw = true; }
    else if (alertCount != lastAlertCount) { needsRedraw = true; }
    // Use debounced band mask for change detection to prevent flicker
    else if (debouncedBandMask != lastDebouncedBands) { needsRedraw = true; }
    else if (state.muted != lastMultiState.muted) { needsRedraw = true; }
    
    // Also check if any secondary alert changed (but not signal strength or direction - those fluctuate)
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
    // Arrow display depends on per-profile priorityArrowOnly setting
    Direction arrowsToShow = settingsManager.getSlotPriorityArrowOnly(s.activeSlot) ? state.priorityArrow : state.arrows;
    bool arrowsChanged = (arrowsToShow != lastArrows);
    bool signalBarsChanged = (state.signalBars != lastSignalBars);
    
    // If any flash bits set, arrows need continuous redraws for animation
    bool arrowsFlashing = (state.flashBits & 0xE0) != 0;
    
    if (!needsRedraw && !arrowsChanged && !signalBarsChanged && !arrowsFlashing) {
        // Nothing changed on main display, but still process cards for expiration
        drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
#if defined(DISPLAY_WAVESHARE_349)
        tft->flush();
#endif
        return;
    }
    
    if (!needsRedraw && (arrowsChanged || signalBarsChanged || arrowsFlashing)) {
        // Only arrows and/or signal bars changed - do incremental update without full redraw
        if (arrowsChanged || arrowsFlashing) {
            lastArrows = arrowsToShow;
            drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
        }
        if (signalBarsChanged) {
            lastSignalBars = state.signalBars;
            drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
        }
        // Still process cards so they can expire and be cleared
        drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
#if defined(DISPLAY_WAVESHARE_349)
        tft->flush();
#endif
        return;
    }
    
    // Full redraw needed - store current state for next comparison
    lastPriority = priority;
    lastAlertCount = alertCount;
    lastMultiState = state;
    lastArrows = settingsManager.getSlotPriorityArrowOnly(s.activeSlot) ? state.priorityArrow : state.arrows;
    lastSignalBars = state.signalBars;
    lastDebouncedBands = debouncedBandMask;
    for (int i = 0; i < alertCount && i < 4; i++) {
        lastSecondary[i] = allAlerts[i];
    }
    
    drawBaseFrame();

    // Use debounced band mask to prevent flicker from signal fluctuation
    uint8_t bandMask = debouncedBandMask;
    
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
    drawFrequency(priority.frequency, priority.band, state.muted);
    drawBandIndicators(bandMask, state.muted);
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    
    // Arrow display: use priority arrow only if setting enabled, otherwise all V1 arrows
    // (arrowsToShow already computed above for change detection)
    drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
    drawMuteIcon(state.muted);
    drawProfileIndicator(currentProfileSlot);
    
    // Force card redraw since drawBaseFrame cleared the screen
    forceCardRedraw = true;
    
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
// With persistence: cards stay visible (greyed) for a grace period after alert disappears
void V1Display::drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const int cardH = SECONDARY_ROW_HEIGHT;  // 30px
    const int cardY = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Y=142
    const int cardW = 145;     // Card width (wider to fit freq + band)
    const int cardSpacing = 6;
    const int startX = 120;    // Start after band indicators (L/Ka/K/X ends around X=115)
    
    // Get persistence time from profile settings (same as main alert persistence)
    const V1Settings& settings = settingsManager.get();
    uint8_t persistSec = settingsManager.getSlotAlertPersistSec(settings.activeSlot);
    unsigned long gracePeriodMs = persistSec * 1000UL;
    
    // If persistence is disabled (0), cards disappear immediately
    if (gracePeriodMs == 0) {
        gracePeriodMs = 1;  // Minimum 1ms so expiration logic works
    }
    
    unsigned long now = millis();
    
    // Static card slots for persistence tracking
    static struct {
        AlertData alert{};
        unsigned long lastSeen = 0;  // 0 = empty slot
    } cards[2];
    
    // Track previous priority to add as persisted card when it disappears
    static AlertData lastPriorityForCards;
    
    // Track what was drawn last frame to avoid unnecessary redraws (declared early for reset)
    static struct {
        Band band = BAND_NONE;
        uint32_t frequency = 0;
        bool isGraced = false;
        bool wasMuted = false;
    } lastDrawnCards[2];
    static int lastDrawnCount = 0;
    
    // Track profile changes - clear cards when profile rotates
    static int lastCardProfileSlot = -1;
    if (settings.activeSlot != lastCardProfileSlot) {
        lastCardProfileSlot = settings.activeSlot;
        // Clear all card state on profile change
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
            lastDrawnCards[c].band = BAND_NONE;
            lastDrawnCards[c].frequency = 0;
        }
        lastDrawnCount = 0;
        lastPriorityForCards = AlertData();
    }
    
    // If called with nullptr alerts and count 0, force-expire all cards immediately
    // (used when transitioning to non-alert screens to clear stale card state)
    if (alerts == nullptr && alertCount == 0) {
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
        }
        lastPriorityForCards = AlertData();
        // Clear the card area
        const int signalBarsX = SCREEN_WIDTH - 200 - 2;
        const int clearWidth = signalBarsX - startX;
        if (clearWidth > 0) {
            FILL_RECT(startX, cardY, clearWidth, cardH, PALETTE_BG);
        }
        return;
    }
    
    // Helper: check if two alerts match (same band + exact frequency)
    auto alertsMatch = [](const AlertData& a, const AlertData& b) -> bool {
        if (a.band != b.band) return false;
        if (a.band == BAND_LASER) return true;
        return (a.frequency == b.frequency);
    };
    
    // Helper: check if alert matches priority (returns false if priority is invalid)
    auto isSameAsPriority = [&priority, &alertsMatch](const AlertData& a) -> bool {
        if (!priority.isValid || priority.band == BAND_NONE) return false;
        return alertsMatch(a, priority);
    };
    
    // Step 0: Check if priority changed - add old priority as persisted card
    // This handles the case where laser takes priority, then stops - laser should persist as card
    if (lastPriorityForCards.isValid && lastPriorityForCards.band != BAND_NONE) {
        bool priorityChanged = !alertsMatch(lastPriorityForCards, priority);
        bool oldPriorityGone = true;
        
        // Check if old priority is still in current alerts
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(lastPriorityForCards, alerts[i])) {
                    oldPriorityGone = false;
                    break;
                }
            }
        }
        
        // If old priority is gone (not just demoted), add it as persisted card
        if (priorityChanged && oldPriorityGone) {
            // Check if already tracked
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (cards[c].lastSeen > 0 && alertsMatch(cards[c].alert, lastPriorityForCards)) {
                    found = true;
                    break;
                }
            }
            
            // Add to empty slot if not already tracked
            if (!found) {
                for (int c = 0; c < 2; c++) {
                    if (cards[c].lastSeen == 0) {
                        cards[c].alert = lastPriorityForCards;
                        cards[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }
    
    // Update last priority tracking
    lastPriorityForCards = priority;
    
    // Step 1: Update existing slots - refresh timestamp if alert still exists
    for (int c = 0; c < 2; c++) {
        if (cards[c].lastSeen == 0) continue;
        
        bool stillExists = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(cards[c].alert, alerts[i])) {
                    stillExists = true;
                    cards[c].alert = alerts[i];  // Update with latest data
                    cards[c].lastSeen = now;
                    break;
                }
            }
        }
        
        // Expire if past grace period
        if (!stillExists) {
            unsigned long age = now - cards[c].lastSeen;
            if (age > gracePeriodMs) {
                cards[c].alert = AlertData();
                cards[c].lastSeen = 0;
            }
        }
    }
    
    // Step 2: Add new alerts to empty slots (including priority - it may become secondary later)
    if (alerts != nullptr) {
        for (int i = 0; i < alertCount; i++) {
            if (!alerts[i].isValid || alerts[i].band == BAND_NONE) continue;
            
            // Check if already tracked
            bool found = false;
            for (int c = 0; c < 2; c++) {
                if (cards[c].lastSeen > 0 && alertsMatch(cards[c].alert, alerts[i])) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                // Find empty slot
                for (int c = 0; c < 2; c++) {
                    if (cards[c].lastSeen == 0) {
                        cards[c].alert = alerts[i];
                        cards[c].lastSeen = now;
                        break;
                    }
                }
            }
        }
    }
    
    // For debug logging if needed
    bool doDebug = false;
    
    // Build list of cards to draw this frame
    struct CardToDraw {
        int slot;
        bool isGraced;
    } cardsToDraw[2];
    int cardsToDrawCount = 0;
    
    for (int c = 0; c < 2 && cardsToDrawCount < 2; c++) {
        if (cards[c].lastSeen == 0) continue;
        if (isSameAsPriority(cards[c].alert)) continue;
        cardsToDraw[cardsToDrawCount].slot = c;
        // Check if live or graced
        bool isLive = false;
        if (alerts != nullptr) {
            for (int i = 0; i < alertCount; i++) {
                if (alertsMatch(cards[c].alert, alerts[i])) {
                    isLive = true;
                    break;
                }
            }
        }
        cardsToDraw[cardsToDrawCount].isGraced = !isLive;
        cardsToDrawCount++;
    }
    
    // Check if cards changed from last frame
    bool cardsChanged = (cardsToDrawCount != lastDrawnCount);
    if (!cardsChanged) {
        for (int i = 0; i < cardsToDrawCount; i++) {
            int slot = cardsToDraw[i].slot;
            if (cards[slot].alert.band != lastDrawnCards[i].band ||
                cards[slot].alert.frequency != lastDrawnCards[i].frequency ||
                cardsToDraw[i].isGraced != lastDrawnCards[i].isGraced ||
                muted != lastDrawnCards[i].wasMuted) {
                cardsChanged = true;
                break;
            }
        }
    }
    
    // Only clear and redraw if cards changed (or forced after full screen redraw)
    if (!cardsChanged && !forceCardRedraw) {
        return;  // No card changes - skip redraw
    }
    forceCardRedraw = false;  // Reset the force flag
    
    // Clear card area only when needed
    const int signalBarsX = SCREEN_WIDTH - 200 - 2;
    const int clearWidth = signalBarsX - startX;
    if (clearWidth > 0) {
        FILL_RECT(startX, cardY, clearWidth, SECONDARY_ROW_HEIGHT, PALETTE_BG);
    }
    
    // Update tracking for next frame
    lastDrawnCount = cardsToDrawCount;
    for (int i = 0; i < 2; i++) {
        if (i < cardsToDrawCount) {
            int slot = cardsToDraw[i].slot;
            lastDrawnCards[i].band = cards[slot].alert.band;
            lastDrawnCards[i].frequency = cards[slot].alert.frequency;
            lastDrawnCards[i].isGraced = cardsToDraw[i].isGraced;
            lastDrawnCards[i].wasMuted = muted;
        } else {
            lastDrawnCards[i].band = BAND_NONE;
            lastDrawnCards[i].frequency = 0;
            lastDrawnCards[i].isGraced = false;
            lastDrawnCards[i].wasMuted = false;
        }
    }
    
    // Step 3: Draw the cards we identified
    for (int i = 0; i < cardsToDrawCount; i++) {
        int c = cardsToDraw[i].slot;
        const AlertData& alert = cards[c].alert;
        bool isGraced = cardsToDraw[i].isGraced;
        bool drawMuted = muted || isGraced;
        
        int cardX = startX + i * (cardW + cardSpacing);
        
        if (doDebug) {
            Serial.printf("[CARDS] DRAW slot%d b%d f%d graced=%d X=%d\n", 
                          c, alert.band, alert.frequency, isGraced, cardX);
        }
        
        // Card background and border colors
        uint16_t bandCol = getBandColor(alert.band);
        uint16_t bgCol, borderCol;
        
        if (isGraced) {
            // Graced: use PALETTE_MUTED (grey) with slightly visible background
            bgCol = 0x2104;  // Dark grey background
            borderCol = PALETTE_MUTED;
        } else if (drawMuted) {
            // Muted but not graced
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else {
            // Active card - darker version of band color
            uint8_t r = ((bandCol >> 11) & 0x1F) * 3 / 10;
            uint8_t g = ((bandCol >> 5) & 0x3F) * 3 / 10;
            uint8_t b = (bandCol & 0x1F) * 3 / 10;
            bgCol = (r << 11) | (g << 5) | b;
            borderCol = bandCol;
        }
        
        FILL_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bgCol);
        DRAW_ROUND_RECT(cardX, cardY, cardW, cardH, 5, borderCol);
        
        // Colors for content - graced cards use grey
        uint16_t contentCol = (isGraced || drawMuted) ? PALETTE_MUTED : TFT_WHITE;
        uint16_t bandLabelCol = (isGraced || drawMuted) ? PALETTE_MUTED : bandCol;
        
        // Direction arrow on left side of card
        int arrowX = cardX + 18;
        int arrowCY = cardY + cardH / 2;
        
        if (alert.direction & DIR_FRONT) {
            tft->fillTriangle(arrowX, arrowCY - 8, arrowX - 7, arrowCY + 8, arrowX + 7, arrowCY + 8, contentCol);
        } else if (alert.direction & DIR_REAR) {
            tft->fillTriangle(arrowX, arrowCY + 8, arrowX - 7, arrowCY - 8, arrowX + 7, arrowCY - 8, contentCol);
        } else if (alert.direction & DIR_SIDE) {
            FILL_RECT(arrowX - 8, arrowCY - 3, 16, 6, contentCol);
        }
        
        // Band indicator (colored dot or short label) on left after arrow
        int labelX = cardX + 36;
        tft->setTextColor(bandLabelCol);
        tft->setTextSize(2);
        if (alert.band == BAND_LASER) {
            tft->setCursor(labelX, cardY + (cardH - 16) / 2);
            tft->print("LASER");
        } else {
            // Band letter + frequency: "Ka 34.740" or "K 24.150"
            const char* bandStr = bandToString(alert.band);
            tft->setCursor(labelX, cardY + (cardH - 16) / 2);
            tft->print(bandStr);
            
            // Frequency after band
            tft->setTextColor(contentCol);
            int freqX = labelX + strlen(bandStr) * 12 + 4;
            tft->setCursor(freqX, cardY + (cardH - 16) / 2);
            if (alert.frequency > 0) {
                char freqStr[10];
                snprintf(freqStr, sizeof(freqStr), "%.3f", alert.frequency / 1000.0f);
                tft->print(freqStr);
            } else {
                tft->print("---");
            }
        }
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
        uint16_t col = active ? (muted ? PALETTE_MUTED_OR_PERSISTED : cells[i].color) : TFT_DARKGREY;
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
            FILL_RECT(x, y, barWidth, height, 0xF800);  // Red
        } else {
            // Draw empty bar outline
            DRAW_RECT(x, y, barWidth, height, TFT_DARKGREY);
            FILL_RECT(x + 1, y + 1, barWidth - 2, height - 2, PALETTE_BG);
        }
    }
}

// Classic 7-segment frequency display (original V1 style)
void V1Display::drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const float scale = 2.2f; // Larger for wider screen
#else
    const float scale = 1.7f; // ~15% smaller than the counter digits
#endif
    SegMetrics m = segMetrics(scale);
    
    // Position frequency at bottom with proper margin (shifted up in multi-alert mode)
    int y = getEffectiveScreenHeight() - m.digitH - 8;
    
    if (band == BAND_LASER) {
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
        draw14SegmentText(laserStr, x, y, scale, muted ? PALETTE_MUTED_OR_PERSISTED : set.colorBandL, PALETTE_BG);
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
    
    // Determine frequency color: muted -> grey, else band color (if enabled) or custom freq color
    uint16_t freqColor;
    if (muted) {
        freqColor = PALETTE_MUTED_OR_PERSISTED;
    } else if (!hasFreq) {
        freqColor = PALETTE_GRAY;
    } else if (s.freqUseBandColor && band != BAND_NONE) {
        freqColor = getBandColor(band);
    } else {
        freqColor = s.colorFrequency;
    }
    drawSevenSegmentText(freqStr, x, y, scale, freqColor, PALETTE_BG);
}

// Modern frequency display - Antialiased with OpenFontRender
void V1Display::drawFrequencyModern(uint32_t freqMHz, Band band, bool muted) {
    const V1Settings& s = settingsManager.get();
    
    // Fall back to Classic style if OFR not initialized or resting state (show dim 7-seg dashes)
    if (!ofrInitialized || (freqMHz == 0 && band != BAND_LASER)) {
        drawFrequencyClassic(freqMHz, band, muted);
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
    
    if (band == BAND_LASER) {
        uint16_t color = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
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
    
    // Determine frequency color: muted -> grey, else band color (if enabled) or custom freq color
    uint16_t freqColor;
    if (muted) {
        freqColor = PALETTE_MUTED_OR_PERSISTED;
    } else if (freqMHz == 0) {
        freqColor = PALETTE_GRAY;
    } else if (s.freqUseBandColor && band != BAND_NONE) {
        freqColor = getBandColor(band);
    } else {
        freqColor = s.colorFrequency;
    }
    ofr.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);
    
    // Get text width for centering
    FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, freqStr);
    int textW = bbox.xMax - bbox.xMin;
    int x = (maxWidth - textW) / 2;  // Center in left portion like Classic
    
    ofr.setCursor(x, freqY);
    ofr.printf("%s", freqStr);
}

// Router: calls appropriate frequency draw method based on display style setting
void V1Display::drawFrequency(uint32_t freqMHz, Band band, bool muted) {
    const V1Settings& s = settingsManager.get();
    if (s.displayStyle == DISPLAY_STYLE_MODERN) {
        drawFrequencyModern(freqMHz, band, muted);
    } else {
        drawFrequencyClassic(freqMHz, band, muted);
    }
}


// Draw large direction arrow (t4s3 style)
// flashBits: bit5=front, bit6=side, bit7=rear - set when arrow should flash
void V1Display::drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits) {
    // Blink timer for flashing arrows (~4Hz like real V1)
    static unsigned long lastBlinkMs = 0;
    static bool blinkOn = true;
    unsigned long now = millis();
    if (now - lastBlinkMs > 125) {  // ~4Hz blink rate (125ms on/off)
        blinkOn = !blinkOn;
        lastBlinkMs = now;
    }
    
    // Determine which arrows should flash (from V1 image1/image2 decode)
    bool frontFlash = (flashBits & 0x20) != 0;  // bit 5
    bool sideFlash = (flashBits & 0x40) != 0;   // bit 6
    bool rearFlash = (flashBits & 0x80) != 0;   // bit 7
    
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
    uint16_t frontCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowFront;
    uint16_t sideCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowSide;
    uint16_t rearCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowRear;
    uint16_t offCol = 0x1082;  // Very dark grey for inactive arrows (matches PALETTE_GRAY)

    // Clear the entire arrow region using the max dimensions
    // Stop above profile indicator area (profile at Y=152)
    const int maxW = (topW > bottomW) ? topW : bottomW;
    const int maxH = (topH > bottomH) ? topH : bottomH;
    int clearTop = topArrowCenterY - topH/2 - 15;
    int clearBottom = bottomArrowCenterY + bottomH/2 + 2;  // Reduced to not overlap profile area
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

    // Up, side, down arrows - apply per-arrow flash state
    // Arrow shows if: direction bit set AND (not flashing OR blink phase is on)
    bool frontActive = (dir & DIR_FRONT) && (!frontFlash || blinkOn);
    bool sideActive = (dir & DIR_SIDE) && (!sideFlash || blinkOn);
    bool rearActive = (dir & DIR_REAR) && (!rearFlash || blinkOn);
    
    drawTriangleArrow(topArrowCenterY, false, frontActive, topW, topH, topNotchW, topNotchH, frontCol);
    drawSideArrow(sideActive);
    drawTriangleArrow(bottomArrowCenterY, true, rearActive, bottomW, bottomH, bottomNotchW, bottomNotchH, rearCol);
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
    const int barWidth = 44;   // Narrower bars
    const int barHeight = 14;  // Similar to original
    const int barSpacing = 10; // Tighter spacing
#else
    const int barWidth = 26;
    const int barHeight = 10;
    const int barSpacing = 6;
#endif
    const int totalH = barCount * (barHeight + barSpacing) - barSpacing;

    // Place bars to the right of the band stack and vertically centered
#if defined(DISPLAY_WAVESHARE_349)
    int startX = SCREEN_WIDTH - 200;  // Moved closer to arrows (was 228)
#else
    int startX = SCREEN_WIDTH - 90;   // Relative position for narrower screen
#endif
    // Align signal bars so gap between bars 3 and 4 aligns with middle arrow center (cy=85)
    // With 8 bars: barHeight=14, barSpacing=10, gap center at startY + 3*(barHeight+barSpacing) - barSpacing/2
    // Want: startY + 3*24 - 5 = 85, so startY = 85 - 67 = 18
    int startY = 18;  // Fixed position to align with middle arrow
    if (startY < 8) startY = 8; // keep some padding from top icons

    // Clear area once
    int clearH = totalH + 4;
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
    // Always use standard palette - custom colors are per-element in settings
    currentPalette = ColorThemes::STANDARD();
}
