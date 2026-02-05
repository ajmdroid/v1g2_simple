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
        std::function<bool()> deleteDebugLogs; // delete debug logs, returns true on success
    };

    void begin(V1Display* disp,
               TouchHandler* touch,
               SettingsManager* settingsMgr,
               const Callbacks& cbs);

    // Returns true if UI consumed the loop (brightness/volume adjustment active)
    bool process(unsigned long nowMs, bool bootPressed);

    bool isAdjustMode() const { return brightnessAdjustMode; }
    bool isDeleteLogsMode() const { return deleteLogsMode; }

private:
    void enterAdjustMode();
    void exitAdjustModeAndSave();
    bool handleSliderTouch(unsigned long nowMs);
    
    void enterDeleteLogsMode();
    void exitDeleteLogsMode(bool performDelete);
    bool handleDeleteLogsTouch(unsigned long nowMs);

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
    
    // Delete logs mode
    bool deleteLogsMode = false;

    unsigned long bootPressStart = 0;
    bool bootWasPressed = false;
    bool deleteLogsTriggered = false;  // Track if 10s BOOT hold triggered delete mode

    // Timing constants (mirrors previous inline logic)
    static constexpr unsigned long BOOT_DEBOUNCE_MS = 300;
    static constexpr unsigned long AP_TOGGLE_LONG_PRESS_MS = 4000;
    static constexpr unsigned long VOLUME_TEST_DEBOUNCE_MS = 1000;
    static constexpr unsigned long DELETE_LOGS_LONG_PRESS_MS = 10000;  // 10 second BOOT button hold
};
