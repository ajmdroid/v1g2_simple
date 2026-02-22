// Minimal Arduino_GFX stubs for native unit testing when real display headers
// include <Arduino_GFX_Library.h>.
#pragma once

#include <cstdint>
#include <cstddef>

class Arduino_DataBus {
public:
    virtual ~Arduino_DataBus() = default;
};

class Arduino_ESP32QSPI : public Arduino_DataBus {
public:
    Arduino_ESP32QSPI(int, int, int, int, int, int) {}
};

class Arduino_GFX {
public:
    virtual ~Arduino_GFX() = default;
    virtual void begin(int = 0) {}
    virtual int16_t width() { return 640; }
    virtual int16_t height() { return 172; }
    virtual void fillScreen(uint16_t) {}
    virtual void flush() {}
    virtual void setCursor(int16_t, int16_t) {}
    virtual void print(const char*) {}
    virtual void getTextBounds(const char*,
                               int16_t,
                               int16_t,
                               int16_t* x1,
                               int16_t* y1,
                               uint16_t* w,
                               uint16_t* h) {
        if (x1) *x1 = 0;
        if (y1) *y1 = 0;
        if (w) *w = 0;
        if (h) *h = 0;
    }
};

class Arduino_AXS15231B : public Arduino_GFX {
public:
    Arduino_AXS15231B(Arduino_DataBus*, int, int, bool, int, int) {}
};

class Arduino_Canvas : public Arduino_GFX {
public:
    Arduino_Canvas(int16_t, int16_t, Arduino_GFX*, int16_t = 0, int16_t = 0) {}
};
