#pragma once
// ============================================================================
// OpenFontRender stub for native unit tests.
// Provides the minimal interface needed so files that include OpenFontRender.h
// compile without the real ESP32 OpenFontRender library.
// ============================================================================

#ifndef OPENFONTRENDERH_H
#define OPENFONTRENDERH_H

#include <cstdint>
#include <cstddef>
#include <cstring>

// FreeType error code
typedef int FT_Error;

// Forward-declare Arduino_GFX so setDrawer can accept a reference
class Arduino_GFX;

// Minimal FreeType bounding box stub
struct FT_BBox {
    long xMin = 0;
    long yMin = 0;
    long xMax = 0;
    long yMax = 0;
};

// Alignment / Layout enums used by DisplayFontManager
enum class Align { Left = 0, Center = 1, Right = 2 };
enum class Layout { Horizontal = 0, Vertical = 1 };

class OpenFontRender {
public:
    enum AlignEnum { ALIGN_LEFT = 0, ALIGN_CENTER = 1, ALIGN_RIGHT = 2 };

    FT_Error loadFont(const uint8_t* /*fontData*/, size_t /*fontDataSize*/) { return 0; }
    void setDrawer(Arduino_GFX& /*gfx*/) {}
    void setCacheSize(int /*glyphN*/, int /*cacheN*/, size_t /*bytes*/) {}
    void setFontColor(uint16_t /*color*/) {}
    void setFontColor(uint16_t /*fg*/, uint16_t /*bg*/) {}
    void setFontColor(uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) {}
    void setBackgroundColor(uint8_t /*r*/, uint8_t /*g*/, uint8_t /*b*/) {}
    void setFontSize(float /*size*/) {}
    void setAlignment(AlignEnum /*align*/) {}
    void setCursor(int16_t /*x*/, int16_t /*y*/) {}
    void printf(const char* /*fmt*/, ...) {}
    int16_t getTextWidth(const char* text) { return static_cast<int16_t>(strlen(text) * 10); }
    int16_t getTextHeight(const char* /*text*/) { return 20; }
    FT_BBox calculateBoundingBox(int /*x*/, int /*y*/, int fontSize,
                                  Align /*align*/, Layout /*layout*/,
                                  const char* text) {
        FT_BBox box;
        box.xMin = 0;
        box.xMax = static_cast<long>(strlen(text) * fontSize / 2);
        box.yMin = 0;
        box.yMax = fontSize;
        return box;
    }
};

#endif  // OPENFONTRENDERH_H
