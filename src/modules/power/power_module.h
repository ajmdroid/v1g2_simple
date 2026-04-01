#pragma once

#include <Arduino.h>

#include "battery_manager.h"
#include "display.h"
#include "settings.h"

class PowerModule {
public:
    using ShutdownPreparationCallback = void (*)(void*);

    void begin(BatteryManager* batteryMgr,
               V1Display* disp,
               SettingsManager* settingsMgr);

    void setShutdownPreparationCallback(ShutdownPreparationCallback callback, void* context);

    // Log initial battery status after display init.
    void logStartupStatus();

    // Mark that we have seen real V1 data (arms auto power-off on disconnect).
    void onV1DataReceived();

    // Notify connection changes to manage auto power-off timers.
    void onV1ConnectionChange(bool connected);

    // Run periodic tasks: battery polling, critical shutdown, auto power-off timer.
    void process(unsigned long nowMs);

private:
    void performShutdownRequest();

    BatteryManager* battery_ = nullptr;
    V1Display* display_ = nullptr;
    SettingsManager* settings_ = nullptr;
    ShutdownPreparationCallback shutdownPreparationCallback_ = nullptr;
    void* shutdownPreparationContext_ = nullptr;

    bool lowBatteryWarningShown_ = false;
    unsigned long criticalBatteryTime_ = 0;

    unsigned long autoPowerOffTimerStart_ = 0;  // 0 = timer not running
    bool autoPowerOffArmed_ = false;
};
