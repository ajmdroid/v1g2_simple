/**
 * Settings slider methods — extracted from display.cpp (Phase 3B)
 *
 * Contains showSettingsSliders, updateSettingsSliders,
 * getActiveSliderFromTouch, hideBrightnessSlider.
 */

#include "display.h"
#include "../include/display_layout.h"
#include "../include/display_palette.h"
#include "../include/display_flush.h"
#include "../include/display_slider_math.h"

// Combined settings screen with brightness and voice volume sliders
void V1Display::showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel) {
    // Clear screen to dark background
    tft_->fillScreen(0x0000);

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
    tft_->setTextColor(0xFFFF);  // White
    tft_->setTextSize(2);
    tft_->setCursor((SCREEN_WIDTH - 120) / 2, 5);
    tft_->print("SETTINGS");

    // ============================================================================
    // Brightness slider
    // ============================================================================
    tft_->setTextSize(1);
    tft_->setTextColor(0xFFFF);
    tft_->setCursor(sliderMargin, brightnessY - 16);
    tft_->print("BRIGHTNESS");

    // Draw slider track
    tft_->drawRect(sliderX - 2, brightnessY - 2, sliderWidth + 4, sliderHeight + 4, 0x4208);
    tft_->fillRect(sliderX, brightnessY, sliderWidth, sliderHeight, 0x2104);

    // Fill based on brightness level (80-255 range)
    int brightnessFill = computeBrightnessSliderFill(brightnessLevel, sliderWidth);
    tft_->fillRect(sliderX, brightnessY, brightnessFill, sliderHeight, 0x07E0);  // Green

    // Thumb
    int brightThumbX = sliderX + brightnessFill - 4;
    if (brightThumbX < sliderX) brightThumbX = sliderX;
    if (brightThumbX > sliderX + sliderWidth - 8) brightThumbX = sliderX + sliderWidth - 8;
    tft_->fillRect(brightThumbX, brightnessY - 4, 8, sliderHeight + 8, 0xFFFF);

    // Percentage text
    char brightStr[8];
    int brightPercent = computeBrightnessSliderPercent(brightnessLevel);
    snprintf(brightStr, sizeof(brightStr), "%d%%", brightPercent);
    tft_->setCursor(sliderX + sliderWidth + 8, brightnessY);
    tft_->print(brightStr);

    // ============================================================================
    // Voice volume slider
    // ============================================================================
    tft_->setTextColor(0xFFFF);
    tft_->setCursor(sliderMargin, volumeY - 16);
    tft_->print("VOICE VOLUME");

    // Draw slider track
    tft_->drawRect(sliderX - 2, volumeY - 2, sliderWidth + 4, sliderHeight + 4, 0x4208);
    tft_->fillRect(sliderX, volumeY, sliderWidth, sliderHeight, 0x2104);

    // Fill based on volume level (0-100 range)
    int volumeFill = (volumeLevel * sliderWidth) / 100;
    tft_->fillRect(sliderX, volumeY, volumeFill, sliderHeight, 0x001F);  // Blue for volume

    // Thumb
    int volThumbX = sliderX + volumeFill - 4;
    if (volThumbX < sliderX) volThumbX = sliderX;
    if (volThumbX > sliderX + sliderWidth - 8) volThumbX = sliderX + sliderWidth - 8;
    tft_->fillRect(volThumbX, volumeY - 4, 8, sliderHeight + 8, 0xFFFF);

    // Percentage text
    char volStr[8];
    snprintf(volStr, sizeof(volStr), "%d%%", volumeLevel);
    tft_->setCursor(sliderX + sliderWidth + 8, volumeY);
    tft_->print(volStr);

    // Instructions at bottom
    tft_->setTextSize(1);
    tft_->setTextColor(0x8410);  // Gray
    tft_->setCursor((SCREEN_WIDTH - 220) / 2, 155);
    tft_->print("Touch sliders - BOOT to save");

    DISPLAY_FLUSH();
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
