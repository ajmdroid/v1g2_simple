#ifndef V1_LOGO_H
#define V1_LOGO_H

#include <Arduino.h>

// Simple V1 Gen2 logo - 80x60 pixels in RGB565 format
// This is a simple design with "V1" and "Gen2" text
const uint16_t LOGO_WIDTH = 80;
const uint16_t LOGO_HEIGHT = 60;

// RGB565 colors
const uint16_t LOGO_BG = 0x0000;      // Black
const uint16_t LOGO_RED = 0xF800;     // Red (Valentine1 signature color)
const uint16_t LOGO_WHITE = 0xFFFF;   // White

// Simple V1 Gen2 logo bitmap (80x60 = 4800 pixels)
// This will be drawn procedurally for simplicity
// Format: RGB565 (16-bit color)

#endif // V1_LOGO_H
