// Touch UI module - handles BOOT button brightness/volume adjustment UI and WiFi toggle

#pragma once

#include <Arduino.h>
#include <functional>

#include "display.h"
#include "settings.h"
#include "touch_handler.h"

class TouchUiModule {
public:
    TouchUiModule() = default;

    struct Callbacks {
        std::function<bool()> isWifiSetupActive;
        std::function<void()> stopWifiSetup;   // stop AP/setup mode
        std::function<void()> startWifi;       // start WiFi/AP
        std::function<void()> drawWifiIndicator;
        std::function<void()> restoreDisplay;  // refresh display with current state
    };

    void begin(V1Display* disp,
               TouchHandler* touch,
               SettingsManager* settingsMgr,
               const Callbacks& cbs);

    // Returns true if UI consumed the loop (brightness/volume adjustment active)
    bool process(unsigned long nowMs, bool bootPressed);

    bool isAdjustMode() const { return brightnessAdjustMode; }

private:
    void enterAdjustMode();
    void exitAdjustModeAndSave();
    bool handleSliderTouch(unsigned long nowMs);

    // State
    V1Display* display = nullptr;
    TouchHandler* touchHandler = nullptr;
    SettingsManager* settings = nullptr;
    Callbacks callbacks{};

    bool brightnessAdjustMode = false;
    uint8_t brightnessAdjustValue = 200;
    uint8_t volumeAdjustValue = 75;
    int activeSlider = 0;
    unsigned long lastVolumeChangeMs = 0;

    unsigned long bootPressStart = 0;
    bool bootWasPressed = false;
    bool wifiToggleTriggered = false;

    // Timing constants (mirrors previous inline logic)
    static constexpr unsigned long BOOT_DEBOUNCE_MS = 300;
    static constexpr unsigned long AP_TOGGLE_LONG_PRESS_MS = 4000;
    static constexpr unsigned long VOLUME_TEST_DEBOUNCE_MS = 1000;
};
