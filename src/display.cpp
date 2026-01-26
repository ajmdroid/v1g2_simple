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
#include "ble_client.h"
#include "obd_handler.h"
#include "gps_handler.h"
#include "storage_manager.h"
#include "audio_beep.h"
#include <esp_heap_caps.h>
#include <cstring>
#include "../include/FreeSansBold24pt7b.h"  // Custom font for band labels

// OpenFontRender for antialiased TrueType rendering
#include "OpenFontRender.h"
#include "../include/MontserratBold.h"       // Montserrat Bold TTF (subset: 0-9, -, ., LASER, SCAN)
#include "../include/HemiHead.h"             // Hemi Head TTF (subset: 0-9, -, ., LASER, SCAN)
#include "../include/Segment7Font.h"         // Segment7 TTF for Classic display (JBV1 style)
#include "../include/Serpentine.h"           // Serpentine TTF (JB's favorite)

// Global OpenFontRender instances
static OpenFontRender ofr;                   // For Modern style (Montserrat Bold)
static OpenFontRender ofrSegment7;           // For Classic style (Segment7 - JBV1)
static OpenFontRender ofrHemi;               // For Hemi style (Hemi Head - retro speedometer)
static OpenFontRender ofrSerpentine;         // For Serpentine style (JB's favorite)
static bool ofrInitialized = false;
static bool ofrSegment7Initialized = false;
static bool ofrHemiInitialized = false;
static bool ofrSerpentineInitialized = false;

// Multi-alert mode tracking (used for card display, no longer shifts main content)
static bool g_multiAlertMode = false;
static constexpr int PRIMARY_ZONE_HEIGHT = 95;  // Fixed height for primary display (with gap above cards)

// Force card redraw flag - set by update() when full screen is cleared
static bool forceCardRedraw = false;

// Force frequency cache invalidation - set when screen is cleared to ensure redraw
static bool s_forceFrequencyRedraw = false;

// Force battery percent cache invalidation - set when screen is cleared
static bool s_forceBatteryRedraw = false;

// Force band/signal bar/arrow cache invalidation - set when screen is cleared
static bool s_forceBandRedraw = false;
static bool s_forceSignalBarsRedraw = false;
static bool s_forceArrowRedraw = false;

// Force status bar/mute icon/top counter cache invalidation - set when screen is cleared
static bool s_forceStatusBarRedraw = false;
static bool s_forceMuteIconRedraw = false;
static bool s_forceTopCounterRedraw = false;

// Volume zero warning tracking (show for 10 seconds when no app connected, after 15 second delay)
static unsigned long volumeZeroDetectedMs = 0;       // When we first detected volume=0
static unsigned long volumeZeroWarningStartMs = 0;   // When warning display actually started
static bool volumeZeroWarningShown = false;
static bool volumeZeroWarningAcknowledged = false;
static constexpr unsigned long VOLUME_ZERO_DELAY_MS = 15000;          // Wait 15 seconds before showing warning
static constexpr unsigned long VOLUME_ZERO_WARNING_DURATION_MS = 10000;  // Show warning for 10 seconds

// External reference to BLE client for checking proxy connection
extern V1BLEClient bleClient;

// External reference to GPS handler for GPS indicator
extern GPSHandler gpsHandler;

// Helper to get effective screen height (fixed primary zone, cards below)
static inline int getEffectiveScreenHeight() {
    return PRIMARY_ZONE_HEIGHT;  // Always use fixed primary zone height
}

// Debug timing for display operations (set to true to profile display)
static constexpr bool DISPLAY_PERF_TIMING = false;  // Disable for production
static unsigned long _dispPerfStart = 0;
#define DISP_PERF_START() do { if (DISPLAY_PERF_TIMING) _dispPerfStart = micros(); } while(0)
#define DISP_PERF_LOG(label) do { if (DISPLAY_PERF_TIMING) { \
    unsigned long _dur = micros() - _dispPerfStart; \
    if (_dur > 5000) Serial.printf("[DISP] %s: %luus\n", label, _dur); \
    _dispPerfStart = micros(); \
} } while(0)

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
    
    // Initialize Segment7 font for Classic style (JBV1)
    Serial.printf("Loading Segment7 font (%d bytes)...\n", sizeof(Segment7Font));
    ofrSegment7.setSerial(Serial);
    ofrSegment7.setDrawer(*tft);
    FT_Error ftErr2 = ofrSegment7.loadFont(Segment7Font, sizeof(Segment7Font));
    if (ftErr2) {
        Serial.printf("ERROR: Failed to load Segment7 font! FT_Error: 0x%02X\n", ftErr2);
        ofrSegment7Initialized = false;
    } else {
        Serial.println("Segment7 font initialized (JBV1 Classic style)");
        ofrSegment7Initialized = true;
    }
    
    // Initialize Hemi Head font for Hemi style (retro speedometer)
    Serial.printf("Loading Hemi Head font (%d bytes)...\n", sizeof(HemiHead));
    ofrHemi.setSerial(Serial);
    ofrHemi.setDrawer(*tft);
    FT_Error ftErr3 = ofrHemi.loadFont(HemiHead, sizeof(HemiHead));
    if (ftErr3) {
        Serial.printf("ERROR: Failed to load Hemi Head font! FT_Error: 0x%02X\n", ftErr3);
        ofrHemiInitialized = false;
    } else {
        Serial.println("Hemi Head font initialized (retro speedometer style)");
        ofrHemiInitialized = true;
    }
    
    // Initialize Serpentine font (JB's favorite)
    Serial.printf("Loading Serpentine font (%d bytes)...\n", sizeof(Serpentine));
    ofrSerpentine.setSerial(Serial);
    ofrSerpentine.setDrawer(*tft);
    FT_Error ftErr4 = ofrSerpentine.loadFont(Serpentine, sizeof(Serpentine));
    if (ftErr4) {
        Serial.printf("ERROR: Failed to load Serpentine font! FT_Error: 0x%02X\n", ftErr4);
        ofrSerpentineInitialized = false;
    } else {
        Serial.println("Serpentine font initialized (JB's favorite)");
        ofrSerpentineInitialized = true;
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
    // Legacy single-slider mode - redirect to settings sliders with volume at 75 (default)
    showSettingsSliders(currentLevel, 75);
}

// Combined settings screen with brightness and voice volume sliders
void V1Display::showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel) {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Clear screen to dark background
    tft->fillScreen(0x0000);
    
    // Layout: 640x172 landscape - two horizontal sliders stacked
    const int sliderMargin = 40;
    const int sliderHeight = 10;
    const int sliderWidth = SCREEN_WIDTH - (sliderMargin * 2);  // 560 pixels
    const int sliderX = sliderMargin;
    
    // Brightness slider at top (y=45)
    const int brightnessY = 45;
    // Volume slider lower (y=115)
    const int volumeY = 115;
    
    // Title
    tft->setTextColor(0xFFFF);  // White
    tft->setTextSize(2);
    tft->setCursor((SCREEN_WIDTH - 120) / 2, 5);
    tft->print("SETTINGS");
    
    // === Brightness slider ===
    tft->setTextSize(1);
    tft->setTextColor(0xFFFF);
    tft->setCursor(sliderMargin, brightnessY - 16);
    tft->print("BRIGHTNESS");
    
    // Draw slider track
    tft->drawRect(sliderX - 2, brightnessY - 2, sliderWidth + 4, sliderHeight + 4, 0x4208);
    tft->fillRect(sliderX, brightnessY, sliderWidth, sliderHeight, 0x2104);
    
    // Fill based on brightness level (80-255 range)
    int brightnessFill = ((brightnessLevel - 80) * sliderWidth) / 175;
    tft->fillRect(sliderX, brightnessY, brightnessFill, sliderHeight, 0x07E0);  // Green
    
    // Thumb
    int brightThumbX = sliderX + brightnessFill - 4;
    if (brightThumbX < sliderX) brightThumbX = sliderX;
    if (brightThumbX > sliderX + sliderWidth - 8) brightThumbX = sliderX + sliderWidth - 8;
    tft->fillRect(brightThumbX, brightnessY - 4, 8, sliderHeight + 8, 0xFFFF);
    
    // Percentage text
    char brightStr[8];
    int brightPercent = ((brightnessLevel - 80) * 100) / 175;
    snprintf(brightStr, sizeof(brightStr), "%d%%", brightPercent);
    tft->setCursor(sliderX + sliderWidth + 8, brightnessY);
    tft->print(brightStr);
    
    // === Voice volume slider ===
    tft->setTextColor(0xFFFF);
    tft->setCursor(sliderMargin, volumeY - 16);
    tft->print("VOICE VOLUME");
    
    // Draw slider track
    tft->drawRect(sliderX - 2, volumeY - 2, sliderWidth + 4, sliderHeight + 4, 0x4208);
    tft->fillRect(sliderX, volumeY, sliderWidth, sliderHeight, 0x2104);
    
    // Fill based on volume level (0-100 range)
    int volumeFill = (volumeLevel * sliderWidth) / 100;
    tft->fillRect(sliderX, volumeY, volumeFill, sliderHeight, 0x001F);  // Blue for volume
    
    // Thumb
    int volThumbX = sliderX + volumeFill - 4;
    if (volThumbX < sliderX) volThumbX = sliderX;
    if (volThumbX > sliderX + sliderWidth - 8) volThumbX = sliderX + sliderWidth - 8;
    tft->fillRect(volThumbX, volumeY - 4, 8, sliderHeight + 8, 0xFFFF);
    
    // Percentage text
    char volStr[8];
    snprintf(volStr, sizeof(volStr), "%d%%", volumeLevel);
    tft->setCursor(sliderX + sliderWidth + 8, volumeY);
    tft->print(volStr);
    
    // Instructions at bottom
    tft->setTextSize(1);
    tft->setTextColor(0x8410);  // Gray
    tft->setCursor((SCREEN_WIDTH - 220) / 2, 155);
    tft->print("Touch sliders - BOOT to save");
    
    tft->flush();
#endif
}

void V1Display::updateBrightnessSlider(uint8_t level) {
    // Re-render the slider with new level
    // Also apply the brightness in real-time for visual feedback
    setBrightness(level);
    showBrightnessSlider(level);
}

void V1Display::updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider) {
    // Apply brightness in real-time for visual feedback
    setBrightness(brightnessLevel);
    showSettingsSliders(brightnessLevel, volumeLevel);
}

// Returns which slider was touched: 0=brightness, 1=volume, -1=none
// Touch Y is inverted relative to display Y:
//   Low touch Y = bottom of display = volume slider
//   High touch Y = top of display = brightness slider
int V1Display::getActiveSliderFromTouch(int16_t touchY) {
    if (touchY <= 60) return 1;   // Volume (bottom of display)
    if (touchY >= 80) return 0;   // Brightness (top of display)
    return -1;  // Dead zone between sliders
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

void V1Display::setBLEProxyStatus(bool proxyEnabled, bool clientConnected, bool receivingData) {
#if defined(DISPLAY_WAVESHARE_349)
    // Detect app disconnect - was connected, now isn't
    // Reset VOL 0 warning state immediately so it can trigger again
    if (bleProxyClientConnected && !clientConnected) {
        volumeZeroDetectedMs = 0;
        volumeZeroWarningStartMs = 0;
        volumeZeroWarningShown = false;
        volumeZeroWarningAcknowledged = false;
    }
    
    // Check if proxy client connection changed - update RSSI display
    bool proxyChanged = (clientConnected != bleProxyClientConnected);
    
    // Check if receiving state changed (for heartbeat visual)
    bool receivingChanged = (receivingData != bleReceivingData);
    
    if (bleProxyDrawn &&
        proxyEnabled == bleProxyEnabled &&
        clientConnected == bleProxyClientConnected &&
        !receivingChanged) {
        return;  // No visual change needed
    }

    bleProxyEnabled = proxyEnabled;
    bleProxyClientConnected = clientConnected;
    bleReceivingData = receivingData;
    drawBLEProxyIndicator();
    
    // Update RSSI display when proxy connection changes
    if (proxyChanged) {
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    
    flush();
#endif
}

void V1Display::drawBaseFrame() {
    // Clean black background (t4s3-style)
    TFT_CALL(fillScreen)(PALETTE_BG);
    bleProxyDrawn = false;  // Force indicator redraw after full clears
    s_forceFrequencyRedraw = true;  // Force frequency cache invalidation after screen clear
    s_forceBatteryRedraw = true;    // Force battery percent cache invalidation after screen clear
    s_forceBandRedraw = true;       // Force band indicator cache invalidation after screen clear
    s_forceSignalBarsRedraw = true; // Force signal bars cache invalidation after screen clear
    s_forceArrowRedraw = true;      // Force arrow cache invalidation after screen clear
    s_forceStatusBarRedraw = true;  // Force status bar cache invalidation after screen clear
    s_forceMuteIconRedraw = true;   // Force mute icon cache invalidation after screen clear
    s_forceTopCounterRedraw = true; // Force top counter cache invalidation after screen clear
    drawBLEProxyIndicator();  // Redraw BLE icon after screen clear
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
    } else if (c == '=' || c == '#') {
        // Three horizontal bars for laser alert (top, middle, bottom)
        // '#' is the decoded byte value for laser (73)
        segments[0] = segments[6] = segments[3] = true;
    } else if (c == 'A' || c == 'a') {
        // A = all but bottom segment
        segments[0] = segments[1] = segments[2] = segments[4] = segments[5] = segments[6] = true;
    } else if (c == 'L') {
        // Full L: bottom + lower-left + upper-left
        segments[3] = segments[4] = segments[5] = true;
    } else if (c == 'l' || c == '&') {
        // Logic (lowercase) L: bottom + lower-left only
        // '&' is used in decoder for little L (logic mode, byte value 24)
        segments[3] = segments[4] = true;
    } else if (c == 'S' || c == 's') {
        // S = top, upper-left, middle, lower-right, bottom (like 5)
        segments[0] = segments[5] = segments[6] = segments[2] = segments[3] = true;
    } else if (c == 'E') {
        // E = top, upper-left, middle, lower-left, bottom
        segments[0] = segments[5] = segments[6] = segments[4] = segments[3] = true;
    } else if (c == 'R') {
        // R = top, upper-left, middle, lower-left (fuller capital R)
        segments[0] = segments[5] = segments[6] = segments[4] = true;
    } else if (c == 'r') {
        // r = middle, lower-left (lowercase r style)
        segments[6] = segments[4] = true;
    } else if (c == 'J') {
        // J = Junk: upper-right, lower-right, bottom, lower-left
        segments[1] = segments[2] = segments[3] = segments[4] = true;
    } else if (c == 'P') {
        // P = Photo radar: top, upper-right, upper-left, middle, lower-left
        segments[0] = segments[1] = segments[5] = segments[6] = segments[4] = true;
    } else if (c == 'F') {
        // F = top, upper-left, middle, lower-left
        segments[0] = segments[5] = segments[6] = segments[4] = true;
    } else if (c == 'C') {
        // C = top, upper-left, lower-left, bottom
        segments[0] = segments[5] = segments[4] = segments[3] = true;
    } else if (c == 'U') {
        // U = upper-right, lower-right, bottom, lower-left, upper-left
        segments[1] = segments[2] = segments[3] = segments[4] = segments[5] = true;
    } else if (c == 'u') {
        // lowercase u = lower-right, bottom, lower-left
        segments[2] = segments[3] = segments[4] = true;
    } else if (c == 'b') {
        // b = upper-left, lower-left, bottom, lower-right, middle
        segments[5] = segments[4] = segments[3] = segments[2] = segments[6] = true;
    } else if (c == 'c') {
        // lowercase c = middle, lower-left, bottom
        segments[6] = segments[4] = segments[3] = true;
    } else if (c == 'd') {
        // d = upper-right, lower-right, bottom, lower-left, middle
        segments[1] = segments[2] = segments[3] = segments[4] = segments[6] = true;
    } else if (c == 'e') {
        // e = top, upper-left, lower-left, bottom, middle, upper-right (differs from capital E by having bottom right)
        segments[0] = segments[5] = segments[4] = segments[3] = segments[6] = true;
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
// Uses Segment7 TTF font (JBV1 style) if available, falls back to software renderer
void V1Display::drawTopCounterClassic(char symbol, bool muted, bool showDot) {
    // Change detection: skip redraw if nothing changed
    static char lastSymbol = '\0';
    static bool lastMuted = false;
    static bool lastShowDot = false;
    static uint16_t lastBogeyColor = 0;
    
    const V1Settings& s = settingsManager.get();
    
    // Check if color setting changed
    bool colorChanged = (s.colorBogey != lastBogeyColor);
    
    // Skip redraw if nothing changed (unless forced after screen clear)
    if (!s_forceTopCounterRedraw && !colorChanged &&
        symbol == lastSymbol && muted == lastMuted && showDot == lastShowDot) {
        return;
    }
    s_forceTopCounterRedraw = false;
    lastSymbol = symbol;
    lastMuted = muted;
    lastShowDot = showDot;
    lastBogeyColor = s.colorBogey;
    
    // Use bogey color for digits, muted color if muted, otherwise bogey color
    bool isDigit = (symbol >= '0' && symbol <= '9');
    uint16_t color;
    if (isDigit) {
        color = s.colorBogey;
    } else {
        color = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBogey;
    }
    
    // Build display string
    char buf[3] = {symbol, 0, 0};
    if (showDot) {
        buf[1] = '.';
    }
    
    if (ofrSegment7Initialized) {
        // Use Segment7 TTF font (JBV1 style)
        const int fontSize = 60;
        const int x = 18;
        const int y = 8;
        
        // Clear area
        FILL_RECT(x - 2, y - 2, 55, fontSize + 8, PALETTE_BG);
        
        // Convert RGB565 to RGB888 for OpenFontRender
        uint8_t bgR = (PALETTE_BG >> 11) << 3;
        uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        ofrSegment7.setBackgroundColor(bgR, bgG, bgB);
        ofrSegment7.setFontSize(fontSize);
        ofrSegment7.setFontColor((color >> 11) << 3, ((color >> 5) & 0x3F) << 2, (color & 0x1F) << 3);
        ofrSegment7.setCursor(x, y);
        ofrSegment7.printf("%s", buf);
    } else {
        // Fallback to software 7-segment renderer
#if defined(DISPLAY_WAVESHARE_349)
        const float scale = 2.2f;
#else
        const float scale = 2.0f;
#endif
        SegMetrics m = segMetrics(scale);
        int x = 12;
        int y = 10;
        FILL_RECT(x - 2, y - 2, m.digitW + m.dot + 12, m.digitH + 8, PALETTE_BG);
        drawSevenSegmentText(buf, x, y, scale, color, PALETTE_BG);
    }
}

// Router: bogey counter uses Classic 7-segment for all styles (laser flag support + perf)
// Frequency still uses OFR for Modern/Hemi/Serpentine
void V1Display::drawTopCounter(char symbol, bool muted, bool showDot) {
    // Always use Classic 7-segment for bogey counter (both styles)
    // This ensures laser flag ('=') and all symbols render correctly
    drawTopCounterClassic(symbol, muted, showDot);
}

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
    char mainBuf[4];
    snprintf(mainBuf, sizeof(mainBuf), "%dV", mainVol);
    TFT_CALL(setTextColor)(s.colorVolumeMain, PALETTE_BG);
    GFX_drawString(tft, mainBuf, x, y);
    
    // Draw mute volume "0M" in mute volume color, offset to the right
    char muteBuf[4];
    snprintf(muteBuf, sizeof(muteBuf), "%dM", muteVol);
    TFT_CALL(setTextColor)(s.colorVolumeMute, PALETTE_BG);
    GFX_drawString(tft, muteBuf, x + 36, y);  // Aligned with RSSI number
}

void V1Display::drawRssiIndicator(int rssi) {
    // Check if RSSI indicator is hidden
    const V1Settings& s = settingsManager.get();
    if (s.hideRssiIndicator) {
        return;  // Don't draw anything
    }
    
    // Draw BLE RSSI below volume indicator
    // Shows V1 RSSI and JBV1 RSSI (if connected) stacked vertically
    const int x = 8;
    // Evenly spaced: volume at y=75, height 16, gap 8 -> y=99
    const int y = 99;
    const int lineHeight = 22;  // Increased spacing between V and P lines
    const int clearW = 70;
    const int clearH = lineHeight * 2;  // Room for two lines
    
    // Clear the area first
    FILL_RECT(x, y, clearW, clearH, PALETTE_BG);
    
    // Get both RSSIs
    int v1Rssi = rssi;  // V1 RSSI passed in
    int jbv1Rssi = bleClient.getProxyClientRssi();  // JBV1/phone RSSI
    
    GFX_setTextDatum(TL_DATUM);
    TFT_CALL(setTextSize)(2);  // Match volume text size
    
    // Draw V1 RSSI if connected
    if (v1Rssi != 0) {
        // Draw "V " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiV1, PALETTE_BG);
        GFX_drawString(tft, "V ", x, y);
        
        // Color code RSSI value: green >= -60, yellow -60 to -80, red < -80
        uint16_t rssiColor;
        if (v1Rssi >= -60) {
            rssiColor = COLOR_GREEN;
        } else if (v1Rssi >= -80) {
            rssiColor = COLOR_YELLOW;
        } else {
            rssiColor = COLOR_RED;
        }
        
        TFT_CALL(setTextColor)(rssiColor, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", v1Rssi);
        GFX_drawString(tft, buf, x + 24, y);  // Offset for "V " width
    }
    
    // Draw JBV1 RSSI below V1 RSSI if connected
    if (jbv1Rssi != 0) {
        // Draw "P " label with configurable color
        TFT_CALL(setTextColor)(s.colorRssiProxy, PALETTE_BG);
        GFX_drawString(tft, "P ", x, y + lineHeight);
        
        // Color code RSSI value
        uint16_t rssiColor;
        if (jbv1Rssi >= -60) {
            rssiColor = COLOR_GREEN;
        } else if (jbv1Rssi >= -80) {
            rssiColor = COLOR_YELLOW;
        } else {
            rssiColor = COLOR_RED;
        }
        
        TFT_CALL(setTextColor)(rssiColor, PALETTE_BG);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", jbv1Rssi);
        GFX_drawString(tft, buf, x + 24, y + lineHeight);  // Offset for "P " width
    }
}

void V1Display::drawMuteIcon(bool muted) {
    // Change detection: skip redraw if nothing changed
    static bool lastMutedState = false;
    static bool lastLockoutMuted = false;
    
    // Skip redraw if nothing changed (unless forced after screen clear)
    if (!s_forceMuteIconRedraw && muted == lastMutedState && lockoutMuted == lastLockoutMuted) {
        return;
    }
    s_forceMuteIconRedraw = false;
    lastMutedState = muted;
    lastLockoutMuted = lockoutMuted;
    
    // Draw badge at fixed top position (top ~10% of screen)
#if defined(DISPLAY_WAVESHARE_349)
    const int leftMargin = 120;    // After band indicators
    const int rightMargin = 200;   // Before signal bars (at X=440)
#else
    const int leftMargin = 0;
    const int rightMargin = 120;
#endif
    int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    
    // Badge dimensions - wider for "LOCKOUT" text
    int w = lockoutMuted ? 130 : 110;
    int h = 26;
    int x = leftMargin + (maxWidth - w) / 2;  // Center between bands and signal bars
    int y = 5;  // Fixed near top of screen
    
    if (muted) {
        // Draw badge with muted styling
        uint16_t outline = PALETTE_MUTED;
        uint16_t fill = PALETTE_MUTED;
        
        FILL_ROUND_RECT(x, y, w, h, 5, fill);
        DRAW_ROUND_RECT(x, y, w, h, 5, outline);
        
        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);  // Larger text for visibility
        TFT_CALL(setTextColor)(PALETTE_BG, fill);
        int cx = x + w / 2;
        int cy = y + h / 2;
        
        // Show different text for lockout mute vs user mute
        const char* muteText = lockoutMuted ? "LOCKOUT" : "MUTED";
        // Pseudo-bold: draw twice with slight offset
        GFX_drawString(tft, muteText, cx, cy);
        GFX_drawString(tft, muteText, cx + 1, cy);
    } else {
        // Clear the badge area when not muted (use wider area to cover both sizes)
        FILL_RECT(leftMargin + (maxWidth - 130) / 2, y, 130, h, PALETTE_BG);
        // Reset lockout flag when unmuted
        lockoutMuted = false;
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
        bool needsRedraw = s_forceBatteryRedraw ||  // Screen was cleared
                           (!lastPctVisible) ||
                           (pct != lastPctDrawn) ||
                           (textColor != lastPctColor) ||
                           ((nowMs - lastPctDrawMs) >= PCT_FORCE_REDRAW_MS);

        if (!needsRedraw) {
            return;  // Skip expensive render when nothing changed
        }
        
        // Clear force flag - we're handling it
        s_forceBatteryRedraw = false;

        // Format percentage string (no % to save space)
        char pctStr[4];
        snprintf(pctStr, sizeof(pctStr), "%d", pct);

        // Always clear the text area before drawing (covers shorter numbers)
        // Positioned to avoid top arrow which extends to roughly SCREEN_WIDTH - 14
        FILL_RECT(SCREEN_WIDTH - 50, 0, 48, 30, PALETTE_BG);

        // Draw using OpenFontRender if available; fall back to built-in font otherwise
        if (ofrInitialized) {
            const int fontSize = 20;  // Larger for better visibility
            uint8_t bgR = (PALETTE_BG >> 11) << 3;
            uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
            uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
            ofr.setBackgroundColor(bgR, bgG, bgB);
            ofr.setFontColor((textColor >> 11) << 3, ((textColor >> 5) & 0x3F) << 2, (textColor & 0x1F) << 3);
            ofr.setFontSize(fontSize);

            FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, pctStr);
            int textW = bbox.xMax - bbox.xMin;
            int textH = bbox.yMax - bbox.yMin;

            // Position text at top-right corner - use fixed Y position near top
            int textX = SCREEN_WIDTH - textW - 4;
            int textY = 2;  // Fixed: 2 pixels from top (OFR draws from top of glyph, not baseline)

            ofr.setCursor(textX, textY);
            ofr.printf("%s", pctStr);
        } else {
            // Fallback: built-in font, right-aligned near top-right
            GFX_setTextDatum(TR_DATUM);
            TFT_CALL(setTextSize)(2);  // Larger for better visibility
            TFT_CALL(setTextColor)(textColor, PALETTE_BG);
            GFX_drawString(tft, pctStr, SCREEN_WIDTH - 4, 12);
        }

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

    // Icon color from settings: connected vs disconnected
    // When connected but not receiving data, dim further to show "stale" state
    uint16_t btColor;
    if (bleProxyClientConnected) {
        // Connected: bright green when receiving, dimmed when stale
        btColor = bleReceivingData ? dimColor(s.colorBleConnected, 85)
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
    
    // Show WiFi icon when Setup Mode (AP) is active
    bool isSetupMode = wifiManager.isSetupModeActive();
    
    if (!isSetupMode) {
        // Clear the WiFi icon area when Setup Mode is off
        FILL_RECT(wifiX - 2, wifiY - 2, wifiSize + 4, wifiSize + 4, PALETTE_BG);
        return;
    }
    
    // Check if any clients are connected to the AP
    bool hasClients = WiFi.softAPgetStationNum() > 0;
    
    // WiFi icon color: connected vs disconnected (like BLE icon)
    uint16_t wifiColor = hasClients ? dimColor(s.colorWiFiConnected, 85)
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

void V1Display::showResting(bool forceRedraw) {
    // Always use multi-alert layout positioning
    g_multiAlertMode = true;
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
        if (bleClient.isConnected() && savedBogeyChar != 0) {
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
        
        // Frequency display: KITT scanner if enabled, otherwise normal frequency display
        const V1Settings& s = settingsManager.get();
        if (s.kittScannerEnabled) {
            // KITT scanner will be drawn on each frame in the periodic update
            // Just clear the area here
        } else if (s.displayStyle != DISPLAY_STYLE_MODERN) {
            drawFrequency(0, BAND_NONE);
        }
        
        // Mute indicator off
        drawMuteIcon(false);
        
        // Profile indicator
        drawProfileIndicator(profileSlot);
        
        // Status bar (GPS/CAM/OBD indicators)
        drawStatusBar();

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

void V1Display::forceNextRedraw() {
    // Reset lastState to force next update() to detect all changes and redraw
    lastState = DisplayState();
    // Also reset screen mode so next update knows we need full redraw
    currentScreen = ScreenMode::Resting;
    // Reset all static change tracking variables (volume, mode, arrows, etc.)
    // This ensures the next update() does a full redraw with fresh data
    resetChangeTracking();
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
    // Volume indicator not shown in scanning state (no DisplayState available)
    drawBandIndicators(0, false);
    drawVerticalSignalBars(0, 0, BAND_KA, false);
    drawDirectionArrow(DIR_NONE, false);
    drawMuteIcon(false);
    drawProfileIndicator(currentProfileSlot);
    drawStatusBar();  // GPS/CAM/OBD status indicators
    
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
        
        // Center between band indicators and signal bars (match frequency positioning)
        const int leftMargin = 120;   // After band indicators
        const int rightMargin = 200;  // Before signal bars
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - textWidth) / 2;
        int y = getEffectiveScreenHeight() - 72;  // Match frequency positioning
        
        FILL_RECT(x - 4, y - textHeight - 4, textWidth + 8, textHeight + 12, PALETTE_BG);
        ofr.setCursor(x, y);
        ofr.printf("%s", text);
    } else if (s.displayStyle == DISPLAY_STYLE_HEMI && ofrHemiInitialized) {
        // Hemi style: use Hemi Head via OFR (retro speedometer look)
        const int fontSize = 76;  // Larger for retro impact
        ofrHemi.setFontColor(s.colorBandKa, PALETTE_BG);
        ofrHemi.setFontSize(fontSize);
        
        const char* text = "SCAN";
        FT_BBox bbox = ofrHemi.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
        int textWidth = bbox.xMax - bbox.xMin;
        int textHeight = bbox.yMax - bbox.yMin;
        
        const int leftMargin = 120;
        const int rightMargin = 200;
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - textWidth) / 2;
        int y = getEffectiveScreenHeight() - 72;
        
        FILL_RECT(x - 4, y - textHeight - 4, textWidth + 8, textHeight + 12, PALETTE_BG);
        ofrHemi.setCursor(x, y);
        ofrHemi.printf("%s", text);
    } else if (s.displayStyle == DISPLAY_STYLE_SERPENTINE && ofrSerpentineInitialized) {
        // Serpentine style: JB's favorite font
        const int fontSize = 65;
        ofrSerpentine.setFontColor(s.colorBandKa, PALETTE_BG);
        ofrSerpentine.setFontSize(fontSize);
        
        const char* text = "SCAN";
        FT_BBox bbox = ofrSerpentine.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
        int textWidth = bbox.xMax - bbox.xMin;
        int textHeight = bbox.yMax - bbox.yMin;
        
        const int leftMargin = 120;
        const int rightMargin = 200;
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - textWidth) / 2;
        int y = getEffectiveScreenHeight() - 72;
        
        FILL_RECT(x - 4, y - textHeight - 4, textWidth + 8, textHeight + 12, PALETTE_BG);
        ofrSerpentine.setCursor(x, y);
        ofrSerpentine.printf("%s", text);
    } else if (ofrSegment7Initialized) {
        // Classic style: use Segment7 TTF font (JBV1 style)
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
        ofrSegment7.setBackgroundColor(bgR, bgG, bgB);
        ofrSegment7.setFontSize(fontSize);
        ofrSegment7.setFontColor((s.colorBandKa >> 11) << 3, ((s.colorBandKa >> 5) & 0x3F) << 2, (s.colorBandKa & 0x1F) << 3);
        ofrSegment7.setCursor(x, y);
        ofrSegment7.printf("%s", text);
    } else {
        // Fallback: software 14-segment display
#if defined(DISPLAY_WAVESHARE_349)
        const float scale = 2.3f;  // Match frequency scale
#else
        const float scale = 1.7f;
#endif
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
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->flush();
#endif

    currentScreen = ScreenMode::Scanning;
    lastRestingProfileSlot = -1;
}

// Static flag to signal display change tracking reset on next update
static bool s_resetChangeTrackingFlag = false;

// RSSI periodic update timer (shared between resting and alert modes)
static unsigned long s_lastRssiUpdateMs = 0;
static constexpr unsigned long RSSI_UPDATE_INTERVAL_MS = 2000;  // Update RSSI every 2 seconds

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
    
    // Draw version number in bottom-right corner
    GFX_setTextDatum(BR_DATUM);  // Bottom-right alignment
    TFT_CALL(setTextSize)(2);
    TFT_CALL(setTextColor)(0x7BEF, PALETTE_BG);  // Gray text (mid-gray RGB565)
    GFX_drawString(tft, "v" FIRMWARE_VERSION, SCREEN_WIDTH - 8, SCREEN_HEIGHT - 6);

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
    
    // Don't process resting update if we're in Scanning mode - wait for showResting() to be called
    if (currentScreen == ScreenMode::Scanning) {
        return;
    }
    
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

    // In resting mode (no alerts), never show muted visual - just normal display
    // Apps commonly set main volume to 0 when idle, adjusting on new alerts
    // The muted state should only affect active alert display, not resting
    bool effectiveMuted = false;

    // Track last debounced bands for change detection
    static uint8_t lastRestingDebouncedBands = 0;
    static uint8_t lastRestingSignalBars = 0;
    static uint8_t lastRestingArrows = 0;
    static uint8_t lastRestingMainVol = 255;
    static uint8_t lastRestingMuteVol = 255;
    static uint8_t lastRestingBogeyByte = 0;  // Track V1's bogey counter for change detection
    
    // Reset resting statics when change tracking reset is requested (on V1 disconnect)
    if (s_resetChangeTrackingFlag) {
        firstUpdate = true;
        lastRestingDebouncedBands = 0;
        lastRestingSignalBars = 0;
        lastRestingArrows = 0;
        lastRestingMainVol = 255;
        lastRestingMuteVol = 255;
        lastRestingBogeyByte = 0;
        s_lastRssiUpdateMs = 0;
        // Don't clear the flag here - let the alert update() clear it
    }
    
    // Check if RSSI needs periodic refresh (every 2 seconds)
    bool rssiNeedsUpdate = (now - s_lastRssiUpdateMs) >= RSSI_UPDATE_INTERVAL_MS;
    
    // Separate full redraw triggers from incremental updates
    bool needsFullRedraw =
        firstUpdate ||
        flashJustExpired ||
        wasPersistedMode ||  // Force full redraw when leaving persisted mode
        restingDebouncedBands != lastRestingDebouncedBands ||
        effectiveMuted != lastState.muted;
    
    bool arrowsChanged = (state.arrows != lastRestingArrows);
    bool signalBarsChanged = (state.signalBars != lastRestingSignalBars);
    bool volumeChanged = (state.mainVolume != lastRestingMainVol || state.muteVolume != lastRestingMuteVol);
    bool bogeyCounterChanged = (state.bogeyCounterByte != lastRestingBogeyByte);
    
    // Check if volume zero warning should be active (for flashing effect)
    // Use current proxy state for accurate check
    bool currentProxyConnected = bleClient.isProxyClientConnected();
    bool volumeWarningActive = false;
    bool shouldStartVolWarningTimer = false;
    
    if (state.mainVolume == 0 && state.hasVolumeData && !currentProxyConnected && !volumeZeroWarningAcknowledged) {
        if (volumeZeroDetectedMs == 0) {
            // Need to start the timer - force full redraw to reach timer-start code
            shouldStartVolWarningTimer = true;
        } else if ((millis() - volumeZeroDetectedMs) >= VOLUME_ZERO_DELAY_MS) {
            // Warning is active if we're past the delay and within the warning duration
            if (volumeZeroWarningStartMs == 0 || (millis() - volumeZeroWarningStartMs) < VOLUME_ZERO_WARNING_DURATION_MS) {
                volumeWarningActive = true;
            }
        }
    }
    
    // Force full redraw when warning is active, shown, or needs to start timer
    if (volumeWarningActive || volumeZeroWarningShown || shouldStartVolWarningTimer) {
        needsFullRedraw = true;
    }
    
    // Check if KITT scanner is enabled and needs continuous animation
    const V1Settings& sKitt = settingsManager.get();
    bool kittScannerActive = sKitt.kittScannerEnabled && !volumeWarningActive;
    
    if (!needsFullRedraw && !arrowsChanged && !signalBarsChanged && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
        // Nothing changed - but KITT scanner needs continuous updates for animation
        if (kittScannerActive) {
            drawKittScanner();
#if defined(DISPLAY_WAVESHARE_349)
            tft->flush();
#endif
        }
        return;
    }
    
    if (!needsFullRedraw && (arrowsChanged || signalBarsChanged || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
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
        const V1Settings& s = settingsManager.get();
        if (volumeChanged && state.supportsVolume() && !s.hideVolumeIndicator) {
            lastRestingMainVol = state.mainVolume;
            lastRestingMuteVol = state.muteVolume;
            drawVolumeIndicator(state.mainVolume, state.muteVolume);
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;  // Reset RSSI timer when we update with volume
        }
        if (rssiNeedsUpdate && !volumeChanged) {
            // Periodic RSSI-only update
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;
        }
        if (bogeyCounterChanged) {
            lastRestingBogeyByte = state.bogeyCounterByte;
            drawTopCounter(state.bogeyCounterChar, effectiveMuted, state.bogeyCounterDot);
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
    lastRestingMainVol = state.mainVolume;
    lastRestingMuteVol = state.muteVolume;
    lastRestingBogeyByte = state.bogeyCounterByte;
    s_lastRssiUpdateMs = now;  // Reset RSSI timer on full redraw
    
    drawBaseFrame();
    // Use V1's decoded bogey counter byte - shows mode, volume, etc.
    char topChar = state.bogeyCounterChar;
    drawTopCounter(topChar, effectiveMuted, state.bogeyCounterDot);
    const V1Settings& s = settingsManager.get();
    if (state.supportsVolume() && !s.hideVolumeIndicator) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);  // Show volume below bogey counter (V1 4.1028+)
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    drawBandIndicators(restingDebouncedBands, effectiveMuted);
    // BLE proxy status indicator
    
    // Determine primary band for frequency and signal bar coloring
    Band primaryBand = BAND_NONE;
    if (restingDebouncedBands & BAND_LASER) primaryBand = BAND_LASER;
    else if (restingDebouncedBands & BAND_KA) primaryBand = BAND_KA;
    else if (restingDebouncedBands & BAND_K) primaryBand = BAND_K;
    else if (restingDebouncedBands & BAND_X) primaryBand = BAND_X;
    
    // Check if we should show volume zero warning:
    // - Main volume is 0
    // - No BLE proxy client (app) connected
    // - We have volume data
    // - Wait 15 seconds before showing (to let JBV1 connect)
    // - Warning shown for 10 seconds after delay
    // - Also trigger if app just disconnected with volume=0
    bool showVolumeWarning = false;
    bool proxyConnected = bleClient.isProxyClientConnected();
    
    if (state.mainVolume == 0 && state.hasVolumeData && !proxyConnected) {
        if (!volumeZeroWarningAcknowledged) {
            if (volumeZeroDetectedMs == 0) {
                // First time detecting volume=0, start the delay timer
                volumeZeroDetectedMs = millis();
            }
            
            unsigned long elapsed = millis() - volumeZeroDetectedMs;
            
            // Check if delay period has passed
            if (elapsed >= VOLUME_ZERO_DELAY_MS) {
                if (volumeZeroWarningStartMs == 0) {
                    // Delay passed, start the actual warning display
                    volumeZeroWarningStartMs = millis();
                    volumeZeroWarningShown = true;
                    // Play beep when VOL 0 warning first appears
                    play_vol0_beep();
                }
                
                // Check if warning period has expired
                if ((millis() - volumeZeroWarningStartMs) < VOLUME_ZERO_WARNING_DURATION_MS) {
                    showVolumeWarning = true;
                } else {
                    // Warning period expired, acknowledge it
                    volumeZeroWarningAcknowledged = true;
                    volumeZeroWarningShown = false;
                }
            }
        }
    } else {
        // Volume is non-zero or app connected - reset warning state
        volumeZeroDetectedMs = 0;
        volumeZeroWarningStartMs = 0;
        volumeZeroWarningShown = false;
        volumeZeroWarningAcknowledged = false;
    }
    
    if (showVolumeWarning) {
        drawVolumeZeroWarning();
    } else if (s.kittScannerEnabled) {
        drawKittScanner();  // Knight Rider easter egg
    } else {
        drawFrequency(0, primaryBand, effectiveMuted);
    }
    
    drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, effectiveMuted);
    // Never draw arrows in resting display - arrows should only appear in live mode
    // when we have actual alert data with frequency. If display packet has arrows but
    // no alert packet arrived, we shouldn't show arrows without frequency.
    drawDirectionArrow(DIR_NONE, effectiveMuted, 0);
    drawMuteIcon(effectiveMuted);
    drawProfileIndicator(currentProfileSlot);
    drawStatusBar();  // GPS/CAM/OBD status indicators at top
    
    // Clear any persisted card slots when entering resting state
    AlertData emptyPriority;
    drawSecondaryAlertCards(nullptr, 0, emptyPriority, effectiveMuted);

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();  // Push canvas to display
#endif

    currentScreen = ScreenMode::Resting;  // Set screen mode after redraw complete
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
    
    // Bogey counter shows V1's decoded display - NOT greyed, always visible
    char topChar = state.bogeyCounterChar;
    drawTopCounter(topChar, false, state.bogeyCounterDot);  // muted=false to keep it visible
    const V1Settings& s = settingsManager.get();
    if (state.supportsVolume() && !s.hideVolumeIndicator) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);  // Show current volume (V1 4.1028+)
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    
    // Band indicator in persisted color
    uint8_t bandMask = alert.band;
    drawBandIndicators(bandMask, true);  // muted=true triggers PALETTE_MUTED_OR_PERSISTED
    
    // Frequency in persisted color (pass muted=true)
    // Note: Photo radar check uses state.bogeyCounterChar even for persisted alerts
    bool isPhotoRadar = (state.bogeyCounterChar == 'P');
    drawFrequency(alert.frequency, alert.band, true, isPhotoRadar);
    
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

    // V1 is source of truth - use activeBands directly, no debouncing
    // This allows V1's native blinking to come through

    // Change detection: check if we need to redraw
    static AlertData lastPriority;
    static uint8_t lastBogeyByte = 0;  // Track V1's bogey counter byte for change detection
    static DisplayState lastMultiState;
    static bool firstRun = true;
    static AlertData lastSecondary[4];
    static uint8_t lastArrows = 0;
    static uint8_t lastSignalBars = 0;
    static uint8_t lastActiveBands = 0;
    
    // Check if reset was requested (e.g., on V1 disconnect)
    if (s_resetChangeTrackingFlag) {
        lastPriority = AlertData();
        lastBogeyByte = 0;
        lastMultiState = DisplayState();
        firstRun = true;
        for (int i = 0; i < 4; i++) lastSecondary[i] = AlertData();
        lastArrows = 0;
        lastSignalBars = 0;
        lastActiveBands = 0;
        s_resetChangeTrackingFlag = false;
    }
    
    bool needsRedraw = false;
    
    // Frequency tolerance for V1 jitter (V1 can report ±1-3 MHz variation between packets)
    const uint32_t FREQ_TOLERANCE_MHZ = 5;
    auto freqDifferent = [FREQ_TOLERANCE_MHZ](uint32_t a, uint32_t b) -> bool {
        uint32_t diff = (a > b) ? (a - b) : (b - a);
        return diff > FREQ_TOLERANCE_MHZ;
    };
    
    // Always redraw on first run, entering live mode, or when transitioning from persisted mode
    if (firstRun) { needsRedraw = true; firstRun = false; }
    else if (enteringLiveMode) { needsRedraw = true; }
    else if (wasPersistedMode) { needsRedraw = true; }
    // V1 is source of truth - always redraw when priority alert changes
    // Use frequency tolerance to avoid full redraws from V1 jitter
    else if (freqDifferent(priority.frequency, lastPriority.frequency)) { needsRedraw = true; }
    else if (priority.band != lastPriority.band) { needsRedraw = true; }
    else if (state.muted != lastMultiState.muted) { needsRedraw = true; }
    // Note: bogey counter changes are handled via incremental update (bogeyCounterChanged) for rapid response
    
    // Also check if any secondary alert changed (set-based, not order-based)
    // V1 may reorder alerts by signal strength - we only care if the SET of alerts changed
    if (!needsRedraw) {
        // Compare counts first
        int lastAlertCount = 0;
        for (int i = 0; i < 4; i++) {
            if (lastSecondary[i].band != BAND_NONE) lastAlertCount++;
        }
        if (alertCount != lastAlertCount) {
            needsRedraw = true;
        } else {
            // Check if any current alert is NOT in last set (set membership test)
            // Use frequency tolerance (±5 MHz) to handle V1 jitter
            const uint32_t FREQ_TOLERANCE_MHZ = 5;
            for (int i = 0; i < alertCount && i < 4 && !needsRedraw; i++) {
                bool foundInLast = false;
                for (int j = 0; j < 4; j++) {
                    if (allAlerts[i].band == lastSecondary[j].band) {
                        if (allAlerts[i].band == BAND_LASER) {
                            foundInLast = true;
                        } else {
                            uint32_t diff = (allAlerts[i].frequency > lastSecondary[j].frequency) 
                                ? (allAlerts[i].frequency - lastSecondary[j].frequency) 
                                : (lastSecondary[j].frequency - allAlerts[i].frequency);
                            if (diff <= FREQ_TOLERANCE_MHZ) foundInLast = true;
                        }
                        if (foundInLast) break;
                    }
                }
                if (!foundInLast) {
                    needsRedraw = true;
                }
            }
        }
    }
    
    // Track arrow, signal bar, and band changes separately for incremental update
    // Arrow display depends on per-profile priorityArrowOnly setting
    // When priorityArrowOnly is enabled, still respect V1's arrow blinking by masking with state.arrows
    // V1 handles blinking by toggling image1 arrow bits - we follow that
    Direction arrowsToShow;
    if (settingsManager.getSlotPriorityArrowOnly(s.activeSlot)) {
        // Show priority arrow only when V1 is also showing that direction
        arrowsToShow = static_cast<Direction>(state.priorityArrow & state.arrows);
    } else {
        arrowsToShow = state.arrows;
    }
    bool arrowsChanged = (arrowsToShow != lastArrows);
    bool signalBarsChanged = (state.signalBars != lastSignalBars);
    bool bandsChanged = (state.activeBands != lastActiveBands);
    bool bogeyCounterChanged = (state.bogeyCounterByte != lastBogeyByte);
    
    // Volume tracking
    static uint8_t lastMainVol = 255;
    static uint8_t lastMuteVol = 255;
    bool volumeChanged = (state.mainVolume != lastMainVol || state.muteVolume != lastMuteVol);
    
    // Check if RSSI needs periodic refresh (every 2 seconds)
    unsigned long now = millis();
    bool rssiNeedsUpdate = (now - s_lastRssiUpdateMs) >= RSSI_UPDATE_INTERVAL_MS;
    
    // Force periodic redraw when something is flashing (for blink animation)
    // Check if any arrows or bands are marked as flashing
    bool hasFlashing = (state.flashBits != 0) || (state.bandFlashBits != 0);
    static unsigned long lastFlashRedraw = 0;
    bool needsFlashUpdate = false;
    if (hasFlashing) {
        if (now - lastFlashRedraw >= 75) {  // Redraw at ~13Hz for smoother blink
            needsFlashUpdate = true;
            lastFlashRedraw = now;
        }
    }
    
    if (!needsRedraw && !arrowsChanged && !signalBarsChanged && !bandsChanged && !needsFlashUpdate && !volumeChanged && !bogeyCounterChanged && !rssiNeedsUpdate) {
        // Nothing changed on main display, but still process cards for expiration
        drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
#if defined(DISPLAY_WAVESHARE_349)
        tft->flush();
#endif
        return;
    }
    
    if (!needsRedraw && (arrowsChanged || signalBarsChanged || bandsChanged || needsFlashUpdate || volumeChanged || bogeyCounterChanged || rssiNeedsUpdate)) {
        // Only arrows, signal bars, bands, or bogey count changed - do incremental update without full redraw
        // Also handle flash updates (periodic redraw for blink animation)
        if (arrowsChanged || (needsFlashUpdate && state.flashBits != 0)) {
            lastArrows = arrowsToShow;
            drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
        }
        if (signalBarsChanged) {
            lastSignalBars = state.signalBars;
            drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
        }
        if (bandsChanged || (needsFlashUpdate && state.bandFlashBits != 0)) {
            lastActiveBands = state.activeBands;
            drawBandIndicators(state.activeBands, state.muted, state.bandFlashBits);
        }
        if (volumeChanged && state.supportsVolume() && !s.hideVolumeIndicator) {
            lastMainVol = state.mainVolume;
            lastMuteVol = state.muteVolume;
            drawVolumeIndicator(state.mainVolume, state.muteVolume);
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;  // Reset RSSI timer when we update with volume
        }
        if (rssiNeedsUpdate && !volumeChanged) {
            // Periodic RSSI-only update
            drawRssiIndicator(bleClient.getConnectionRssi());
            s_lastRssiUpdateMs = now;
        }
        if (bogeyCounterChanged) {
            // Bogey counter update - use V1's decoded byte (shows J, P, volume, etc.)
            lastBogeyByte = state.bogeyCounterByte;
            drawTopCounter(state.bogeyCounterChar, state.muted, state.bogeyCounterDot);
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
    lastBogeyByte = state.bogeyCounterByte;
    lastMultiState = state;
    // Use same arrowsToShow logic as computed above for change detection
    lastArrows = arrowsToShow;
    lastSignalBars = state.signalBars;
    lastActiveBands = state.activeBands;
    lastMainVol = state.mainVolume;
    lastMuteVol = state.muteVolume;
    s_lastRssiUpdateMs = now;  // Reset RSSI timer on full redraw
    for (int i = 0; i < alertCount && i < 4; i++) {
        lastSecondary[i] = allAlerts[i];
    }
    
    DISP_PERF_START();
    drawBaseFrame();
    DISP_PERF_LOG("drawBaseFrame");

    // V1 is source of truth - use activeBands directly (allows blinking)
    uint8_t bandMask = state.activeBands;
    
    // Bogey counter - use V1's decoded byte (shows J=Junk, P=Photo, volume, etc.)
    drawTopCounter(state.bogeyCounterChar, state.muted, state.bogeyCounterDot);
    
    const V1Settings& settings = settingsManager.get();
    if (state.supportsVolume() && !settings.hideVolumeIndicator) {
        drawVolumeIndicator(state.mainVolume, state.muteVolume);  // Show volume below bogey counter (V1 4.1028+)
        drawRssiIndicator(bleClient.getConnectionRssi());
    }
    DISP_PERF_LOG("counters+vol");
    
    // Main alert display (frequency, bands, arrows, signal bars)
    // Use state.signalBars which is the MAX across ALL alerts (calculated in packet_parser)
    bool isPhotoRadar = (state.bogeyCounterChar == 'P');
    drawFrequency(priority.frequency, priority.band, state.muted, isPhotoRadar);
    DISP_PERF_LOG("drawFrequency");
    drawBandIndicators(bandMask, state.muted, state.bandFlashBits);
    drawVerticalSignalBars(state.signalBars, state.signalBars, priority.band, state.muted);
    DISP_PERF_LOG("bands+bars");
    
    // Arrow display: use priority arrow only if setting enabled, otherwise all V1 arrows
    // (arrowsToShow already computed above for change detection)
    drawDirectionArrow(arrowsToShow, state.muted, state.flashBits);
    drawMuteIcon(state.muted);
    drawProfileIndicator(currentProfileSlot);
    drawStatusBar();  // GPS/CAM/OBD status indicators at top
    DISP_PERF_LOG("arrows+icons");
    
    // Force card redraw since drawBaseFrame cleared the screen
    forceCardRedraw = true;
    
    // Draw secondary alert cards at bottom
    drawSecondaryAlertCards(allAlerts, alertCount, priority, state.muted);
    DISP_PERF_LOG("cards");
    
    // Keep g_multiAlertMode true while in multi-alert - only reset when going to single-alert mode

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();
    DISP_PERF_LOG("flush");
#endif

    lastAlert = priority;
    lastState = state;
}

// Draw mini alert cards for secondary (non-priority) alerts
// With persistence: cards stay visible (greyed) for a grace period after alert disappears
void V1Display::drawSecondaryAlertCards(const AlertData* alerts, int alertCount, const AlertData& priority, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const int cardH = SECONDARY_ROW_HEIGHT;  // 57px (compact with uniform signal bars)
    const int cardY = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT;  // Y=116
    const int cardW = 145;     // Card width (wider to fit freq + band)
    const int cardSpacing = 10;  // Increased spacing between cards
    const int leftMargin = 120;   // After band indicators
    const int rightMargin = 200;  // Before signal bars (at X=440)
    const int availableWidth = SCREEN_WIDTH - leftMargin - rightMargin;  // 320px
    // Center two cards in available space
    const int totalCardsWidth = cardW * 2 + cardSpacing;  // 300px
    const int startX = leftMargin + (availableWidth - totalCardsWidth) / 2;  // Center offset
    
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
    
    // Track what was drawn at each POSITION (0 or 1) for incremental updates
    static struct {
        bool isCamera = false;      // Was this a camera or V1 card?
        // V1 card state
        Band band = BAND_NONE;
        uint32_t frequency = 0;
        uint8_t direction = 0;
        bool isGraced = false;
        bool wasMuted = false;
        uint8_t bars = 0;           // Signal strength bars (0-6)
        // Camera card state
        int cameraIndex = -1;
        char typeName[16] = {0};
        int distanceFt = -1;        // Distance in feet for comparison
        uint16_t color = 0;
    } lastDrawnPositions[2];
    static int lastDrawnCount = 0;
    
    // Track profile changes - clear cards when profile rotates
    static int lastCardProfileSlot = -1;
    if (settings.activeSlot != lastCardProfileSlot) {
        lastCardProfileSlot = settings.activeSlot;
        // Clear all card state on profile change
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
            lastDrawnPositions[c].band = BAND_NONE;
            lastDrawnPositions[c].frequency = 0;
            lastDrawnPositions[c].bars = 0;
            lastDrawnPositions[c].isCamera = false;
            lastDrawnPositions[c].cameraIndex = -1;
            lastDrawnPositions[c].distanceFt = -1;
            lastDrawnPositions[c].typeName[0] = '\0';
        }
        lastDrawnCount = 0;
        lastPriorityForCards = AlertData();
    }
    
    // Check if any camera cards are active
    bool hasActiveCameras = false;
    for (int i = 0; i < MAX_CAMERA_CARDS; i++) {
        if (cameraCards[i].active && cameraCards[i].typeName[0] != '\0') {
            hasActiveCameras = true;
            break;
        }
    }
    
    // If called with nullptr alerts and count 0, clear V1 card state
    // BUT: if camera cards are active, we need to continue and draw them
    if (alerts == nullptr && alertCount == 0) {
        for (int c = 0; c < 2; c++) {
            cards[c].alert = AlertData();
            cards[c].lastSeen = 0;
        }
        lastPriorityForCards = AlertData();
        
        // Only fully return if no camera cards to draw
        if (!hasActiveCameras) {
            // Clear the card area
            const int signalBarsX = SCREEN_WIDTH - 200 - 2;
            const int clearWidth = signalBarsX - startX;
            if (clearWidth > 0) {
                FILL_RECT(startX, cardY, clearWidth, cardH, PALETTE_BG);
            }
            // Reset last drawn count so next time cards appear, change is detected
            lastDrawnCount = 0;
            return;
        }
        // Otherwise, fall through to draw camera cards
    }
    
    // Helper: check if two alerts match (same band + frequency within tolerance)
    // V1 frequency can jitter by a few MHz between frames - use ±5 MHz tolerance
    auto alertsMatch = [](const AlertData& a, const AlertData& b) -> bool {
        if (a.band != b.band) return false;
        if (a.band == BAND_LASER) return true;
        const uint32_t FREQ_TOLERANCE_MHZ = 5;
        uint32_t diff = (a.frequency > b.frequency) ? (a.frequency - b.frequency) : (b.frequency - a.frequency);
        return diff <= FREQ_TOLERANCE_MHZ;
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
    
    // Step 2: Add new non-priority alerts to empty slots
    // Skip priority alert - it's shown in the main display, not as a card
    if (alerts != nullptr) {
        for (int i = 0; i < alertCount; i++) {
            if (!alerts[i].isValid || alerts[i].band == BAND_NONE) continue;
            if (isSameAsPriority(alerts[i])) continue;  // Skip priority - don't waste a card slot
            
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
    
    // Helper: get signal bars for an alert based on direction
    auto getAlertBars = [](const AlertData& a) -> uint8_t {
        if (a.direction & DIR_FRONT) return a.frontStrength;
        if (a.direction & DIR_REAR) return a.rearStrength;
        return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
    };
    
    // Build list of cards to draw this frame (V1 alerts first, then camera if room)
    struct CardToDraw {
        int slot;           // V1 card slot index (-1 for camera)
        bool isGraced;
        uint8_t bars;       // Signal strength for V1 cards
        bool isCamera;      // True if this is a camera alert card
        int cameraIndex;    // Which camera in cameraCards[] array
    } cardsToDraw[2];
    int cardsToDrawCount = 0;
    
    // First: add V1 secondary alerts (they get priority over camera)
    for (int c = 0; c < 2 && cardsToDrawCount < 2; c++) {
        if (cards[c].lastSeen == 0) continue;
        if (isSameAsPriority(cards[c].alert)) continue;
        cardsToDraw[cardsToDrawCount].slot = c;
        cardsToDraw[cardsToDrawCount].bars = getAlertBars(cards[c].alert);
        cardsToDraw[cardsToDrawCount].isCamera = false;
        cardsToDraw[cardsToDrawCount].cameraIndex = -1;
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
    
    // Second: add active camera alerts to fill remaining card slots
    for (int cam = 0; cam < MAX_CAMERA_CARDS && cardsToDrawCount < 2; cam++) {
        if (cameraCards[cam].active && cameraCards[cam].typeName[0] != '\0') {
            cardsToDraw[cardsToDrawCount].slot = -1;  // -1 indicates camera card
            cardsToDraw[cardsToDrawCount].bars = 0;
            cardsToDraw[cardsToDrawCount].isCamera = true;
            cardsToDraw[cardsToDrawCount].cameraIndex = cam;
            cardsToDraw[cardsToDrawCount].isGraced = false;
            cardsToDrawCount++;
        }
    }
    
    // Track camera state for change detection (now tracks multiple cameras)
    static int lastActiveCameraCount = 0;
    
    // === INCREMENTAL UPDATE LOGIC ===
    // Instead of clearing all cards and redrawing, check each position independently
    
    // Capture forceCardRedraw before resetting (need it for redraw checks)
    bool doForceRedraw = forceCardRedraw;
    forceCardRedraw = false;  // Reset the force flag
    
    // Helper to check if position needs full redraw vs just update
    auto positionNeedsFullRedraw = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) {
            // Position now empty but had content - needs clear
            return lastDrawnPositions[pos].band != BAND_NONE || 
                   lastDrawnPositions[pos].isCamera;
        }
        
        auto& last = lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];
        
        // Type changed (camera <-> V1) - full redraw
        if (curr.isCamera != last.isCamera) return true;
        
        if (curr.isCamera) {
            // Camera card - check if camera index or type changed
            int camIdx = curr.cameraIndex;
            if (camIdx != last.cameraIndex) return true;
            if (camIdx >= 0 && camIdx < MAX_CAMERA_CARDS) {
                if (strcmp(cameraCards[camIdx].typeName, last.typeName) != 0) return true;
                if (cameraCards[camIdx].color != last.color) return true;
            }
        } else {
            // V1 card - check if band/freq/direction changed (needs full card redraw)
            // Use frequency tolerance (±5 MHz) to handle V1 jitter
            const uint32_t FREQ_TOLERANCE_MHZ = 5;
            int slot = curr.slot;
            if (cards[slot].alert.band != last.band) return true;
            uint32_t freqDiff = (cards[slot].alert.frequency > last.frequency) 
                ? (cards[slot].alert.frequency - last.frequency) 
                : (last.frequency - cards[slot].alert.frequency);
            if (freqDiff > FREQ_TOLERANCE_MHZ) return true;
            if (cards[slot].alert.direction != last.direction) return true;
            if (curr.isGraced != last.isGraced) return true;
            if (muted != last.wasMuted) return true;
        }
        return false;
    };
    
    // Helper to check if position needs dynamic update (distance or bars only)
    auto positionNeedsDynamicUpdate = [&](int pos) -> bool {
        if (pos >= cardsToDrawCount) return false;
        
        auto& last = lastDrawnPositions[pos];
        auto& curr = cardsToDraw[pos];
        
        if (curr.isCamera) {
            int camIdx = curr.cameraIndex;
            if (camIdx >= 0 && camIdx < MAX_CAMERA_CARDS) {
                int currDistFt = static_cast<int>(cameraCards[camIdx].distance_m * 3.28084f);
                // Update if distance changed by more than 10ft
                if (abs(currDistFt - last.distanceFt) >= 10) return true;
            }
        } else {
            // V1 card - check signal bars
            if (curr.bars != last.bars) return true;
        }
        return false;
    };
    
    const int signalBarsX = SCREEN_WIDTH - 200 - 2;
    
    // Process each card position
    for (int i = 0; i < 2; i++) {
        int cardX = startX + i * (cardW + cardSpacing);
        
        bool needsFullRedraw = positionNeedsFullRedraw(i) || doForceRedraw;
        bool needsDynamicUpdate = !needsFullRedraw && positionNeedsDynamicUpdate(i);
        
        // Clear position if it's now empty
        if (i >= cardsToDrawCount) {
            if (lastDrawnPositions[i].band != BAND_NONE || lastDrawnPositions[i].isCamera) {
                FILL_RECT(cardX, cardY, cardW, cardH, PALETTE_BG);
                lastDrawnPositions[i].band = BAND_NONE;
                lastDrawnPositions[i].isCamera = false;
                lastDrawnPositions[i].cameraIndex = -1;
                lastDrawnPositions[i].distanceFt = -1;
            }
            continue;
        }
        
        if (!needsFullRedraw && !needsDynamicUpdate) {
            continue;  // Skip this position - nothing changed
        }
        
        // Handle camera card
        if (cardsToDraw[i].isCamera) {
            int camIdx = cardsToDraw[i].cameraIndex;
            if (camIdx < 0 || camIdx >= MAX_CAMERA_CARDS) continue;
            
            uint16_t camColor = cameraCards[camIdx].color;
            const char* camTypeName = cameraCards[camIdx].typeName;
            float camDistance = cameraCards[camIdx].distance_m;
            int currDistFt = static_cast<int>(camDistance * 3.28084f);
            
            if (needsFullRedraw) {
                // === FULL CAMERA CARD REDRAW ===
                uint8_t r = ((camColor >> 11) & 0x1F) * 3 / 10;
                uint8_t g = ((camColor >> 5) & 0x3F) * 3 / 10;
                uint8_t b = (camColor & 0x1F) * 3 / 10;
                uint16_t bgCol = (r << 11) | (g << 5) | b;
                
                FILL_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bgCol);
                DRAW_ROUND_RECT(cardX, cardY, cardW, cardH, 5, camColor);
                
                const int contentCenterY = cardY + 18;
                int topRowY = cardY + 11;
                
                // Direction arrow
                int arrowX = cardX + 18;
                int arrowCY = contentCenterY;
                tft->fillTriangle(arrowX, arrowCY - 7, arrowX - 6, arrowCY + 5, arrowX + 6, arrowCY + 5, TFT_WHITE);
                
                // Bottom row: Camera type name
                const int bottomRowY = cardY + 38;
                tft->setTextSize(1);
                tft->setTextColor(camColor);
                int typeLen = strlen(camTypeName);
                int typePixelWidth = typeLen * 6;
                int typeX = cardX + (cardW - typePixelWidth) / 2;
                tft->setCursor(typeX, bottomRowY);
                tft->print(camTypeName);
            }
            
            // Draw distance (always when full redraw, or just update if dynamic)
            if (needsFullRedraw || needsDynamicUpdate) {
                int topRowY = cardY + 11;
                int labelX = cardX + 36;
                
                // Clear just the distance text area
                uint8_t r = ((camColor >> 11) & 0x1F) * 3 / 10;
                uint8_t g = ((camColor >> 5) & 0x3F) * 3 / 10;
                uint8_t b = (camColor & 0x1F) * 3 / 10;
                uint16_t bgCol = (r << 11) | (g << 5) | b;
                FILL_RECT(labelX, topRowY - 2, cardW - 40, 18, bgCol);
                
                // Format and draw distance
                char distStr[16];
                if (camDistance < 1609.34f) {
                    snprintf(distStr, sizeof(distStr), "%dft", currDistFt);
                } else {
                    snprintf(distStr, sizeof(distStr), "%.1fmi", camDistance / 1609.34f);
                }
                
                tft->setTextColor(TFT_WHITE);
                tft->setTextSize(2);
                int distLen = strlen(distStr);
                int distPixelWidth = distLen * 12;
                int maxDistX = cardX + cardW - distPixelWidth - 5;
                int distX = labelX;
                if (distX + distPixelWidth > cardX + cardW - 5) {
                    distX = maxDistX;
                }
                tft->setCursor(distX, topRowY);
                tft->print(distStr);
                
                // Update tracking
                lastDrawnPositions[i].distanceFt = currDistFt;
            }
            
            // Update position tracking
            lastDrawnPositions[i].isCamera = true;
            lastDrawnPositions[i].cameraIndex = camIdx;
            strncpy(lastDrawnPositions[i].typeName, camTypeName, sizeof(lastDrawnPositions[i].typeName) - 1);
            lastDrawnPositions[i].color = camColor;
            lastDrawnPositions[i].band = BAND_NONE;
            
            continue;
        }
        
        // === V1 ALERT CARD ===
        int c = cardsToDraw[i].slot;
        const AlertData& alert = cards[c].alert;
        bool isGraced = cardsToDraw[i].isGraced;
        bool drawMuted = muted || isGraced;
        uint8_t bars = cardsToDraw[i].bars;
        
        // Card background and border colors
        uint16_t bandCol = getBandColor(alert.band);
        uint16_t bgCol, borderCol;
        
        if (isGraced) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else if (drawMuted) {
            bgCol = 0x2104;
            borderCol = PALETTE_MUTED;
        } else {
            uint8_t r = ((bandCol >> 11) & 0x1F) * 3 / 10;
            uint8_t g = ((bandCol >> 5) & 0x3F) * 3 / 10;
            uint8_t b = (bandCol & 0x1F) * 3 / 10;
            bgCol = (r << 11) | (g << 5) | b;
            borderCol = bandCol;
        }
        
        uint16_t contentCol = (isGraced || drawMuted) ? PALETTE_MUTED : TFT_WHITE;
        uint16_t bandLabelCol = (isGraced || drawMuted) ? PALETTE_MUTED : bandCol;
        
        if (needsFullRedraw) {
            // === FULL V1 CARD REDRAW ===
            FILL_ROUND_RECT(cardX, cardY, cardW, cardH, 5, bgCol);
            DRAW_ROUND_RECT(cardX, cardY, cardW, cardH, 5, borderCol);
            
            const int contentCenterY = cardY + 18;
            int topRowY = cardY + 11;
            
            // Direction arrow
            int arrowX = cardX + 18;
            int arrowCY = contentCenterY;
            if (alert.direction & DIR_FRONT) {
                tft->fillTriangle(arrowX, arrowCY - 7, arrowX - 6, arrowCY + 5, arrowX + 6, arrowCY + 5, contentCol);
            } else if (alert.direction & DIR_REAR) {
                tft->fillTriangle(arrowX, arrowCY + 7, arrowX - 6, arrowCY - 5, arrowX + 6, arrowCY - 5, contentCol);
            } else if (alert.direction & DIR_SIDE) {
                FILL_RECT(arrowX - 6, arrowCY - 2, 12, 4, contentCol);
            }
            
            // Band + frequency
            int labelX = cardX + 36;
            tft->setTextColor(bandLabelCol);
            tft->setTextSize(2);
            if (alert.band == BAND_LASER) {
                tft->setCursor(labelX, topRowY);
                tft->print("LASER");
            } else {
                const char* bandStr = bandToString(alert.band);
                tft->setCursor(labelX, topRowY);
                tft->print(bandStr);
                
                tft->setTextColor(contentCol);
                int freqX = labelX + strlen(bandStr) * 12 + 4;
                tft->setCursor(freqX, topRowY);
                if (alert.frequency > 0) {
                    char freqStr[10];
                    snprintf(freqStr, sizeof(freqStr), "%.3f", alert.frequency / 1000.0f);
                    tft->print(freqStr);
                } else {
                    tft->print("---");
                }
            }
            
            // Draw meter background
            const int meterY = cardY + 34;
            const int meterX = cardX + 10;
            const int meterW = cardW - 20;
            const int meterH = 18;
            FILL_RECT(meterX, meterY, meterW, meterH, 0x1082);
        }
        
        // Draw/update signal bars (always after full redraw, or on bars change)
        if (needsFullRedraw || needsDynamicUpdate) {
            const int meterY = cardY + 34;
            const int meterX = cardX + 10;
            const int meterW = cardW - 20;
            const int meterH = 18;
            const int barCount = 6;
            const int barSpacing = 2;
            const int barWidth = (meterW - (barCount - 1) * barSpacing) / barCount;
            
            // Clear meter area for bar update (not full redraw which already did it)
            if (!needsFullRedraw) {
                FILL_RECT(meterX, meterY, meterW, meterH, 0x1082);
            }
            
            uint16_t barColors[6] = {
                settings.colorBar1, settings.colorBar2, settings.colorBar3,
                settings.colorBar4, settings.colorBar5, settings.colorBar6
            };
            
            for (int b = 0; b < barCount; b++) {
                int barX = meterX + b * (barWidth + barSpacing);
                int barH = 10;
                int barY = meterY + (meterH - barH) / 2;
                
                if (b < bars) {
                    uint16_t fillColor = (isGraced || drawMuted) ? PALETTE_MUTED : barColors[b];
                    FILL_RECT(barX, barY, barWidth, barH, fillColor);
                } else {
                    DRAW_RECT(barX, barY, barWidth, barH, dimColor(barColors[b], 30));
                }
            }
        }
        
        // Update position tracking for V1 card
        lastDrawnPositions[i].isCamera = false;
        lastDrawnPositions[i].band = alert.band;
        lastDrawnPositions[i].frequency = alert.frequency;
        lastDrawnPositions[i].direction = alert.direction;
        lastDrawnPositions[i].isGraced = isGraced;
        lastDrawnPositions[i].wasMuted = muted;
        lastDrawnPositions[i].bars = bars;
        lastDrawnPositions[i].cameraIndex = -1;
    }
    
    // Update global tracking
    lastDrawnCount = cardsToDrawCount;
    lastActiveCameraCount = activeCameraCount;
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

void V1Display::drawBandIndicators(uint8_t bandMask, bool muted, uint8_t bandFlashBits) {
    // Cache to avoid redrawing unchanged bands
    static uint8_t lastEffectiveMask = 0xFF;  // Invalid initial value to force first draw
    static bool lastMuted = false;
    static bool cacheValid = false;
    
    // Local blink timer - V1 blinks at ~5Hz, we match that
    // Use same timer as arrows for synchronized blinking
    static unsigned long lastBlinkTime = 0;
    static bool blinkOn = true;
    const unsigned long BLINK_INTERVAL_MS = 100;  // ~5Hz blink rate
    
    unsigned long now = millis();
    if (now - lastBlinkTime >= BLINK_INTERVAL_MS) {
        blinkOn = !blinkOn;
        lastBlinkTime = now;
    }
    
    // Apply blink: if flash bit is set and we're in OFF phase, treat band as inactive
    uint8_t effectiveBandMask = bandMask;
    if (!blinkOn) {
        // Turn off bands that are flashing during the OFF phase
        effectiveBandMask &= ~bandFlashBits;
    }
    
    // Check for forced invalidation (e.g., after screen clear)
    if (s_forceBandRedraw) {
        cacheValid = false;
        s_forceBandRedraw = false;
    }
    
    // Check if anything changed
    if (cacheValid && effectiveBandMask == lastEffectiveMask && muted == lastMuted) {
        return;  // Nothing changed, skip redraw
    }
    
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
        {"Ka", BAND_KA,    s.colorBandKa},
        {"K",  BAND_K,     s.colorBandK},
        {"X",  BAND_X,     s.colorBandX}
    };

    // Use 24pt font for crisp band labels (no scaling)
    TFT_CALL(setFont)(&FreeSansBold24pt7b);
    TFT_CALL(setTextSize)(textSize);
    GFX_setTextDatum(ML_DATUM);
    
    // Clear and redraw each band label individually
    // Font is ~35px tall, labels are at ML_DATUM (middle-left), so text extends above/below Y
    const int labelClearW = 50;  // Width for "Ka" (widest label)
    const int labelClearH = 38;  // Height of 24pt font
    
    // Only redraw bands that changed
    uint8_t changedBands = cacheValid ? (effectiveBandMask ^ lastEffectiveMask) : 0xFF;
    bool mutedChanged = !cacheValid || (muted != lastMuted);
    
    for (int i = 0; i < 4; ++i) {
        // Skip unchanged bands (unless muted state changed, which affects all lit bands)
        bool bandChanged = (changedBands & cells[i].mask) != 0;
        bool wasActive = (lastEffectiveMask & cells[i].mask) != 0;
        bool isActive = (effectiveBandMask & cells[i].mask) != 0;
        
        if (cacheValid && !bandChanged && !(mutedChanged && (wasActive || isActive))) {
            continue;  // This band hasn't changed
        }
        
        int labelY = startY + i * spacing;
        // Clear area around this label (ML_DATUM means Y is vertical center)
        FILL_RECT(x - 5, labelY - labelClearH/2, labelClearW, labelClearH, PALETTE_BG);
        
        uint16_t col = isActive ? (muted ? PALETTE_MUTED_OR_PERSISTED : cells[i].color) : TFT_DARKGREY;
        TFT_CALL(setTextColor)(col, PALETTE_BG);
        GFX_drawString(tft, cells[i].label, x, labelY);
    }
    
    // Update cache
    lastEffectiveMask = effectiveBandMask;
    lastMuted = muted;
    cacheValid = true;
    
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
// Uses Segment7 TTF font (JBV1 style) if available, falls back to software renderer
void V1Display::drawFrequencyClassic(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    
    if (ofrSegment7Initialized) {
        // Use Segment7 TTF font (JBV1 style)
        const int fontSize = 75;
        
#if defined(DISPLAY_WAVESHARE_349)
        const int leftMargin = 135;   // After band indicators (avoid clipping Ka)
        const int rightMargin = 200;  // Before signal bars (at X=440)
#else
        const int leftMargin = 0;
        const int rightMargin = 120;
#endif
        
        // Position frequency centered between mute icon and cards
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 13;
        
        if (band == BAND_LASER) {
            // Draw "LASER" using Segment7 font
            const char* text = "LASER";
            
            // Get actual text dimensions
            FT_BBox bbox = ofrSegment7.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, text);
            int textWidth = bbox.xMax - bbox.xMin;
            
            int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
            int lx = leftMargin + (maxWidth - textWidth) / 2;
            
            // Clear frequency area (start 10px after leftMargin to avoid clipping Ka)
            const int clearLeft = leftMargin + 10;
            FILL_RECT(clearLeft, y - 5, maxWidth - 10, fontSize + 10, PALETTE_BG);
            
            uint16_t laserColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
            
            // Convert RGB565 to RGB888 for OpenFontRender
            uint8_t bgR = (PALETTE_BG >> 11) << 3;
            uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
            uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
            ofrSegment7.setBackgroundColor(bgR, bgG, bgB);
            ofrSegment7.setFontSize(fontSize);
            ofrSegment7.setFontColor((laserColor >> 11) << 3, ((laserColor >> 5) & 0x3F) << 2, (laserColor & 0x1F) << 3);
            
            ofrSegment7.setCursor(lx, y);
            ofrSegment7.printf("%s", text);
            return;
        }

        bool hasFreq = freqMHz > 0;
        char freqStr[16];
        if (hasFreq) {
            float freqGhz = freqMHz / 1000.0f;
            snprintf(freqStr, sizeof(freqStr), "%05.3f", freqGhz);
        } else {
            snprintf(freqStr, sizeof(freqStr), "--.---");
        }

        int charCount = strlen(freqStr);
        int approxWidth = charCount * 37;  // ~37px per char at fontSize 75
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - approxWidth) / 2;
        if (x < leftMargin) x = leftMargin;
        
        // Clear frequency area (start 10px after leftMargin to avoid clipping Ka)
        const int clearLeft = leftMargin + 10;
        FILL_RECT(clearLeft, y - 5, maxWidth - 10, fontSize + 10, PALETTE_BG);
        
        // Determine frequency color
        uint16_t freqColor;
        if (muted) {
            freqColor = PALETTE_MUTED_OR_PERSISTED;
        } else if (!hasFreq) {
            freqColor = PALETTE_GRAY;
        } else if (isPhotoRadar && s.freqUseBandColor) {
            freqColor = s.colorBandPhoto;  // Photo radar gets its own color
        } else if (s.freqUseBandColor && band != BAND_NONE) {
            freqColor = getBandColor(band);
        } else {
            freqColor = s.colorFrequency;
        }
        
        // Convert RGB565 to RGB888 for OpenFontRender
        uint8_t bgR = (PALETTE_BG >> 11) << 3;
        uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        ofrSegment7.setBackgroundColor(bgR, bgG, bgB);
        ofrSegment7.setFontSize(fontSize);
        ofrSegment7.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);
        ofrSegment7.setCursor(x, y);
        ofrSegment7.printf("%s", freqStr);
    } else {
        // Fallback to software 7-segment renderer
#if defined(DISPLAY_WAVESHARE_349)
        const float scale = 2.3f;
#else
        const float scale = 1.7f;
#endif
        SegMetrics m = segMetrics(scale);
        
        const int muteIconBottom = 33;
        int effectiveHeight = getEffectiveScreenHeight();
        int y = muteIconBottom + (effectiveHeight - muteIconBottom - m.digitH) / 2 + 5;
        
        if (band == BAND_LASER) {
            const char* laserStr = "LASER";
            int width = measureSevenSegmentText(laserStr, scale);
#if defined(DISPLAY_WAVESHARE_349)
            const int leftMargin = 120;
            const int rightMargin = 200;
#else
            const int leftMargin = 0;
            const int rightMargin = 120;
#endif
            int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
            int x = leftMargin + (maxWidth - width) / 2;
            if (x < leftMargin) x = leftMargin;
            FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
            draw14SegmentText(laserStr, x, y, scale, muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL, PALETTE_BG);
            return;
        }

        bool hasFreq = freqMHz > 0;
        char freqStr[16];
        if (hasFreq) {
            float freqGhz = freqMHz / 1000.0f;
            snprintf(freqStr, sizeof(freqStr), "%05.3f", freqGhz);
        } else {
            snprintf(freqStr, sizeof(freqStr), "--.---");
        }

        int width = measureSevenSegmentText(freqStr, scale);
#if defined(DISPLAY_WAVESHARE_349)
        const int leftMargin = 120;
        const int rightMargin = 200;
#else
        const int leftMargin = 0;
        const int rightMargin = 120;
#endif
        int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
        int x = leftMargin + (maxWidth - width) / 2;
        if (x < leftMargin) x = leftMargin;
        
        FILL_RECT(x - 2, y, width + 4, m.digitH + 4, PALETTE_BG);
        
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
}

// Modern frequency display - Antialiased with OpenFontRender
void V1Display::drawFrequencyModern(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    
    // Fall back to Classic style if OFR not initialized
    if (!ofrInitialized) {
        drawFrequencyClassic(freqMHz, band, muted, isPhotoRadar);
        return;
    }
    
    // Layout constants
    const int fontSize = 69;  // 15% larger for better visibility
    const int leftMargin = 140;   // After band indicators (Ka extends to ~132px)
    const int rightMargin = 200;  // Before signal bars (at X=440)
    const int effectiveHeight = getEffectiveScreenHeight();
    const int freqY = effectiveHeight - 60;  // Position in middle of primary zone
    const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    const int clearTop = 30;  // Top of frequency area to clear
    const int clearHeight = effectiveHeight - clearTop;  // Full height from top to bottom of zone
    
    // Cache to avoid expensive OFR work when unchanged
    static char lastText[16] = "";
    static uint16_t lastColor = 0;
    static bool cacheValid = false;
    static unsigned long lastDrawMs = 0;
    static int lastDrawX = 0;      // Cache last draw position for minimal clearing
    static int lastDrawWidth = 0;  // Cache last text width
    static constexpr unsigned long FREQ_FORCE_REDRAW_MS = 500;
    
    // Check for forced invalidation (e.g., after screen clear)
    if (s_forceFrequencyRedraw) {
        cacheValid = false;
        s_forceFrequencyRedraw = false;  // Clear flag - we're handling it
    }
    
    // Modern style: show nothing when no frequency (resting/idle state)
    // But we must clear the area if we previously drew something
    if (freqMHz == 0 && band != BAND_LASER) {
        if (cacheValid && lastText[0] != '\0') {
            // Clear only the previously drawn text area
            FILL_RECT(lastDrawX - 5, clearTop, lastDrawWidth + 10, clearHeight, PALETTE_BG);
            lastText[0] = '\0';
            cacheValid = false;
        }
        return;
    }
    
    // Build text and color
    char textBuf[16];
    if (band == BAND_LASER) {
        strcpy(textBuf, "LASER");
    } else if (freqMHz > 0) {
        snprintf(textBuf, sizeof(textBuf), "%.3f", freqMHz / 1000.0f);
    } else {
        snprintf(textBuf, sizeof(textBuf), "--.---");
    }

    uint16_t freqColor;
    if (band == BAND_LASER) {
        freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
    } else if (muted) {
        freqColor = PALETTE_MUTED_OR_PERSISTED;
    } else if (freqMHz == 0) {
        freqColor = PALETTE_GRAY;
    } else if (isPhotoRadar && s.freqUseBandColor) {
        freqColor = s.colorBandPhoto;  // Photo radar gets its own color
    } else if (s.freqUseBandColor && band != BAND_NONE) {
        freqColor = getBandColor(band);
    } else {
        freqColor = s.colorFrequency;
    }

    // Check if anything changed
    unsigned long nowMs = millis();
    bool textChanged = strcmp(lastText, textBuf) != 0;
    bool changed = !cacheValid ||
                   lastColor != freqColor ||
                   textChanged ||
                   (nowMs - lastDrawMs) >= FREQ_FORCE_REDRAW_MS;

    if (!changed) {
        return;
    }

    // Only recalculate bbox if text actually changed (expensive FreeType call)
    // For frequency strings (XX.XXX format), width is consistent
    int textW, x;
    if (textChanged || !cacheValid) {
        ofr.setFontSize(fontSize);
        FT_BBox bbox = ofr.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, textBuf);
        textW = bbox.xMax - bbox.xMin;
        x = leftMargin + (maxWidth - textW) / 2;
    } else {
        // Reuse cached position for color-only changes
        textW = lastDrawWidth;
        x = lastDrawX;
    }

    // Clear only the area we're about to draw (minimizes flash)
    int clearX = (lastDrawWidth > 0) ? min(x, lastDrawX) - 5 : x - 5;
    int clearW = max(textW, lastDrawWidth) + 10;
    FILL_RECT(clearX, clearTop, clearW, clearHeight, PALETTE_BG);

    ofr.setFontSize(fontSize);
    ofr.setBackgroundColor(0, 0, 0);  // Black background
    ofr.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);

    ofr.setCursor(x, freqY);
    ofr.printf("%s", textBuf);

    // Update cache
    strncpy(lastText, textBuf, sizeof(lastText));
    lastText[sizeof(lastText) - 1] = '\0';
    lastColor = freqColor;
    lastDrawX = x;
    lastDrawWidth = textW;
    cacheValid = true;
    lastDrawMs = nowMs;
}

// Hemi frequency display - Retro speedometer style with Hemi Head font
void V1Display::drawFrequencyHemi(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    
    // Fall back to Classic style if Hemi OFR not initialized
    if (!ofrHemiInitialized) {
        drawFrequencyClassic(freqMHz, band, muted, isPhotoRadar);
        return;
    }
    
    // Layout constants
    const int fontSize = 80;  // Larger for retro speedometer impact
    const int leftMargin = 140;   // After band indicators (Ka extends to ~132px)
    const int rightMargin = 200;  // Before signal bars
    const int effectiveHeight = getEffectiveScreenHeight();
    const int freqY = effectiveHeight - 70;  // Centered between mute icon and cards
    const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    const int clearTop = 20;  // Top of frequency area to clear
    const int clearHeight = effectiveHeight - clearTop;  // Full height from top to bottom of zone
    
    // Cache to avoid expensive OFR work when unchanged
    static char lastText[16] = "";
    static uint16_t lastColor = 0;
    static bool cacheValid = false;
    static unsigned long lastDrawMs = 0;
    static int lastDrawX = 0;      // Cache last draw position for minimal clearing
    static int lastDrawWidth = 0;  // Cache last text width
    static constexpr unsigned long FREQ_FORCE_REDRAW_MS = 500;

    // Check for forced invalidation (e.g., after screen clear)
    if (s_forceFrequencyRedraw) {
        cacheValid = false;
        s_forceFrequencyRedraw = false;  // Clear flag - we're handling it
    }

    // Hemi style: show nothing when no frequency (resting/idle state)
    // But we must clear the area if we previously drew something
    if (freqMHz == 0 && band != BAND_LASER) {
        if (cacheValid && lastText[0] != '\0') {
            // Clear only the previously drawn text area
            FILL_RECT(lastDrawX - 5, clearTop, lastDrawWidth + 10, clearHeight, PALETTE_BG);
            lastText[0] = '\0';
            cacheValid = false;
        }
        return;
    }

    // Build text and color
    char textBuf[16];
    if (band == BAND_LASER) {
        strcpy(textBuf, "LASER");
    } else if (freqMHz > 0) {
        snprintf(textBuf, sizeof(textBuf), "%.3f", freqMHz / 1000.0f);
    } else {
        snprintf(textBuf, sizeof(textBuf), "--.---");
    }

    uint16_t freqColor;
    if (band == BAND_LASER) {
        freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
    } else if (muted) {
        freqColor = PALETTE_MUTED_OR_PERSISTED;
    } else if (freqMHz == 0) {
        freqColor = PALETTE_GRAY;
    } else if (isPhotoRadar && s.freqUseBandColor) {
        freqColor = s.colorBandPhoto;  // Photo radar gets its own color
    } else if (s.freqUseBandColor && band != BAND_NONE) {
        freqColor = getBandColor(band);
    } else {
        freqColor = s.colorFrequency;
    }

    // Check if anything changed
    unsigned long nowMs = millis();
    bool textChanged = strcmp(lastText, textBuf) != 0;
    bool changed = !cacheValid ||
                   lastColor != freqColor ||
                   textChanged ||
                   (nowMs - lastDrawMs) >= FREQ_FORCE_REDRAW_MS;

    if (!changed) {
        return;
    }

    // Only recalculate bbox if text actually changed (expensive FreeType call)
    int textW, x;
    if (textChanged || !cacheValid) {
        ofrHemi.setFontSize(fontSize);
        FT_BBox bbox = ofrHemi.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, textBuf);
        textW = bbox.xMax - bbox.xMin;
        x = leftMargin + (maxWidth - textW) / 2;
    } else {
        // Reuse cached position for color-only changes
        textW = lastDrawWidth;
        x = lastDrawX;
    }

    // Clear only the area we're about to draw (minimizes flash)
    int clearX = (lastDrawWidth > 0) ? min(x, lastDrawX) - 5 : x - 5;
    int clearW = max(textW, lastDrawWidth) + 10;
    FILL_RECT(clearX, clearTop, clearW, clearHeight, PALETTE_BG);

    ofrHemi.setFontSize(fontSize);
    ofrHemi.setBackgroundColor(0, 0, 0);  // Black background
    ofrHemi.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);

    ofrHemi.setCursor(x, freqY);
    ofrHemi.printf("%s", textBuf);

    // Update cache
    strncpy(lastText, textBuf, sizeof(lastText));
    lastText[sizeof(lastText) - 1] = '\0';
    lastColor = freqColor;
    lastDrawX = x;
    lastDrawWidth = textW;
    cacheValid = true;
    lastDrawMs = nowMs;
}

// Serpentine frequency display - JB's favorite font
void V1Display::drawFrequencySerpentine(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    
    // Fall back to Classic style if Serpentine OFR not initialized
    if (!ofrSerpentineInitialized) {
        drawFrequencyClassic(freqMHz, band, muted, isPhotoRadar);
        return;
    }
    
    // Layout constants
    const int fontSize = 65;  // Sized to match display area
    const int leftMargin = 140;   // After band indicators (Ka extends to ~132px)
    const int rightMargin = 200;  // Before signal bars
    const int effectiveHeight = getEffectiveScreenHeight();
    const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    // Available vertical space: mute icon bottom (31) to card top (116) = 85px
    // freqY is the baseline - with 65px font, visual center needs baseline around 55-60
    const int freqY = 35;  // Baseline Y position
    const int clearTop = 20;  // Top of frequency area to clear
    const int clearHeight = effectiveHeight - clearTop;  // Full height from top to bottom of zone
    
    // Cache to avoid expensive OFR work when unchanged
    static char lastText[16] = "";
    static uint16_t lastColor = 0;
    static bool cacheValid = false;
    static unsigned long lastDrawMs = 0;
    static int lastDrawX = 0;      // Cache last draw position for minimal clearing
    static int lastDrawWidth = 0;  // Cache last text width
    static constexpr unsigned long FREQ_FORCE_REDRAW_MS = 500;

    // Check for forced invalidation (e.g., after screen clear)
    if (s_forceFrequencyRedraw) {
        cacheValid = false;
        s_forceFrequencyRedraw = false;  // Clear flag - we're handling it
    }

    // Serpentine style: show nothing when no frequency (resting/idle state)
    // But we must clear the area if we previously drew something
    if (freqMHz == 0 && band != BAND_LASER) {
        if (cacheValid && lastText[0] != '\0') {
            // Clear only the previously drawn text area
            FILL_RECT(lastDrawX - 5, clearTop, lastDrawWidth + 10, clearHeight, PALETTE_BG);
            lastText[0] = '\0';
            cacheValid = false;
        }
        return;
    }

    // Build text and color
    char textBuf[16];
    if (band == BAND_LASER) {
        strcpy(textBuf, "LASER");
    } else if (freqMHz > 0) {
        snprintf(textBuf, sizeof(textBuf), "%.3f", freqMHz / 1000.0f);
    } else {
        snprintf(textBuf, sizeof(textBuf), "--.---");
    }

    uint16_t freqColor;
    if (band == BAND_LASER) {
        freqColor = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorBandL;
    } else if (muted) {
        freqColor = PALETTE_MUTED_OR_PERSISTED;
    } else if (freqMHz == 0) {
        freqColor = PALETTE_GRAY;
    } else if (isPhotoRadar && s.freqUseBandColor) {
        freqColor = s.colorBandPhoto;  // Photo radar gets its own color
    } else if (s.freqUseBandColor && band != BAND_NONE) {
        freqColor = getBandColor(band);
    } else {
        freqColor = s.colorFrequency;
    }

    // Check if anything changed
    unsigned long nowMs = millis();
    bool textChanged = strcmp(lastText, textBuf) != 0;
    bool changed = !cacheValid ||
                   lastColor != freqColor ||
                   textChanged ||
                   (nowMs - lastDrawMs) >= FREQ_FORCE_REDRAW_MS;

    if (!changed) {
        return;
    }

    // Only recalculate bbox if text actually changed (expensive FreeType call)
    int textW, x;
    if (textChanged || !cacheValid) {
        ofrSerpentine.setFontSize(fontSize);
        FT_BBox bbox = ofrSerpentine.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, textBuf);
        textW = bbox.xMax - bbox.xMin;
        x = leftMargin + (maxWidth - textW) / 2;
    } else {
        // Reuse cached position for color-only changes
        textW = lastDrawWidth;
        x = lastDrawX;
    }

    // Clear only the area we're about to draw (minimizes flash)
    int clearX = (lastDrawWidth > 0) ? min(x, lastDrawX) - 5 : x - 5;
    int clearW = max(textW, lastDrawWidth) + 10;
    FILL_RECT(clearX, clearTop, clearW, clearHeight, PALETTE_BG);

    ofrSerpentine.setFontSize(fontSize);
    ofrSerpentine.setBackgroundColor(0, 0, 0);  // Black background
    ofrSerpentine.setFontColor((freqColor >> 11) << 3, ((freqColor >> 5) & 0x3F) << 2, (freqColor & 0x1F) << 3);

    ofrSerpentine.setCursor(x, freqY);
    ofrSerpentine.printf("%s", textBuf);

    // Update cache
    strncpy(lastText, textBuf, sizeof(lastText));
    lastText[sizeof(lastText) - 1] = '\0';
    lastColor = freqColor;
    lastDrawX = x;
    lastDrawWidth = textW;
    cacheValid = true;
    lastDrawMs = nowMs;
}

// Draw volume zero warning in the frequency area (flashing red text)
void V1Display::drawVolumeZeroWarning() {
    // Flash at ~2Hz
    static unsigned long lastFlashTime = 0;
    static bool flashOn = true;
    unsigned long now = millis();
    if (now - lastFlashTime >= 250) {
        flashOn = !flashOn;
        lastFlashTime = now;
    }
    
    // Position warning centered in frequency area
#if defined(DISPLAY_WAVESHARE_349)
    const int leftMargin = 120;
    const int rightMargin = 200;
    const int textScale = 6;  // Large for visibility
#else
    const int leftMargin = 0;
    const int rightMargin = 120;
    const int textScale = 4;
#endif
    int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    int centerX = leftMargin + maxWidth / 2;
    int centerY = getEffectiveScreenHeight() / 2 + 10;
    
    // Use default built-in font (NULL) - has all ASCII characters
    // Each char is 6x8 pixels at scale 1, so "VOL 0" = 5 chars * 6 * scale wide
    const char* warningStr = "VOL 0";
    int charW = 6 * textScale;
    int charH = 8 * textScale;
    int textW = 5 * charW;  // 5 characters
    int textX = centerX - textW / 2;
    int textY = centerY - charH / 2;
    
    // Clear the frequency area
    FILL_RECT(leftMargin, textY - 5, maxWidth, charH + 10, PALETTE_BG);
    
    if (flashOn) {
        tft->setFont(NULL);  // Default built-in font
        tft->setTextSize(textScale);
        tft->setTextColor(0xF800, PALETTE_BG);  // Bright red on background
        tft->setCursor(textX, textY);
        tft->print(warningStr);
    }
}

// KITT scanner animation (Knight Rider style scanning LED bar)
// Draws a red "eye" that sweeps back and forth in the frequency area
void V1Display::drawKittScanner() {
    const unsigned long KITT_FRAME_MS = 16;  // ~60fps animation
    unsigned long now = millis();
    
    if (now - lastKittUpdate < KITT_FRAME_MS) {
        return;  // Not time to update yet
    }
    lastKittUpdate = now;
    
    // Scanner layout - centered in the frequency display area
#if defined(DISPLAY_WAVESHARE_349)
    const int leftMargin = 160;   // After band indicators  
    const int rightMargin = 220;  // Before signal bars
#else
    const int leftMargin = 10;
    const int rightMargin = 130;
#endif
    
    const int scannerWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    const int eyeWidth = 50;       // Width of the bright center
    const int tailLength = 400;    // Length of the fading trail (longer = more "always lit" like KITT)
    const int barHeight = 25;      // Height of the scanner bar
    // Center vertically in full screen height (172 pixels), not just primary zone
    const int barY = (SCREEN_HEIGHT - barHeight) / 2;
    
    // Animation speed: faster sweep (~0.5 second full cycle)
    const float speedPerFrame = 0.055f;
    
    // Update position
    kittPosition += speedPerFrame * kittDirection;
    
    // Bounce at edges
    if (kittPosition >= 1.0f) {
        kittPosition = 1.0f;
        kittDirection = -1;
    } else if (kittPosition <= 0.0f) {
        kittPosition = 0.0f;
        kittDirection = 1;
    }
    
    // Calculate eye center position
    int eyeCenter = leftMargin + (int)(kittPosition * (scannerWidth - eyeWidth)) + eyeWidth / 2;
    
    // Clear the scanner area
    FILL_RECT(leftMargin, barY, scannerWidth, barHeight, PALETTE_BG);
    
    // Draw the leading glow (dimmer, in front of the eye - brightest far away, dimmest near eye)
    int leadLength = tailLength;  // Same length as trailing for full coverage
    for (int i = 0; i < leadLength; i += 4) {
        int leadX = eyeCenter + (kittDirection * (eyeWidth/2 + i));
        if (leadX < leftMargin || leadX > leftMargin + scannerWidth - 4) continue;
        
        // Reverse fade: dim near eye, brighter far away (but still subtle overall)
        float fade = (float)i / leadLength;  // 0 near eye, 1 far away
        fade = fade * 0.40f;  // Keep it subtle - max 40% brightness
        
        // Red color with fading intensity
        uint8_t r = (uint8_t)(31 * fade);
        uint16_t color = (r << 11);
        
        FILL_RECT(leadX, barY, 4, barHeight, color);
    }
    
    // Draw the trail (fading red segments behind the eye)
    for (int i = 0; i < tailLength; i += 4) {
        int trailX = eyeCenter - (kittDirection * (eyeWidth/2 + i));
        if (trailX < leftMargin || trailX > leftMargin + scannerWidth - 4) continue;
        
        // Fade from bright to dim based on distance
        float fade = 1.0f - ((float)i / tailLength);
        fade = fade * fade;  // Quadratic falloff for more dramatic effect
        fade = fade * 0.6f;  // Cap max brightness at 60%
        
        // Red color with fading intensity
        uint8_t r = (uint8_t)(31 * fade);  // 5-bit red for RGB565
        uint16_t color = (r << 11);  // Pure red, varying intensity
        
        FILL_RECT(trailX, barY, 4, barHeight, color);
    }
    
    // Draw the bright eye center (gradient effect)
    for (int i = 0; i < eyeWidth; i += 2) {
        int x = eyeCenter - eyeWidth/2 + i;
        if (x < leftMargin || x > leftMargin + scannerWidth - 2) continue;
        
        // Brightest in center, dimmer at edges
        float distFromCenter = fabsf((float)(i - eyeWidth/2) / (eyeWidth/2));
        float brightness = 1.0f - (distFromCenter * 0.5f);
        
        // Bright red/orange at center
        uint8_t r = 31;  // Full red
        uint8_t g = (uint8_t)(20 * brightness * brightness);  // Add some orange tint at center
        uint16_t color = (r << 11) | (g << 5);
        
        FILL_RECT(x, barY, 2, barHeight, color);
    }
}

// Router: calls appropriate frequency draw method based on display style setting
void V1Display::drawFrequency(uint32_t freqMHz, Band band, bool muted, bool isPhotoRadar) {
    const V1Settings& s = settingsManager.get();
    
    // Debug: log which style is being used
    static int lastStyleLogged = -1;
    if (s.displayStyle != lastStyleLogged) {
        Serial.printf("[Display] Style changed: %d (0=Classic, 1=Modern, 2=Hemi, 3=Serpentine), ofrHemiInit=%d, ofrSerpentineInit=%d\n", 
                      s.displayStyle, ofrHemiInitialized, ofrSerpentineInitialized);
        lastStyleLogged = s.displayStyle;
    }
    
    if (s.displayStyle == DISPLAY_STYLE_MODERN) {
        drawFrequencyModern(freqMHz, band, muted, isPhotoRadar);
    } else if (s.displayStyle == DISPLAY_STYLE_HEMI && ofrHemiInitialized) {
        drawFrequencyHemi(freqMHz, band, muted, isPhotoRadar);
    } else if (s.displayStyle == DISPLAY_STYLE_SERPENTINE && ofrSerpentineInitialized) {
        drawFrequencySerpentine(freqMHz, band, muted, isPhotoRadar);
    } else {
        drawFrequencyClassic(freqMHz, band, muted, isPhotoRadar);
    }
}


// Draw large direction arrow (t4s3 style)
// flashBits indicates which arrows should blink (from image1 & ~image2)
void V1Display::drawDirectionArrow(Direction dir, bool muted, uint8_t flashBits) {
    // Cache to avoid redrawing unchanged arrows
    static bool lastShowFront = false;
    static bool lastShowSide = false;
    static bool lastShowRear = false;
    static bool lastMuted = false;
    static bool cacheValid = false;
    
    // Check for forced invalidation (after screen clear)
    if (s_forceArrowRedraw) {
        cacheValid = false;
        s_forceArrowRedraw = false;
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
    if (g_multiAlertMode) {
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
    uint16_t sideCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowSide;
    uint16_t rearCol = muted ? PALETTE_MUTED_OR_PERSISTED : s.colorArrowRear;
    uint16_t offCol = 0x1082;  // Very dark grey for inactive arrows (matches PALETTE_GRAY)

    // Check if anything changed - if so, redraw ALL arrows to avoid clearing overlap issues
    bool anyChanged = !cacheValid || 
                      (showFront != lastShowFront) || 
                      (showSide != lastShowSide) || 
                      (showRear != lastShowRear) ||
                      (muted != lastMuted);
    
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
            const int headW = (int)(28 * scale);
            const int headH = (int)(22 * scale);
            int sideTop = cy - headH - 2;
            int sideHeight = headH * 2 + 4;
            FILL_RECT(clearLeft, sideTop, clearWidth, sideHeight, PALETTE_BG);
        }
        
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

    // Clear entire arrow region once, then redraw all
    const int headH = (int)(22 * scale);
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
    cacheValid = true;
}

// Draw vertical signal bars on right side (t4s3 style)
void V1Display::drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band, bool muted) {
    const int barCount = 6;

    // Use the stronger side so rear-only alerts still light bars
    uint8_t strength = std::max(frontStrength, rearStrength);

    // Clamp strength to valid range (already mapped from 0-8 to 0-6)
    if (strength > 6) strength = 6;

    // Cache to avoid redrawing unchanged bars
    static uint8_t lastStrength = 0xFF;  // Invalid initial to force first draw
    static bool lastMuted = false;
    static bool cacheValid = false;
    
    // Check for forced invalidation (e.g., after screen clear)
    if (s_forceSignalBarsRedraw) {
        cacheValid = false;
        s_forceSignalBarsRedraw = false;
    }
    
    // Check if anything changed
    if (cacheValid && strength == lastStrength && muted == lastMuted) {
        return;  // Nothing changed, skip redraw
    }

    bool hasSignal = (strength > 0);
    
    // Get signal bar colors from settings (one per bar level)
    const V1Settings& s = settingsManager.get();
    uint16_t barColors[6] = {
        s.colorBar1, s.colorBar2, s.colorBar3,
        s.colorBar4, s.colorBar5, s.colorBar6
    };

#if defined(DISPLAY_WAVESHARE_349)
    // Waveshare 640x172 bar dimensions
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
    int startY = 18;  // Fixed position to align with middle arrow
    if (startY < 8) startY = 8; // keep some padding from top icons

    // Only redraw bars that changed state
    for (int i = 0; i < barCount; i++) {
        bool wasLit = cacheValid && (i < lastStrength);
        bool isLit = hasSignal && (i < strength);
        bool wasMutedLit = cacheValid && wasLit && lastMuted;
        bool isMutedLit = isLit && muted;
        
        // Skip if this bar's state hasn't changed
        if (cacheValid && wasLit == isLit && wasMutedLit == isMutedLit) {
            continue;
        }
        
        // Draw from bottom to top
        int visualIndex = barCount - 1 - i;
        int y = startY + visualIndex * (barHeight + barSpacing);
        
        uint16_t fillColor;
        if (!isLit) {
            fillColor = 0x1082; // very dark grey (resting/off)
        } else if (muted) {
            fillColor = PALETTE_MUTED; // muted grey for lit bars
        } else {
            fillColor = barColors[i];  // Use individual bar color
        }
        
        // Draw filled bar
        FILL_ROUND_RECT(startX, y, barWidth, barHeight, 2, fillColor);
    }
    
    // Update cache
    lastStrength = strength;
    lastMuted = muted;
    cacheValid = true;
}

// Status bar at very top of screen showing GPS/CAM/OBD status
void V1Display::drawStatusBar() {
#if defined(DISPLAY_WAVESHARE_349)
    // Change detection: track last state to avoid redundant redraws
    static bool lastGpsHasFix = false;
    static int lastGpsSats = -1;
    static bool lastShowCam = false;
    static bool lastObdConnected = false;
    static bool lastGpsEnabled = false;
    
    const V1Settings& s = settingsManager.get();
    
    // Current state
    bool gpsHasFix = gpsHandler.hasValidFix();
    int gpsSats = gpsHandler.getFix().satellites;  // Get satellites from fix struct
    bool hasCameraDb = storageManager.hasCameraDatabase();
    bool obdConnected = obdHandler.isConnected();
    bool gpsEnabled = gpsHandler.isEnabled();
    
    // For CAM: only show if camera DB exists AND GPS has fix
    bool showCam = hasCameraDb && gpsHasFix;
    
    // Skip redraw if nothing changed (unless forced after screen clear)
    if (!s_forceStatusBarRedraw &&
        gpsHasFix == lastGpsHasFix && gpsSats == lastGpsSats &&
        showCam == lastShowCam && obdConnected == lastObdConnected &&
        gpsEnabled == lastGpsEnabled) {
        return;
    }
    s_forceStatusBarRedraw = false;
    lastGpsHasFix = gpsHasFix;
    lastGpsSats = gpsSats;
    lastShowCam = showCam;
    lastObdConnected = obdConnected;
    lastGpsEnabled = gpsEnabled;
    
    // Status bar positioning
    const int statusY = 1;           // Very top of screen
    const int statusHeight = 10;     // Height for font size 1
    const int leftMargin = 135;      // After band indicators
    const int rightMargin = 200;     // Before signal bars
    const int areaWidth = SCREEN_WIDTH - leftMargin - rightMargin;  // ~305 pixels
    
    // Spacing: GPS on left, CAM center, OBD on right
    const int gpsX = leftMargin + 10;
    const int camX = leftMargin + areaWidth / 2 - 12;  // Center
    const int obdX = leftMargin + areaWidth - 36;
    
    // Use built-in font size 1
    tft->setTextSize(1);
    
    // ---- GPS indicator ----
    // Clear GPS area (wide enough for "GPS XX")
    FILL_RECT(gpsX, statusY, 48, statusHeight, PALETTE_BG);
    
    if (gpsHasFix && gpsSats > 0) {
        // GPS color based on satellite count
        uint16_t gpsColor = (gpsSats >= 4) ? s.colorStatusGps : s.colorStatusGpsWarn;
        tft->setTextColor(gpsColor);
        tft->setCursor(gpsX, statusY + 1);
        tft->printf("GPS %d", gpsSats);
    } else if (gpsHandler.isEnabled()) {
        // Show dimmed "GPS" when enabled but no fix
        tft->setTextColor(PALETTE_MUTED_OR_PERSISTED);
        tft->setCursor(gpsX, statusY + 1);
        tft->print("GPS");
    }
    
    // ---- CAM indicator ----
    // Clear CAM area
    FILL_RECT(camX, statusY, 24, statusHeight, PALETTE_BG);
    
    if (showCam) {
        tft->setTextColor(s.colorStatusCam);
        tft->setCursor(camX, statusY + 1);
        tft->print("CAM");
    }
    
    // ---- OBD indicator ----
    // Clear OBD area
    FILL_RECT(obdX, statusY, 24, statusHeight, PALETTE_BG);
    
    if (obdConnected) {
        tft->setTextColor(s.colorStatusObd);
        tft->setCursor(obdX, statusY + 1);
        tft->print("OBD");
    }
#endif
}

// Camera alert indicator - shows camera type and distance countdown
// Legacy single-camera interface - calls updateCameraAlerts internally
void V1Display::updateCameraAlert(bool active, const char* typeName, float distance_m, bool approaching, uint16_t color, bool v1HasAlerts) {
#if defined(DISPLAY_WAVESHARE_349)
    if (active) {
        CameraAlertInfo cameras[1];
        cameras[0].typeName = typeName;
        cameras[0].distance_m = distance_m;
        cameras[0].color = color;
        updateCameraAlerts(cameras, 1, v1HasAlerts);
    } else {
        updateCameraAlerts(nullptr, 0, v1HasAlerts);
    }
#endif
}

// Multi-camera alert system
void V1Display::updateCameraAlerts(const CameraAlertInfo* cameras, int count, bool v1HasAlerts) {
#if defined(DISPLAY_WAVESHARE_349)
    static bool lastCameraState = false;
    static bool lastWasCard = false;  // Track if primary was showing as card
    static float lastDistance = -1.0f;
    static char lastTypeName[16] = {0};
    static int lastCameraCount = 0;
    static bool lastV1HasAlerts = false;
    
    // Max supported: 1 primary + MAX_CAMERA_CARDS (2) = 3 cameras total
    // When V1 has alerts, max is MAX_CAMERA_CARDS since all become cards
    const int MAX_TOTAL_CAMERAS = MAX_CAMERA_CARDS + 1;
    if (count > MAX_TOTAL_CAMERAS) count = MAX_TOTAL_CAMERAS;
    
    bool active = (count > 0);
    const char* typeName = active ? cameras[0].typeName : "";
    float distance_m = active ? cameras[0].distance_m : 0.0f;
    uint16_t color = active ? cameras[0].color : 0;
    
    // === EARLY EXIT: Check if anything actually changed ===
    // Skip all processing if state is identical to last call
    bool stateChanged = (active != lastCameraState) || 
                        (count != lastCameraCount) ||
                        (v1HasAlerts != lastV1HasAlerts);
    if (!stateChanged && !active) {
        // No cameras now, no cameras before - nothing to do
        return;
    }
    
    // Determine how to display cameras:
    // - If V1 has alerts: All cameras show as cards (V1 gets primary)
    // - If no V1 alerts and 1 camera: Primary camera in main area
    // - If no V1 alerts and 2 cameras: Primary in main area, secondary as card
    bool showPrimaryAsCard = v1HasAlerts;
    
    // === HANDLE CARD STATES ===
    // Set up camera cards for secondary display
    // MAX_CAMERA_CARDS = 2 card slots available
    if (active) {
        if (showPrimaryAsCard) {
            // V1 has alerts - all cameras become cards (max 2)
            int cardCount = (count > MAX_CAMERA_CARDS) ? MAX_CAMERA_CARDS : count;
            for (int i = 0; i < cardCount; i++) {
                setCameraAlertState(i, true, cameras[i].typeName, cameras[i].distance_m, cameras[i].color);
            }
            // Clear any unused slots
            for (int i = cardCount; i < MAX_CAMERA_CARDS; i++) {
                setCameraAlertState(i, false, "", 0, 0);
            }
        } else {
            // No V1 alerts - primary (cameras[0]) in main area
            // Secondary cameras (cameras[1], cameras[2]) get card slots 0, 1
            for (int i = 0; i < MAX_CAMERA_CARDS; i++) {
                int cameraIndex = i + 1;  // Skip cameras[0] which is primary
                if (cameraIndex < count) {
                    setCameraAlertState(i, true, cameras[cameraIndex].typeName, 
                                       cameras[cameraIndex].distance_m, cameras[cameraIndex].color);
                } else {
                    setCameraAlertState(i, false, "", 0, 0);
                }
            }
        }
    } else {
        // No cameras - clear all card states
        clearAllCameraAlerts();
    }
    
    // === DRAW/CLEAR CAMERA CARDS IN SECONDARY AREA ===
    // ONLY force redraw when camera state actually changed
    // This prevents constant full redraws when called every frame with no cameras
    if (stateChanged) {
        forceCardRedraw = true;
        // Call drawSecondaryAlertCards with empty V1 data to render/clear camera cards
        AlertData emptyPriority;  // Default constructor = invalid
        drawSecondaryAlertCards(nullptr, 0, emptyPriority, false);
        
        // Flush to ensure card changes are visible (especially when clearing)
        flush();
    }
    
    // Update tracking for next call
    lastCameraState = active;
    lastCameraCount = count;
    lastV1HasAlerts = v1HasAlerts;;
    
    // === HANDLE MAIN AREA DISPLAY ===
    // If V1 has active alerts, clear main camera area (camera shows as card instead)
    if (active && showPrimaryAsCard) {
        // Clear main frequency area if we were showing there
        if (lastCameraState && !lastWasCard) {
            const int leftMargin = 135;
            const int rightMargin = 200;
            const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
            const int muteIconBottom = 33;
            int effectiveHeight = getEffectiveScreenHeight();
            const int fontSize = 75;
            int freqY = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 13;
            const int clearX = leftMargin + 10;
            const int clearY = freqY - 25;
            const int clearW = maxWidth - 10;
            const int clearH = fontSize + 40;
            FILL_RECT(clearX, clearY, clearW, clearH, PALETTE_BG);
            // Also clear the type label (below status bar at Y=14)
            const int typeLabelY = 14;
            FILL_RECT(leftMargin + 10, typeLabelY - 2, maxWidth - 10, 18, PALETTE_BG);
        }
        lastCameraState = true;
        lastWasCard = true;
        lastDistance = distance_m;
        lastCameraCount = count;
        strncpy(lastTypeName, typeName, sizeof(lastTypeName) - 1);
        lastTypeName[sizeof(lastTypeName) - 1] = '\0';
        return;
    }
    
    // If we were showing as card but V1 no longer has alerts, force main area redraw
    if (active && lastWasCard) {
        lastWasCard = false;
        lastCameraState = false;  // Force redraw
    }
    
    // Camera alert uses same area as frequency display (V1 frequency style)
    const int leftMargin = 135;   // After band indicators (same as V1 frequency)
    const int rightMargin = 200;  // Before signal bars (same as V1 frequency)
    const int maxWidth = SCREEN_WIDTH - leftMargin - rightMargin;
    
    // Vertical positioning - same as V1 frequency display
    const int muteIconBottom = 33;
    int effectiveHeight = getEffectiveScreenHeight();
    const int fontSize = 67;  // ~10% smaller than V1 frequency (75 -> 67)
    int freqY = muteIconBottom + (effectiveHeight - muteIconBottom - fontSize) / 2 + 13;
    
    // Clear area dimensions - sized for 67pt font (don't overlap with cards at Y=118)
    // Cards start at Y = SCREEN_HEIGHT - SECONDARY_ROW_HEIGHT = 172 - 54 = 118
    const int clearX = leftMargin + 10;
    const int clearY = freqY - 5;
    const int clearW = maxWidth - 10;
    const int clearH = fontSize + 10;  // 67 + 10 = 77px (scaled down from 85)
    
    // Type label position (below status bar which ends at Y~11)
    const int typeLabelY = 14;
    
    if (!active) {
        // Clear camera alert area if was previously shown
        if (lastCameraState) {
            if (!lastWasCard) {
                // Clear main distance area
                FILL_RECT(clearX, clearY, clearW, clearH, PALETTE_BG);
                // Also clear the type label (below status bar)
                FILL_RECT(leftMargin + 10, typeLabelY - 2, maxWidth - 10, 18, PALETTE_BG);
            }
            lastCameraState = false;
            lastWasCard = false;
            lastDistance = -1.0f;
            lastCameraCount = 0;
            lastTypeName[0] = '\0';
        }
        return;
    }
    
    // Check if we need to redraw
    bool needsRedraw = !lastCameraState || 
                       (fabs(distance_m - lastDistance) >= 10.0f) ||
                       (strcmp(typeName, lastTypeName) != 0) ||
                       (count != lastCameraCount);
    
    if (!needsRedraw) return;
    
    lastCameraState = true;
    lastWasCard = false;
    lastDistance = distance_m;
    lastCameraCount = count;
    strncpy(lastTypeName, typeName, sizeof(lastTypeName) - 1);
    lastTypeName[sizeof(lastTypeName) - 1] = '\0';
    
    // Clear the display area (frequency area only)
    FILL_RECT(clearX, clearY, clearW, clearH, PALETTE_BG);
    
    // === CAMERA TYPE LABEL (below status bar, above distance) ===
    // Position below status bar (which ends at Y~11)
    // (typeLabelY already declared above at Y=14)
    tft->setTextSize(2);
    tft->setTextColor(color);
    int typeLen = strlen(typeName);
    int typePixelWidth = typeLen * 12;  // size 2 = 12 pixels per char
    int typeX = leftMargin + (maxWidth - typePixelWidth) / 2;
    // Clear just the type label area first (in case type name changed length)
    FILL_RECT(leftMargin + 10, typeLabelY - 2, maxWidth - 10, 18, PALETTE_BG);
    tft->setCursor(typeX, typeLabelY);
    tft->print(typeName);
    
    // === DISTANCE IN 67pt SEGMENT7 FONT (~10% smaller than V1 frequency) ===
    char distStr[16];
    if (distance_m < 1609.34f) {  // Less than 1 mile
        int distFt = static_cast<int>(distance_m * 3.28084f);
        snprintf(distStr, sizeof(distStr), "%d ft", distFt);
    } else {
        snprintf(distStr, sizeof(distStr), "%.1f mi", distance_m / 1609.34f);
    }
    
    // Use OpenFontRender Segment7 at 67pt
    if (ofrSegment7Initialized) {
        ofrSegment7.setFontSize(fontSize);  // 67pt
        uint8_t bgR = (PALETTE_BG >> 11) << 3;
        uint8_t bgG = ((PALETTE_BG >> 5) & 0x3F) << 2;
        uint8_t bgB = (PALETTE_BG & 0x1F) << 3;
        ofrSegment7.setBackgroundColor(bgR, bgG, bgB);
        ofrSegment7.setFontColor((color >> 11) << 3, ((color >> 5) & 0x3F) << 2, (color & 0x1F) << 3);
        
        FT_BBox bbox = ofrSegment7.calculateBoundingBox(0, 0, fontSize, Align::Left, Layout::Horizontal, distStr);
        int distTextWidth = bbox.xMax - bbox.xMin;
        int distX = leftMargin + (maxWidth - distTextWidth) / 2;
        ofrSegment7.setCursor(distX, freqY);  // Same Y position as V1 frequency
        ofrSegment7.printf("%s", distStr);
    } else {
        // Fallback to larger tft font
        tft->setTextSize(4);
        int distLen = strlen(distStr);
        int distX = leftMargin + (maxWidth - distLen * 24) / 2;
        tft->setCursor(distX, freqY);
        tft->print(distStr);
    }
    
    flush();
#endif
}

void V1Display::clearCameraAlert() {
#if defined(DISPLAY_WAVESHARE_349)
    updateCameraAlert(false, "", 0, false, 0, false);
#endif
}

void V1Display::clearCameraAlerts() {
#if defined(DISPLAY_WAVESHARE_349)
    updateCameraAlerts(nullptr, 0, false);
#endif
}

void V1Display::setCameraAlertState(int index, bool active, const char* typeName, float distance_m, uint16_t color) {
    if (index < 0 || index >= MAX_CAMERA_CARDS) return;
    
    cameraCards[index].active = active;
    cameraCards[index].distance_m = distance_m;
    cameraCards[index].color = color;
    if (active && typeName) {
        strncpy(cameraCards[index].typeName, typeName, sizeof(cameraCards[index].typeName) - 1);
        cameraCards[index].typeName[sizeof(cameraCards[index].typeName) - 1] = '\0';
    } else {
        cameraCards[index].typeName[0] = '\0';
    }
    
    // Update active count
    activeCameraCount = 0;
    for (int i = 0; i < MAX_CAMERA_CARDS; i++) {
        if (cameraCards[i].active) activeCameraCount++;
    }
}

void V1Display::clearAllCameraAlerts() {
    for (int i = 0; i < MAX_CAMERA_CARDS; i++) {
        cameraCards[i].active = false;
        cameraCards[i].typeName[0] = '\0';
        cameraCards[i].distance_m = 0.0f;
        cameraCards[i].color = 0;
    }
    activeCameraCount = 0;
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
