#include "touch_ui_module.h"

#include "audio_beep.h"  // audio_set_volume, play_test_voice

void TouchUiModule::begin(V1Display* disp,
               TouchHandler* touch,
               SettingsManager* settingsMgr,
               const Callbacks& cbs) {
    display = disp;
    touchHandler = touch;
    settings = settingsMgr;
    callbacks = cbs;
}

bool TouchUiModule::process(unsigned long nowMs, bool bootPressed) {
    if (!display || !touchHandler || !settings) return false;

    // BOOT button handling: short press enters/exits adjust mode; long press toggles WiFi
    if (bootPressed && !bootWasPressed) {
        bootPressStart = nowMs;
        wifiToggleTriggered = false;
    }

    // Long press for WiFi toggle (only when not in adjust mode or delete logs mode)
    if (bootPressed && !wifiToggleTriggered && !brightnessAdjustMode && !deleteLogsMode) {
        unsigned long pressDuration = nowMs - bootPressStart;
        if (pressDuration >= AP_TOGGLE_LONG_PRESS_MS) {
            wifiToggleTriggered = true;
            if (callbacks.isWifiSetupActive && callbacks.isWifiSetupActive()) {
                if (callbacks.stopWifiSetup) callbacks.stopWifiSetup();
            } else {
                if (callbacks.startWifi) callbacks.startWifi();
            }
            if (callbacks.drawWifiIndicator) callbacks.drawWifiIndicator();
            display->flush();
        }
    }

    if (!bootPressed && bootWasPressed) {
        unsigned long pressDuration = nowMs - bootPressStart;
        if (pressDuration >= BOOT_DEBOUNCE_MS && !wifiToggleTriggered) {
            if (deleteLogsMode) {
                // BOOT press exits delete logs mode without deleting
                exitDeleteLogsMode(false);
            } else if (brightnessAdjustMode) {
                exitAdjustModeAndSave();
            } else {
                enterAdjustMode();
            }
        }
    }

    bootWasPressed = bootPressed;

    // Handle delete logs mode (highest priority after BOOT button)
    if (deleteLogsMode) {
        handleDeleteLogsTouch(nowMs);
        return true;  // consume loop while in delete logs mode
    }

    // If in settings adjustment mode, handle touch sliders and debounce test voice
    if (brightnessAdjustMode) {
        bool touched = handleSliderTouch(nowMs);

        if (!touched && lastVolumeChangeMs > 0 &&
            (nowMs - lastVolumeChangeMs) >= VOLUME_TEST_DEBOUNCE_MS) {
            play_test_voice();
            lastVolumeChangeMs = 0;
        }
        return true;  // consume loop while adjusting
    }

    // Touch long-press detection for delete logs screen (only when idle)
    int16_t touchX, touchY;
    bool touchPressed = touchHandler->getTouchPoint(touchX, touchY);
    
    if (touchPressed && !touchWasPressed) {
        touchPressStart = nowMs;
        deleteLogsTriggered = false;
    }
    
    if (touchPressed && !deleteLogsTriggered) {
        unsigned long pressDuration = nowMs - touchPressStart;
        if (pressDuration >= DELETE_LOGS_LONG_PRESS_MS) {
            deleteLogsTriggered = true;
            enterDeleteLogsMode();
        }
    }
    
    if (!touchPressed && touchWasPressed) {
        // Touch released - reset tracking
        deleteLogsTriggered = false;
    }
    
    touchWasPressed = touchPressed;

    return false;
}

void TouchUiModule::enterAdjustMode() {
    const V1Settings& s = settings->get();
    brightnessAdjustMode = true;
    brightnessAdjustValue = s.brightness;
    volumeAdjustValue = s.voiceVolume;
    activeSlider = 0;
    lastVolumeChangeMs = 0;
    display->showSettingsSliders(brightnessAdjustValue, volumeAdjustValue);
    Serial.printf("[Settings] Entering adjustment mode (brightness: %d, volume: %d)\n",
                  brightnessAdjustValue, volumeAdjustValue);
}

void TouchUiModule::exitAdjustModeAndSave() {
    brightnessAdjustMode = false;
    settings->updateBrightness(brightnessAdjustValue);
    settings->updateVoiceVolume(volumeAdjustValue);
    settings->save();
    audio_set_volume(volumeAdjustValue);
    display->hideBrightnessSlider();
    if (callbacks.restoreDisplay) callbacks.restoreDisplay();
    Serial.printf("[Settings] Saved brightness: %d, volume: %d\n", brightnessAdjustValue, volumeAdjustValue);
}

bool TouchUiModule::handleSliderTouch(unsigned long nowMs) {
    int16_t touchX, touchY;
    if (!touchHandler->getTouchPoint(touchX, touchY)) {
        return false;
    }

    // Map touch to slider region
    const int sliderX = 40;
    const int sliderWidth = SCREEN_WIDTH - 80;  // 560 pixels

    int touchedSlider = display->getActiveSliderFromTouch(touchY);
    if (touchedSlider < 0 || touchX < sliderX || touchX > sliderX + sliderWidth) {
        return true;  // touch occurred but not on slider region
    }

    activeSlider = touchedSlider;

    if (activeSlider == 0) {
        int newLevel = 255 - (((touchX - sliderX) * 175) / sliderWidth);
        if (newLevel < 80) newLevel = 80;
        if (newLevel > 255) newLevel = 255;
        if (newLevel != brightnessAdjustValue) {
            brightnessAdjustValue = newLevel;
            display->updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
        }
    } else if (activeSlider == 1) {
        int newVolume = 100 - (((touchX - sliderX) * 100) / sliderWidth);
        if (newVolume < 0) newVolume = 0;
        if (newVolume > 100) newVolume = 100;
        if (newVolume != volumeAdjustValue) {
            volumeAdjustValue = newVolume;
            audio_set_volume(volumeAdjustValue);
            display->updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
            lastVolumeChangeMs = nowMs;
        }
    }

    return true;
}

void TouchUiModule::enterDeleteLogsMode() {
    deleteLogsMode = true;
    display->showDeleteLogsScreen();
    Serial.println("[Touch] Entering delete logs mode");
}

void TouchUiModule::exitDeleteLogsMode(bool performDelete) {
    deleteLogsMode = false;
    
    if (performDelete && callbacks.deleteDebugLogs) {
        bool success = callbacks.deleteDebugLogs();
        Serial.printf("[Touch] Delete debug logs: %s\n", success ? "SUCCESS" : "FAILED");
    } else {
        Serial.println("[Touch] Delete logs cancelled");
    }
    
    display->hideBrightnessSlider();  // Clears screen
    if (callbacks.restoreDisplay) callbacks.restoreDisplay();
}

bool TouchUiModule::handleDeleteLogsTouch(unsigned long nowMs) {
    int16_t touchX, touchY;
    if (!touchHandler->getTouchPoint(touchX, touchY)) {
        return false;
    }
    
    int button = display->getDeleteLogsButtonFromTouch(touchX, touchY);
    if (button == 0) {
        // Cancel pressed
        exitDeleteLogsMode(false);
    } else if (button == 1) {
        // Delete pressed
        exitDeleteLogsMode(true);
    }
    
    return true;
}
