#include "tap_gesture_module.h"

void TapGestureModule::begin(TouchHandler* touchHandler,
                             SettingsManager* settingsMgr,
                             V1Display* displayPtr,
                             V1BLEClient* bleClient,
                             PacketParser* parserPtr,
                             CameraAlertModule* cameraAlertModule,
                             AutoPushModule* autoPushModule,
                             AlertPersistenceModule* alertPersistenceModule,
                             DisplayMode* displayModePtr) {
    touch = touchHandler;
    settings = settingsMgr;
    display = displayPtr;
    ble = bleClient;
    parser = parserPtr;
    cameraAlert = cameraAlertModule;
    autoPush = autoPushModule;
    alertPersistence = alertPersistenceModule;
    displayMode = displayModePtr;
}

void TapGestureModule::process(unsigned long nowMs) {
    if (!touch || !settings || !display || !ble || !parser || !cameraAlert || !autoPush || !alertPersistence || !displayMode) {
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

        bool cmdSent = ble->setMute(newMuted);
        Serial.printf("Mute command sent: %s\n", cmdSent ? "OK" : "FAIL");
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

            if (tapCount >= 3) {
                tapCount = 0;

                if (hasActiveAlert) {
                    Serial.println("PROFILE CHANGE BLOCKED: Active alert present - tap to mute instead");
                } else {
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
                        autoPush->start(newSlot);
                    }
                }
            }
        }
    } else {
        if (tapCount > 0 && tapCount < 3 && (nowMs - lastTapTime > TAP_WINDOW_MS)) {
            Serial.printf("Processing %d tap(s) as mute toggle\n", tapCount);
            tapCount = 0;
            performMuteToggle("deferred tap");
        }
    }
}
