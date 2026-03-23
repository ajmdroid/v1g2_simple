#include "tap_gesture_module.h"
#include "../quiet/quiet_coordinator_module.h"

#ifndef UNIT_TEST
#include "modules/auto_push/auto_push_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#endif

void TapGestureModule::begin(TouchHandler* touchHandler,
                             SettingsManager* settingsMgr,
                             V1Display* displayPtr,
                             V1BLEClient* bleClient,
                             PacketParser* parserPtr,
                             AutoPushModule* autoPushModule,
                             AlertPersistenceModule* alertPersistenceModule,
                             DisplayMode* displayModePtr,
                             QuietCoordinatorModule* quietCoordinator) {
    touch = touchHandler;
    settings = settingsMgr;
    display = displayPtr;
    ble = bleClient;
    parser = parserPtr;
    autoPush = autoPushModule;
    alertPersistence = alertPersistenceModule;
    displayMode = displayModePtr;
    quiet = quietCoordinator;
}

void TapGestureModule::process(unsigned long nowMs) {
    if (!touch || !settings || !display || !ble || !parser || !autoPush || !alertPersistence || !displayMode) {
        return;
    }

    int16_t touchX, touchY;
    bool hasActiveAlert = parser->hasAlerts();

    auto performMuteToggle = [&](const char* reason) {
        if (!hasActiveAlert) {
            Serial.println("MUTE BLOCKED: No active alert to mute");
            return;
        }

        DisplayState state = parser->getDisplayState();
        bool currentMuted = state.muted;
        bool newMuted = !currentMuted;

        Serial.printf("Mute: %s -> Sending: %s (%s)\n",
                      currentMuted ? "MUTED" : "UNMUTED",
                      newMuted ? "MUTE_ON" : "MUTE_OFF",
                      reason);

        const bool cmdSent = quiet && quiet->sendMute(QuietOwner::TapGesture, newMuted);
        Serial.printf("Mute command sent: %s\n", cmdSent ? "OK" : "FAIL");
    };

    auto performProfileCycle = [&]() {
        const V1Settings& s = settings->get();
        int newSlot = (s.activeSlot + 1) % 3;
        settings->setActiveSlot(newSlot);
        *displayMode = DisplayMode::IDLE;

        alertPersistence->clearPersistence();

        const char* slotNames[] = {"Default", "Highway", "Comfort"};
        Serial.printf("PROFILE CHANGE: Switched to '%s' (slot %d)\n", slotNames[newSlot], newSlot);

        display->drawProfileIndicator(newSlot);

        if (ble->isConnected() && s.autoPushEnabled) {
            Serial.println("Pushing new profile to V1...");
            const auto queueResult = autoPush->queueSlotPush(newSlot);
            if (queueResult != AutoPushModule::QueueResult::QUEUED) {
                Serial.printf("Profile push skipped: %d\n", static_cast<int>(queueResult));
            }
        }
    };

    if (touch->getTouchPoint(touchX, touchY)) {
        if (nowMs - lastTapTime >= TAP_DEBOUNCE_MS) {
            if (nowMs - lastTapTime <= TAP_WINDOW_MS) {
                tapCount++;
            } else {
                tapCount = 1;
            }
            lastTapTime = nowMs;

            Serial.printf("Tap detected: count=%d, x=%d, y=%d, hasAlert=%d\n", tapCount, touchX, touchY, hasActiveAlert);

            if (hasActiveAlert && tapCount == 1) {
                performMuteToggle("immediate tap");
                tapCount = 0;
                return;
            }

            if (!hasActiveAlert && tapCount >= PROFILE_CHANGE_TAP_COUNT) {
                performProfileCycle();
                tapCount = 0;
            } else if (hasActiveAlert) {
                Serial.printf("Processing %d tap(s) as mute toggle\n", tapCount);
                performMuteToggle("deferred tap");
                tapCount = 0;
            }
        }
    }
}
