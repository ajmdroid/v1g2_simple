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
    display_ = disp;
    touchHandler_ = touch;
    settings_ = settingsMgr;
    callbacks_ = cbs;
}

bool TouchUiModule::process(unsigned long nowMs, bool bootPressed) {
    if (!display_ || !touchHandler_ || !settings_) return false;

    // BOOT button handling:
    // - Short press: enter/exit adjust mode
    // - 4s hold: toggle WiFi (on release)
    if (bootPressed && !bootWasPressed_) {
        bootPressStart_ = nowMs;
    }

    if (bootPressed) {
        const bool shouldArmObdPair = !brightnessAdjustMode_ &&
                                      (nowMs - bootPressStart_) >= OBD_PAIR_LONG_PRESS_MS &&
                                      canArmObdPairGesture(nowMs);
        if (shouldArmObdPair != obdPairGestureArmed_) {
            obdPairGestureArmed_ = shouldArmObdPair;
            updateObdIndicatorAttention(obdPairGestureArmed_, nowMs);
        }
    }

    // On release: determine action based on hold duration
    if (!bootPressed && bootWasPressed_) {
        unsigned long pressDuration = nowMs - bootPressStart_;
        const bool triggerObdPair = obdPairGestureArmed_ && pressDuration >= OBD_PAIR_LONG_PRESS_MS;

        if (obdPairGestureArmed_) {
            obdPairGestureArmed_ = false;
            updateObdIndicatorAttention(false, nowMs);
        }

        if (triggerObdPair) {
            if (callbacks_.requestObdManualPairScan) {
                (void)callbacks_.requestObdManualPairScan(nowMs, callbacks_.requestObdManualPairScanCtx);
            }
            display_->refreshObdIndicator(nowMs);
            display_->flushRegion(kObdBadgeFlushX, kObdBadgeFlushY, kObdBadgeFlushW, kObdBadgeFlushH);
        } else if (pressDuration >= AP_TOGGLE_LONG_PRESS_MS) {
            // 4s+ hold: toggle WiFi on release
            if (callbacks_.isWifiSetupActive && callbacks_.isWifiSetupActive(callbacks_.isWifiSetupActiveCtx)) {
                if (callbacks_.stopWifiSetup) callbacks_.stopWifiSetup(callbacks_.stopWifiSetupCtx);
            } else {
                if (callbacks_.startWifi) callbacks_.startWifi(callbacks_.startWifiCtx);
            }
            if (callbacks_.drawWifiIndicator) callbacks_.drawWifiIndicator(callbacks_.drawWifiIndicatorCtx);
            display_->flush();
        } else if (pressDuration >= BOOT_DEBOUNCE_MS) {
            // Short press: adjust mode toggle
            if (brightnessAdjustMode_) {
                exitAdjustModeAndSave();
            } else {
                enterAdjustMode();
            }
        }
    }

    bootWasPressed_ = bootPressed;

    // If in settings adjustment mode, handle touch sliders and debounce test voice
    if (brightnessAdjustMode_) {
        bool touched = handleSliderTouch(nowMs);

        if (!touched && lastVolumeChangeMs_ > 0 &&
            (nowMs - lastVolumeChangeMs_) >= VOLUME_TEST_DEBOUNCE_MS) {
            play_test_voice();
            lastVolumeChangeMs_ = 0;
        }
        return true;  // consume loop while adjusting
    }

    return false;
}

bool TouchUiModule::canArmObdPairGesture(unsigned long nowMs) const {
    if (!callbacks_.readObdStatus || !callbacks_.isObdPairGestureSafe) {
        return false;
    }

    const ObdRuntimeStatus status = callbacks_.readObdStatus(nowMs, callbacks_.readObdStatusCtx);
    return status.enabled &&
           !status.connected &&
           !status.scanInProgress &&
           !status.manualScanPending &&
           callbacks_.isObdPairGestureSafe(nowMs, callbacks_.isObdPairGestureSafeCtx);
}

void TouchUiModule::updateObdIndicatorAttention(bool attention, unsigned long nowMs) {
    display_->setObdAttention(attention);
    display_->refreshObdIndicator(nowMs);
    display_->flushRegion(kObdBadgeFlushX, kObdBadgeFlushY, kObdBadgeFlushW, kObdBadgeFlushH);
}

void TouchUiModule::enterAdjustMode() {
    const V1Settings& s = settings_->get();
    brightnessAdjustMode_ = true;
    brightnessAdjustValue_ = s.brightness;
    volumeAdjustValue_ = s.voiceVolume;
    activeSlider_ = 0;
    lastVolumeChangeMs_ = 0;
    display_->showSettingsSliders(brightnessAdjustValue_, volumeAdjustValue_);
    Serial.printf("[Settings] Entering adjustment mode (brightness: %d, volume: %d)\n",
                  brightnessAdjustValue_, volumeAdjustValue_);
}

void TouchUiModule::exitAdjustModeAndSave() {
    brightnessAdjustMode_ = false;
    settings_->updateBrightness(brightnessAdjustValue_);
    settings_->updateVoiceVolume(volumeAdjustValue_);
    settings_->save();
    audio_set_volume(volumeAdjustValue_);
    display_->hideBrightnessSlider();
    if (callbacks_.restoreDisplay) callbacks_.restoreDisplay(callbacks_.restoreDisplayCtx);
    Serial.printf("[Settings] Saved brightness: %d, volume: %d\n", brightnessAdjustValue_, volumeAdjustValue_);
}

bool TouchUiModule::handleSliderTouch(unsigned long nowMs) {
    int16_t touchX, touchY;
    if (!touchHandler_->getTouchPoint(touchX, touchY)) {
        return false;
    }

    // Map touch to slider region
    const int sliderX = 40;
    const int sliderWidth = SCREEN_WIDTH - 80;  // 560 pixels

    int touchedSlider = display_->getActiveSliderFromTouch(touchY);
    if (touchedSlider < 0 || touchX < sliderX || touchX > sliderX + sliderWidth) {
        return true;  // touch occurred but not on slider region
    }

    activeSlider_ = touchedSlider;

    if (activeSlider_ == 0) {
        int newLevel = 255 - (((touchX - sliderX) * 175) / sliderWidth);
        if (newLevel < 80) newLevel = 80;
        if (newLevel > 255) newLevel = 255;
        if (newLevel != brightnessAdjustValue_) {
            brightnessAdjustValue_ = newLevel;
            if ((nowMs - lastSliderRedrawMs_) >= SLIDER_REDRAW_MIN_MS) {
                display_->updateSettingsSliders(brightnessAdjustValue_, volumeAdjustValue_, activeSlider_);
                lastSliderRedrawMs_ = nowMs;
            }
        }
    } else if (activeSlider_ == 1) {
        int newVolume = 100 - (((touchX - sliderX) * 100) / sliderWidth);
        if (newVolume < 0) newVolume = 0;
        if (newVolume > 100) newVolume = 100;
        if (newVolume != volumeAdjustValue_) {
            volumeAdjustValue_ = newVolume;
            audio_set_volume(volumeAdjustValue_);
            if ((nowMs - lastSliderRedrawMs_) >= SLIDER_REDRAW_MIN_MS) {
                display_->updateSettingsSliders(brightnessAdjustValue_, volumeAdjustValue_, activeSlider_);
                lastSliderRedrawMs_ = nowMs;
            }
            lastVolumeChangeMs_ = nowMs;
        }
    }

    return true;
}
