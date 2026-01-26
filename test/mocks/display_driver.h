/**
 * Mock display_driver.h for native unit testing
 * Provides minimal stubs for Arduino_GFX display primitives
 */
#pragma once

#include "Arduino.h"
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

// Color definitions (16-bit RGB565)
#define TFT_BLACK       0x0000
#define TFT_WHITE       0xFFFF
#define TFT_RED         0xF800
#define TFT_GREEN       0x07E0
#define TFT_BLUE        0x001F
#define TFT_YELLOW      0xFFE0
#define TFT_CYAN        0x07FF
#define TFT_MAGENTA     0xF81F
#define TFT_ORANGE      0xFD20
#define TFT_GREY        0x8410
#define TFT_LIGHTGREY   0xC618
#define TFT_DARKGREY    0x4208
#define TFT_GOLD        0xFEA0
#define TFT_SILVER      0xC618
#define TFT_PINK        0xFC18
#define TFT_PURPLE      0x8010
#define TFT_BROWN       0x8200

// Screen dimensions (Waveshare 3.49" rotated)
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif

// Mock data bus
class Arduino_DataBus {
public:
    virtual ~Arduino_DataBus() = default;
};

class Arduino_ESP32QSPI : public Arduino_DataBus {
public:
    Arduino_ESP32QSPI(int cs, int sck, int d0, int d1, int d2, int d3) {}
};

// Mock GFX base class
class Arduino_GFX {
public:
    virtual ~Arduino_GFX() = default;
    virtual void begin(int speed = 0) {}
    virtual void fillScreen(uint16_t color) {}
    virtual void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {}
    virtual void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {}
    virtual void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {}
    virtual void drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {}
    virtual void fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {}
    virtual void drawTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {}
    virtual void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint16_t color) {}
    virtual void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {}
    virtual void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color) {}
    virtual void setTextColor(uint16_t color) {}
    virtual void setTextColor(uint16_t fg, uint16_t bg) {}
    virtual void setTextSize(uint8_t size) {}
    virtual void setCursor(int16_t x, int16_t y) {}
    virtual void print(const char* s) {}
    virtual void print(int n) {}
    virtual void print(float n, int decimals = 2) {}
    virtual void println(const char* s = "") {}
    virtual int16_t width() { return SCREEN_WIDTH; }
    virtual int16_t height() { return SCREEN_HEIGHT; }
    virtual void flush() {}
};

// Mock AXS15231B display panel
class Arduino_AXS15231B : public Arduino_GFX {
public:
    Arduino_AXS15231B(Arduino_DataBus* bus, int rst, int rotation, bool ips, int w, int h) {}
};

// Mock canvas for double-buffering
class Arduino_Canvas : public Arduino_GFX {
public:
    Arduino_Canvas(int16_t w, int16_t h, Arduino_GFX* output, int16_t output_x = 0, int16_t output_y = 0)
        : w_(w), h_(h), output_(output), flushCount_(0), fillScreenCount_(0) {}
    
    void begin(int speed = 0) override {}
    
    void fillScreen(uint16_t color) override {
        lastFillColor_ = color;
        fillScreenCount_++;
    }
    
    void flush() override {
        flushCount_++;
    }
    
    // Test helpers
    int getFlushCount() const { return flushCount_; }
    int getFillScreenCount() const { return fillScreenCount_; }
    uint16_t getLastFillColor() const { return lastFillColor_; }
    void resetCounters() { flushCount_ = 0; fillScreenCount_ = 0; }
    
private:
    int16_t w_, h_;
    Arduino_GFX* output_;
    int flushCount_;
    int fillScreenCount_;
    uint16_t lastFillColor_ = 0;
};

// OpenFontRender stub (for TTF fonts)
class OpenFontRender {
public:
    enum Align { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };
    
    void loadFont(const uint8_t* fontData, size_t fontDataSize) {}
    void setDrawer(Arduino_GFX* gfx) {}
    void setFontColor(uint16_t color) {}
    void setFontColor(uint16_t fg, uint16_t bg) {}
    void setFontSize(float size) {}
    void setAlignment(Align align) {}
    void setCursor(int16_t x, int16_t y) {}
    void printf(const char* fmt, ...) {}
    int16_t getTextWidth(const char* text) { return strlen(text) * 10; }
    int16_t getTextHeight(const char* text) { return 20; }
};
