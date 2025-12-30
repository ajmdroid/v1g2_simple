/**
 * Display Driver for V1 Gen2 Display
 * Supports multiple hardware platforms
 */

#include "display.h"
#include "serial_logger.h"
#include "../include/config.h"
#include "../include/color_themes.h"
#include "rdf_logo.h"  // RDF splash screen (only logo actually used)
#include "settings.h"
#include "battery_manager.h"
#include "wifi_manager.h"
#include <esp_heap_caps.h>

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
}

V1Display::~V1Display() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    if (tft) delete tft;
    if (gfxPanel) delete gfxPanel;
    if (bus) delete bus;
#endif
}

bool V1Display::begin() {
    SerialLog.println("Display init start...");
    SerialLog.print("Board: ");
    SerialLog.println(DISPLAY_NAME);
    
#if PIN_POWER_ON >= 0
    // Power was held low in setup(); bring it up now
    digitalWrite(PIN_POWER_ON, HIGH);
    SerialLog.println("Power ON");
    delay(200);
#endif
    
    // Initialize display
    SerialLog.println("Calling display init...");

#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Arduino_GFX initialization for Waveshare 3.49"
    SerialLog.println("Initializing Arduino_GFX for Waveshare 3.49...");
    SerialLog.printf("Pins: CS=%d, SCK=%d, D0=%d, D1=%d, D2=%d, D3=%d, RST=%d, BL=%d\n",
                  LCD_CS, LCD_SCLK, LCD_DATA0, LCD_DATA1, LCD_DATA2, LCD_DATA3, LCD_RST, LCD_BL);
    
    // Configure backlight pin
    SerialLog.println("Configuring backlight...");
    // Waveshare 3.49" has INVERTED backlight PWM:
    // 0 = full brightness, 255 = off
    pinMode(LCD_BL, OUTPUT);
    analogWrite(LCD_BL, 255);  // Start with backlight off (inverted: 255=off)
    SerialLog.println("Backlight configured, set to 255 (off, inverted)");
    
    // Manual RST toggle with Waveshare timing BEFORE creating bus
    // This is critical - Waveshare examples do: HIGH(30ms) -> LOW(250ms) -> HIGH(30ms)
    SerialLog.println("Manual RST toggle (Waveshare timing)...");
    pinMode(LCD_RST, OUTPUT);
    digitalWrite(LCD_RST, HIGH);
    delay(30);
    digitalWrite(LCD_RST, LOW);
    delay(250);
    digitalWrite(LCD_RST, HIGH);
    delay(30);
    SerialLog.println("RST toggle complete");
    
    // Create QSPI bus
    SerialLog.println("Creating QSPI bus...");
    bus = new Arduino_ESP32QSPI(
        LCD_CS,    // CS
        LCD_SCLK,  // SCK
        LCD_DATA0, // D0
        LCD_DATA1, // D1
        LCD_DATA2, // D2
        LCD_DATA3  // D3
    );
    if (!bus) {
        SerialLog.println("ERROR: Failed to create bus!");
        return false;
    }
    SerialLog.println("QSPI bus created");
    
    // Create AXS15231B panel - native 172x640 portrait
    // Pass GFX_NOT_DEFINED for RST since we already did manual reset
    SerialLog.println("Creating AXS15231B panel...");
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
    if (!gfxPanel) {
        SerialLog.println("ERROR: Failed to create panel!");
        return false;
    }
    SerialLog.println("AXS15231B panel created with init_operations");
    
    // Create canvas as 172x640 native with rotation=1 for landscape (90°)
    SerialLog.println("Creating canvas 172x640 with rotation=1 (landscape)...");
    tft = new Arduino_Canvas(172, 640, gfxPanel, 0, 0, 1);
    
    if (!tft) {
        SerialLog.println("ERROR: Failed to create canvas!");
        return false;
    }
    SerialLog.println("Canvas created");
    
    SerialLog.println("Calling tft->begin()...");
    if (!tft->begin()) {
        SerialLog.println("ERROR: tft->begin() failed!");
        return false;
    }
    SerialLog.println("tft->begin() succeeded");
    SerialLog.printf("Canvas size: width=%d, height=%d\n", tft->width(), tft->height());
    
    SerialLog.println("Filling screen with black...");
    tft->fillScreen(COLOR_BLACK);
    tft->flush();
    SerialLog.println("Screen filled and flushed");
    
    // Turn on backlight (inverted: 0 = full brightness)
    SerialLog.println("Turning on backlight (inverted PWM)...");
    analogWrite(LCD_BL, 0);  // Full brightness (inverted: 0=on)
    delay(100);
    SerialLog.println("Backlight ON");
    
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

    SerialLog.println("Display initialized successfully!");
    SerialLog.print("Screen: ");
    SerialLog.print(SCREEN_WIDTH);
    SerialLog.print("x");
    SerialLog.println(SCREEN_HEIGHT);
    
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

void V1Display::clear() {
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->fillScreen(PALETTE_BG);
    tft->flush();
#else
    TFT_CALL(fillScreen)(PALETTE_BG);
#endif
}

void V1Display::drawBaseFrame() {
    // Clean black background (t4s3-style)
    TFT_CALL(fillScreen)(PALETTE_BG);
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

void V1Display::drawTopCounter(char symbol, bool muted, bool showDot) {
    const float scale = 2.0f;
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

void V1Display::drawMuteIcon(bool muted) {
    // Draw centered badge above frequency display
#if defined(DISPLAY_WAVESHARE_349)
    const float freqScale = 2.2f; 
#else
    const float freqScale = 1.7f;
#endif
    SegMetrics mFreq = segMetrics(freqScale);

    // Frequency Y position (from drawFrequency)
    int freqY = SCREEN_HEIGHT - mFreq.digitH - 8;
    const int rightMargin = 120;
    int maxWidth = SCREEN_WIDTH - rightMargin;
    
    // Badge dimensions (50% larger than original)
    int w = 108;  // 72 * 1.5
    int h = 30;   // 20 * 1.5
    int x = (maxWidth - w) / 2;
    int y = freqY - h - 12; // Position above frequency with spacing
    
    if (muted) {
        // Draw badge with muted styling
        uint16_t outline = PALETTE_MUTED;
        uint16_t fill = PALETTE_MUTED;
        
        FILL_ROUND_RECT(x, y, w, h, 6, fill);
        DRAW_ROUND_RECT(x, y, w, h, 6, outline);
        
        GFX_setTextDatum(MC_DATUM);
        TFT_CALL(setTextSize)(2);  // 50% larger text
        TFT_CALL(setTextColor)(PALETTE_BG, fill);
        GFX_drawString(tft, "MUTED", x + w / 2, y + h / 2 + 1);
    } else {
        // Clear the badge area when not muted
        FILL_RECT(x, y, w, h, PALETTE_BG);
    }
}

void V1Display::drawProfileIndicator(int slot) {
    currentProfileSlot = slot;
    
    // Get custom slot names and colors from settings
    extern SettingsManager settingsManager;
    const V1Settings& s = settingsManager.get();
    
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
    
    // Calculate x position to center over the '.' in the frequency display
    // Frequency format: "XX.XXX" - dot is after 2 digits
#if defined(DISPLAY_WAVESHARE_349)
    const float freqScale = 2.2f;
#else
    const float freqScale = 1.7f;
#endif
    SegMetrics mFreq = segMetrics(freqScale);
    
    // Calculate where frequency starts (same as drawFrequency)
    int freqWidth = measureSevenSegmentText("35.500", freqScale);
    const int rightMargin = 120;
    int maxWidth = SCREEN_WIDTH - rightMargin;
    int freqX = (maxWidth - freqWidth) / 2;
    if (freqX < 0) freqX = 0;
    
    // Calculate dot center position: after 2 digits + 2 spacings
    // Each digit is digitW wide, with spacing between
    int dotCenterX = freqX + 2 * mFreq.digitW + 2 * mFreq.spacing + mFreq.dot / 2;
    
    // Measure the profile name text width
    GFX_setTextDatum(TL_DATUM);  // Top-left
    TFT_CALL(setTextSize)(2);    // Larger text for readability
    int16_t nameWidth = strlen(name) * 12;  // Approximate: size 2 = ~12px per char
    
    // Center the name over the dot position
    int x = dotCenterX - nameWidth / 2;
    if (x < 120) x = 120;  // Don't overlap with band indicators on left (L/Ka/K/X)
    
    int y = 14;
    
    // Clear area for profile name only - don't overlap counter/band indicators
    // Clear from x position to maxWidth, not from 0
    int clearWidth = nameWidth + 10;
    if (x + clearWidth > maxWidth) clearWidth = maxWidth - x;
    FILL_RECT(x - 5, y - 2, clearWidth + 10, 28, PALETTE_BG);
    
    // Draw the profile name centered over the dot
    TFT_CALL(setTextColor)(color, PALETTE_BG);
    GFX_drawString(tft, name, x, y);
    
    // Draw WiFi indicator (if connected to STA network)
    drawWiFiIndicator();
    
    // Draw battery indicator after profile name (if on battery)
    drawBatteryIndicator();
}

void V1Display::drawBatteryIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern BatteryManager batteryManager;
    
    // Don't draw anything if no battery is present
    if (!batteryManager.hasBattery()) {
        return;
    }
    
    // Battery icon position - bottom left, aligned with frequency display bottom
    const int battX = 12;   // Align with bogey counter left edge
    const int battW = 24;   // Battery body width
    const int battH = 14;   // Battery body height
    const int battY = SCREEN_HEIGHT - battH - 8;  // Bottom aligned with frequency
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
    
    // Draw battery outline
    DRAW_RECT(battX, battY, battW, battH, PALETTE_TEXT);  // Main body
    FILL_RECT(battX + battW, battY + (battH - capH) / 2, capW, capH, PALETTE_TEXT);  // Positive cap
    
    // Draw charge sections
    int sectionW = (battW - 2 * padding - (sections - 1)) / sections;  // Width of each section with 1px gap
    for (int i = 0; i < sections; i++) {
        int sx = battX + padding + i * (sectionW + 1);
        int sy = battY + padding;
        int sh = battH - 2 * padding;
        
        if (i < filledSections) {
            FILL_RECT(sx, sy, sectionW, sh, fillColor);
        }
    }
#endif
}

void V1Display::drawWiFiIndicator() {
#if defined(DISPLAY_WAVESHARE_349)
    extern WiFiManager wifiManager;
    
    // Only show WiFi icon when connected to a STA network (internet/NTP)
    if (!wifiManager.isConnected()) {
        return;
    }
    
    // WiFi icon position - above battery icon, bottom left
    // Battery is at Y = SCREEN_HEIGHT - 14 - 8 = SCREEN_HEIGHT - 22
    // Put WiFi icon above that with some spacing
    const int wifiX = 14;   // Align with battery left edge
    const int wifiSize = 20; // Overall icon size
    const int battY = SCREEN_HEIGHT - 14 - 8;  // Battery Y position
    const int wifiY = battY - wifiSize - 6;    // Above battery with 6px gap
    
    // Get WiFi icon color from settings (default cyan 0x07FF)
    const V1Settings& s = settingsManager.get();
    uint16_t wifiColor = s.colorWiFiIcon;
    
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

void V1Display::showConnecting() {
    drawBaseFrame();
    drawStatusText("Scanning for V1...", PALETTE_TEXT);
    drawWiFiIndicator();  // Show WiFi status while scanning
    drawBatteryIndicator();
}

void V1Display::showConnected() {
    drawBaseFrame();
    drawStatusText("Connected!", PALETTE_K);
    drawWiFiIndicator();
    drawBatteryIndicator();
    delay(1000);
    drawBaseFrame();
}

void V1Display::showDisconnected() {
    drawBaseFrame();
    drawStatusText("Disconnected", PALETTE_KA);
    drawWiFiIndicator();
    drawBatteryIndicator();
}

void V1Display::showResting() {
    SerialLog.println("showResting() called");
    SerialLog.printf("SCREEN_WIDTH=%d, SCREEN_HEIGHT=%d\n", SCREEN_WIDTH, SCREEN_HEIGHT);
    
    // Clear and draw the base frame
    TFT_CALL(fillScreen)(PALETTE_BG);
    drawBaseFrame();
    
    // Draw idle state: dimmed UI elements showing V1 is ready
    // Top counter showing "0" (no bogeys)
    drawTopCounter('0', false, true);
    
    // Band indicators all dimmed (no active bands)
    SerialLog.println("Drawing band indicators...");
    drawBandIndicators(0, false);
    
    // Signal bars all empty
    drawVerticalSignalBars(0, 0, BAND_KA, false);
    
    // Direction arrows all dimmed
    SerialLog.println("Drawing arrows...");
    drawDirectionArrow(DIR_NONE, false);
    
    // Frequency display showing dashes
    drawFrequency(0, false);
    
    // Mute indicator off
    drawMuteIcon(false);
    
    // Profile indicator
    drawProfileIndicator(currentProfileSlot);
    
    // Reset lastState so next update() detects changes from this "resting" state
    lastState = DisplayState();  // All defaults: bands=0, arrows=0, bars=0, hasMode=false, modeChar=0
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    // Flush canvas to display
    tft->flush();
#endif
    
    SerialLog.println("showResting() complete");
}

void V1Display::showScanning() {
    SerialLog.println("showScanning() called");
    
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
    
    // Draw "SCAN" in frequency area using 14-segment font
#if defined(DISPLAY_WAVESHARE_349)
    const float scale = 2.2f;
#else
    const float scale = 1.7f;
#endif
    SegMetrics m = segMetrics(scale);
    int y = SCREEN_HEIGHT - m.digitH - 8;
    
    const char* text = "SCAN";
    // Center the text
    int width = measureSevenSegmentText("00.000", scale); // Use approx width of freq to center similarly
    const int rightMargin = 120;
    int maxWidth = SCREEN_WIDTH - rightMargin;
    int x = (maxWidth - width) / 2;
    if (x < 0) x = 0;
    
    // Clear area before drawing
    FILL_RECT(x - 4, y - 4, width + 8, m.digitH + 8, PALETTE_BG);
    
    // Draw "SCAN" in red (using Ka band color)
    draw14SegmentText(text, x, y, scale, PALETTE_KA, PALETTE_BG);
    
    // Reset lastState
    lastState = DisplayState();
    
#if defined(DISPLAY_USE_ARDUINO_GFX)
    tft->flush();
#endif
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
}

void V1Display::showBootSplash() {
    TFT_CALL(fillScreen)(PALETTE_BG); // Clear screen to prevent artifacts
    drawBaseFrame();

    // Draw the RDF logo centered with slight offsets to match panel framing
    const int logoX = (SCREEN_WIDTH - RDF_LOGO_WIDTH) / 2 - 5;
    const int logoY = (SCREEN_HEIGHT - RDF_LOGO_HEIGHT) / 2;
    const int yOffset = -7;
    
    for (int y = 0; y < RDF_LOGO_HEIGHT; y++) {
        for (int x = 0; x < RDF_LOGO_WIDTH; x++) {
            uint16_t pixel = pgm_read_word(&rdf_logo_rgb565[y * RDF_LOGO_WIDTH + x]);
            // Skip near-black pixels to keep black background transparent
            if (pixel > 0x0841) {
                TFT_CALL(drawPixel)(logoX + x, logoY + y + yOffset, pixel);
            }
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
    SerialLog.println("Backlight ON (post-splash, inverted)");
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

    bool stateChanged =
        firstUpdate ||
        state.activeBands != lastState.activeBands ||
        state.arrows != lastState.arrows ||
        state.signalBars != lastState.signalBars ||
        state.muted != lastState.muted ||
        state.modeChar != lastState.modeChar ||
        state.hasMode != lastState.hasMode;

    if (stateChanged) {
        firstUpdate = false;
        drawBaseFrame();
        char topChar = state.hasMode ? state.modeChar : '0';
        drawTopCounter(topChar, state.muted, true);  // Always show dot
        drawBandIndicators(state.activeBands, state.muted);
        // BLE proxy status indicator
        
        // Check if laser is active from display state
        bool isLaser = (state.activeBands & BAND_LASER) != 0;
        drawFrequency(0, isLaser, state.muted);
        
        // Determine primary band for signal bar coloring
        Band primaryBand = BAND_KA; // default
        if (state.activeBands & BAND_LASER) primaryBand = BAND_LASER;
        else if (state.activeBands & BAND_KA) primaryBand = BAND_KA;
        else if (state.activeBands & BAND_K) primaryBand = BAND_K;
        else if (state.activeBands & BAND_X) primaryBand = BAND_X;
        
        drawVerticalSignalBars(state.signalBars, state.signalBars, primaryBand, state.muted);
        drawDirectionArrow(state.arrows, state.muted);
        drawMuteIcon(state.muted);
        drawProfileIndicator(currentProfileSlot);

#if defined(DISPLAY_WAVESHARE_349)
        tft->flush();  // Push canvas to display
#endif

        lastState = state;
    }
}

void V1Display::update(const AlertData& alert, bool mutedFlag) {
    if (!alert.isValid) {
        return;
    }

    // Always redraw for clean display
    drawBaseFrame();

    uint8_t bandMask = alert.band;
    drawTopCounter('1', mutedFlag, true); // bogey counter shows 1 during alert
    drawFrequency(alert.frequency, alert.band == BAND_LASER, mutedFlag);
    drawBandIndicators(bandMask, mutedFlag);
    drawVerticalSignalBars(alert.frontStrength, alert.rearStrength, alert.band, mutedFlag);
    drawDirectionArrow(alert.direction, mutedFlag);
    drawMuteIcon(mutedFlag);

#if defined(DISPLAY_WAVESHARE_349)
    tft->flush();  // Push canvas to display
#endif

    lastAlert = alert;
    lastState.activeBands = bandMask;
    lastState.arrows = alert.direction;
    lastState.signalBars = std::max(alert.frontStrength, alert.rearStrength);
    lastState.muted = mutedFlag;
}

void V1Display::update(const AlertData& alert) {
    // Preserve legacy call sites by using the last known muted flag
    update(alert, lastState.muted);
}

void V1Display::update(const AlertData& alert, const DisplayState& state, int alertCount) {
    if (!alert.isValid) {
        return;
    }

    // Always redraw for clean display
    drawBaseFrame();

    // Use activeBands from display state (all detected bands), not just priority alert band
    uint8_t bandMask = state.activeBands;
    
    // Show 'L' for laser alerts, otherwise show bogey count (clamp to single digit, use '9' for 9+)
    char countChar;
    if (alert.band == BAND_LASER) {
        countChar = 'L';
    } else {
        countChar = (alertCount > 9) ? '9' : ('0' + alertCount);
    }
    drawTopCounter(countChar, state.muted, true);
    
    // Frequency from priority alert
    drawFrequency(alert.frequency, alert.band == BAND_LASER, state.muted);
    
    // Use bands from display state for the indicators
    drawBandIndicators(bandMask, state.muted);
    
    // Signal bars from priority alert
    drawVerticalSignalBars(alert.frontStrength, alert.rearStrength, alert.band, state.muted);
    
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
    // Vertical L/Ka/K/X stack tucked beside the left digit (match reference photo)
#if defined(DISPLAY_WAVESHARE_349)
    const int x = 82;
    const int textSize = 4;  // Bigger text for larger screen
    const int spacing = 40;  // More spacing to fill vertical space
    const int startY = 20;   // Move down to avoid clipping L at top
#else
    const int x = 82;
    const int textSize = 3;  // Chunky, readable
    const int spacing = 32;  // Tighten spacing to sit near the top digit
    const int startY = 12;   // Slightly lower to avoid clipping
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

    GFX_setTextDatum(ML_DATUM);
    TFT_CALL(setTextSize)(textSize);
    for (int i = 0; i < 4; ++i) {
        bool active = (bandMask & cells[i].mask) != 0;
        uint16_t col = active ? (muted ? PALETTE_MUTED : cells[i].color) : TFT_DARKGREY;
        TFT_CALL(setTextColor)(col, PALETTE_BG);
        GFX_drawString(tft, cells[i].label, x, startY + i * spacing);
    }
}

void V1Display::drawSignalBars(uint8_t bars) {
    if (bars > MAX_SIGNAL_BARS) {
        bars = MAX_SIGNAL_BARS;
    }
    
    int startX = (SCREEN_WIDTH - (MAX_SIGNAL_BARS * (BAR_WIDTH + BAR_SPACING))) / 2;
    
    for (uint8_t i = 0; i < MAX_SIGNAL_BARS; i++) {
        int x = startX + i * (BAR_WIDTH + BAR_SPACING);
        int height = BAR_HEIGHT * (i + 1) / MAX_SIGNAL_BARS;
        int y = BARS_Y + (BAR_HEIGHT - height);
        
        if (i < bars) {
            // Draw filled bar
            FILL_RECT(x, y, BAR_WIDTH, height, PALETTE_SIGNAL_BAR);
        } else {
            // Draw empty bar outline
            DRAW_RECT(x, y, BAR_WIDTH, height, TFT_DARKGREY);
            FILL_RECT(x + 1, y + 1, BAR_WIDTH - 2, height - 2, PALETTE_BG);
        }
    }
}

void V1Display::drawFrequency(uint32_t freqMHz, bool isLaser, bool muted) {
#if defined(DISPLAY_WAVESHARE_349)
    const float scale = 2.2f; // Larger for wider screen
#else
    const float scale = 1.7f; // ~15% smaller than the counter digits
#endif
    SegMetrics m = segMetrics(scale);
    
    // Position frequency at bottom with proper margin
    int y = SCREEN_HEIGHT - m.digitH - 8;  // More margin from bottom
    
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


// Draw large direction arrow (t4s3 style)
void V1Display::drawDirectionArrow(Direction dir, bool muted) {
    // Stylized stacked arrows sized/positioned to match the real V1 display
    int cx = SCREEN_WIDTH - 70;           // right anchor
    int cy = SCREEN_HEIGHT / 2;           // vertically centered

#if defined(DISPLAY_WAVESHARE_349)
    // Position arrows to fit ABOVE frequency display at bottom
    // Frequency starts around y=126, so arrows must end before that
    // Keep overall position, center middle arrow between top and bottom
    cy = 95;
    cx -= 6;
#endif
    
    // Top arrow: proportions from reference SVG (130x100, ratio 1.3:1)
    // SVG notch: 62px wide (47.7%), 10px tall (10%)
    const int topW = 100;
    const int topH = 70;      // Reduced height for less steep angle
    const int topNotchW = 48;  // 47.7% of width
    const int topNotchH = 8;   // 10% of height

    // Bottom arrow: proportions from reference SVG (130x60, ratio 2.17:1)
    // SVG notch: 62px wide (47.7%), 9px tall (15%)
    const int bottomW = 100;
    const int bottomH = 36;   // Reduced height
    const int bottomNotchW = 48;  // 47.7% of width
    const int bottomNotchH = 7;   // 15% of height

    // Calculate positions for equal gaps between arrows
    // Side arrow bar is 20px tall, centered at cy
    const int sideBarH = 20;
    const int gap = 8;  // gap between arrows
    
    // Top arrow center: above side arrow with gap
    int topArrowCenterY = cy - sideBarH/2 - gap - topH/2;
    // Bottom arrow center: below side arrow with gap  
    int bottomArrowCenterY = cy + sideBarH/2 + gap + bottomH/2;

    const V1Settings& s = settingsManager.get();
    uint16_t onCol = muted ? PALETTE_MUTED : s.colorArrow;
    uint16_t offCol = 0x1082;  // Very dark grey for inactive arrows (matches PALETTE_GRAY)

    // Clear the entire arrow region using the max dimensions
    const int maxW = (topW > bottomW) ? topW : bottomW;
    const int maxH = (topH > bottomH) ? topH : bottomH;
    int clearTop = topArrowCenterY - topH/2 - 15;
    int clearBottom = bottomArrowCenterY + bottomH/2 + 15;
    FILL_RECT(cx - maxW/2 - 10, clearTop, maxW + 24, clearBottom - clearTop, PALETTE_BG);

    auto drawTriangleArrow = [&](int centerY, bool down, bool active, int triW, int triH, int notchW, int notchH) {
        uint16_t fillCol = active ? onCol : offCol;
        // Triangle points
        int ax = cx;
        int ay = centerY + (down ? triH / 2 : -triH / 2);
        int bx = cx - triW / 2;
        int by = centerY + (down ? -triH / 2 : triH / 2);
        int cxp = cx + triW / 2;
        int cyp = by;

        // Always fill - active gets bright color, inactive gets dark grey fill
        FILL_TRIANGLE(ax, ay, bx, by, cxp, cyp, fillCol);

        // Base notch to mirror the printed legend - always filled
        int notchY = down ? (centerY - triH / 2 - notchH / 2) : (centerY + triH / 2 - notchH / 2);
        FILL_ROUND_RECT(cx - notchW / 2, notchY, notchW, notchH, 3, fillCol);
    };

    auto drawSideArrow = [&](bool active) {
        uint16_t fillCol = active ? onCol : offCol;
        const int barW = maxW - 26;
        const int barH = sideBarH;
        const int headW = 20;
        const int headH = 14;
        const int halfH = barH / 2;

        // Always fill - active gets bright color, inactive gets dark grey fill
        FILL_ROUND_RECT(cx - barW / 2, cy - halfH, barW, barH, 4, fillCol);
        FILL_TRIANGLE(cx - barW / 2 - headW, cy, cx - barW / 2, cy - headH, cx - barW / 2, cy + headH, fillCol);
        FILL_TRIANGLE(cx + barW / 2 + headW, cy, cx + barW / 2, cy - headH, cx + barW / 2, cy + headH, fillCol);
    };

    // Up, side, down arrows - using calculated center positions
    drawTriangleArrow(topArrowCenterY, false, dir & DIR_FRONT, topW, topH, topNotchW, topNotchH);
    drawSideArrow(dir & DIR_SIDE);
    drawTriangleArrow(bottomArrowCenterY, true, dir & DIR_REAR, bottomW, bottomH, bottomNotchW, bottomNotchH);
}

// Draw vertical signal bars on right side (t4s3 style)
void V1Display::drawVerticalSignalBars(uint8_t frontStrength, uint8_t rearStrength, Band band, bool muted) {
    const int barCount = 6;

    // Use the stronger side so rear-only alerts still light bars
    uint8_t strength = std::max(frontStrength, rearStrength);

    // Clamp strength to valid range (V1 Gen2 uses 0-6)
    if (strength > 6) strength = 6;

    bool hasSignal = (strength > 0);

#if defined(DISPLAY_WAVESHARE_349)
    // Scale from Lilygo 320x170 to Waveshare 640x172
    // Width is 2x, height is similar - make bars wider but keep height similar
    const int barWidth = 52;   // 26 * 2 scaled for wider screen
    const int barHeight = 12;  // Similar to original
    const int barSpacing = 12; // More spacing to fill vertical space
#else
    const int barWidth = 26;
    const int barHeight = 10;
    const int barSpacing = 6;
#endif
    const int totalH = barCount * (barHeight + barSpacing) - barSpacing;

    // Place bars to the right of the band stack and vertically centered
#if defined(DISPLAY_WAVESHARE_349)
    int startX = SCREEN_WIDTH - 210;  // Further from arrows on wider screen
#else
    int startX = SCREEN_WIDTH - 90;   // Relative position for narrower screen
#endif
    int startY = (SCREEN_HEIGHT - totalH) / 2;

    // Clear area once
    FILL_RECT(startX - 2, startY - 2, barWidth + 4, totalH + 4, PALETTE_BG);

    for (int i = 0; i < barCount; i++) {
        // Draw from bottom to top
        int visualIndex = barCount - 1 - i;
        int y = startY + visualIndex * (barHeight + barSpacing);
        
        bool lit = hasSignal && (i < strength);
        
        // Simple color: green for bottom 2, blue for middle 2, red for top 2
        // Note: Waveshare display uses BGR byte order, so swap red/blue
        uint16_t fillColor;
        if (!lit) {
            fillColor = 0x1082; // very dark grey (resting/off)
        } else if (muted) {
            fillColor = PALETTE_MUTED; // muted grey for lit bars
        } else if (i < 2) {
            fillColor = 0x07E0; // green (same in RGB and BGR)
        } else if (i < 4) {
            fillColor = 0xF800; // blue (0x001F RGB -> 0xF800 BGR)
        } else {
            fillColor = 0x001F; // red (0xF800 RGB -> 0x001F BGR)
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
        case DIR_FRONT: return s.colorArrow;
        case DIR_SIDE: return s.colorArrow;
        case DIR_REAR: return s.colorArrow;
        default: return TFT_DARKGREY;
    }
}
void V1Display::updateColorTheme() {
    // Load the current theme from settings and update palette
    ColorTheme theme = settingsManager.get().colorTheme;
    currentPalette = ColorThemes::getPalette(theme);
    SerialLog.printf("Color theme updated: %s\n", ColorThemes::getThemeName(theme));
}
