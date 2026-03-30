#include "touch_ui_module.h"

#include "audio_beep.h"  // audio_set_volume, play_test_voice

namespace {
constexpr int kObdBadgeFlushX = 360;
constexpr int kObdBadgeFlushY = 0;
constexpr int kObdBadgeFlushW = 72;
constexpr int kObdBadgeFlushH = 36;
}

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

    // BOOT button handling:
    // - Short press: enter/exit adjust mode
    // - 4s hold: toggle WiFi (on release)
    if (bootPressed && !bootWasPressed) {
        bootPressStart = nowMs;
    }

    if (bootPressed) {
        const bool shouldArmObdPair = !brightnessAdjustMode &&
                                      (nowMs - bootPressStart) >= OBD_PAIR_LONG_PRESS_MS &&
                                      canArmObdPairGesture(nowMs);
        if (shouldArmObdPair != obdPairGestureArmed) {
            obdPairGestureArmed = shouldArmObdPair;
            updateObdIndicatorAttention(obdPairGestureArmed, nowMs);
        }
    }

    // On release: determine action based on hold duration
    if (!bootPressed && bootWasPressed) {
        unsigned long pressDuration = nowMs - bootPressStart;
        const bool triggerObdPair = obdPairGestureArmed && pressDuration >= OBD_PAIR_LONG_PRESS_MS;

        if (obdPairGestureArmed) {
            obdPairGestureArmed = false;
            updateObdIndicatorAttention(false, nowMs);
        }

        if (triggerObdPair) {
            if (callbacks.requestObdManualPairScan) {
                (void)callbacks.requestObdManualPairScan(nowMs, callbacks.requestObdManualPairScanCtx);
            }
            display->refreshObdIndicator(nowMs);
            display->flushRegion(kObdBadgeFlushX, kObdBadgeFlushY, kObdBadgeFlushW, kObdBadgeFlushH);
        } else if (pressDuration >= AP_TOGGLE_LONG_PRESS_MS) {
            // 4s+ hold: toggle WiFi on release
            if (callbacks.isWifiSetupActive && callbacks.isWifiSetupActive(callbacks.isWifiSetupActiveCtx)) {
                if (callbacks.stopWifiSetup) callbacks.stopWifiSetup(callbacks.stopWifiSetupCtx);
            } else {
                if (callbacks.startWifi) callbacks.startWifi(callbacks.startWifiCtx);
            }
            if (callbacks.drawWifiIndicator) callbacks.drawWifiIndicator(callbacks.drawWifiIndicatorCtx);
            display->flush();
        } else if (pressDuration >= BOOT_DEBOUNCE_MS) {
            // Short press: adjust mode toggle
            if (brightnessAdjustMode) {
                exitAdjustModeAndSave();
            } else {
                enterAdjustMode();
            }
        }
    }

    bootWasPressed = bootPressed;

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

    return false;
}

bool TouchUiModule::canArmObdPairGesture(unsigned long nowMs) const {
    if (!callbacks.readObdStatus || !callbacks.isObdPairGestureSafe) {
        return false;
    }

    const ObdRuntimeStatus status = callbacks.readObdStatus(nowMs, callbacks.readObdStatusCtx);
    return status.enabled &&
           !status.connected &&
           !status.scanInProgress &&
           !status.manualScanPending &&
           callbacks.isObdPairGestureSafe(nowMs, callbacks.isObdPairGestureSafeCtx);
}

void TouchUiModule::updateObdIndicatorAttention(bool attention, unsigned long nowMs) {
    display->setObdAttention(attention);
    display->refreshObdIndicator(nowMs);
    display->flushRegion(kObdBadgeFlushX, kObdBadgeFlushY, kObdBadgeFlushW, kObdBadgeFlushH);
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
    if (callbacks.restoreDisplay) callbacks.restoreDisplay(callbacks.restoreDisplayCtx);
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
            if ((nowMs - lastSliderRedrawMs) >= SLIDER_REDRAW_MIN_MS) {
                display->updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
                lastSliderRedrawMs = nowMs;
            }
        }
    } else if (activeSlider == 1) {
        int newVolume = 100 - (((touchX - sliderX) * 100) / sliderWidth);
        if (newVolume < 0) newVolume = 0;
        if (newVolume > 100) newVolume = 100;
        if (newVolume != volumeAdjustValue) {
            volumeAdjustValue = newVolume;
            audio_set_volume(volumeAdjustValue);
            if ((nowMs - lastSliderRedrawMs) >= SLIDER_REDRAW_MIN_MS) {
                display->updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
                lastSliderRedrawMs = nowMs;
            }
            lastVolumeChangeMs = nowMs;
        }
    }

    return true;
}
