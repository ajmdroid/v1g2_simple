#pragma once

#include <Arduino.h>

#include "battery_manager.h"
#include "display.h"
#include "settings.h"

class PowerModule {
public:
    void begin(BatteryManager* batteryMgr,
               V1Display* disp,
               SettingsManager* settingsMgr);

    // Log initial battery status after display init.
    void logStartupStatus();

    // Mark that we have seen real V1 data (arms auto power-off on disconnect).
    void onV1DataReceived();

    // Notify connection changes to manage auto power-off timers.
    void onV1ConnectionChange(bool connected);

    // Run periodic tasks: battery polling, critical shutdown, auto power-off timer.
    void process(unsigned long nowMs);

private:
    BatteryManager* battery = nullptr;
    V1Display* display = nullptr;
    SettingsManager* settings = nullptr;

    bool lowBatteryWarningShown = false;
    unsigned long criticalBatteryTime = 0;

    unsigned long autoPowerOffTimerStart = 0;  // 0 = timer not running
    bool autoPowerOffArmed = false;
};
