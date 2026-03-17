#pragma once

#include <Arduino.h>

#include "touch_handler.h"
#include "display.h"
#include "display_mode.h"
#include "settings.h"
#include "ble_client.h"
#include "packet_parser.h"

class AutoPushModule;
class AlertPersistenceModule;

class TapGestureModule {
public:
    void begin(TouchHandler* touchHandler,
               SettingsManager* settingsMgr,
               V1Display* display,
               V1BLEClient* bleClient,
               PacketParser* parser,
               AutoPushModule* autoPushModule,
               AlertPersistenceModule* alertPersistenceModule,
               DisplayMode* displayModePtr);

    void process(unsigned long nowMs);

private:
    TouchHandler* touch = nullptr;
    SettingsManager* settings = nullptr;
    V1Display* display = nullptr;
    V1BLEClient* ble = nullptr;
    PacketParser* parser = nullptr;
    AutoPushModule* autoPush = nullptr;
    AlertPersistenceModule* alertPersistence = nullptr;
    DisplayMode* displayMode = nullptr;

    unsigned long lastTapTime = 0;
    int tapCount = 0;
    static constexpr int PROFILE_CHANGE_TAP_COUNT = 3;
    static constexpr unsigned long TAP_WINDOW_MS = 600;
    static constexpr unsigned long TAP_DEBOUNCE_MS = 150;
};
