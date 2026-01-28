#include "speed_volume_module.h"
#include "settings.h"

SpeedVolumeModule::SpeedVolumeModule() {}

void SpeedVolumeModule::begin(SettingsManager* sett) {
    settings = sett;
    reset();
}

SpeedVolumeAction SpeedVolumeModule::process(const SpeedVolumeContext& ctx) {
    SpeedVolumeAction action;
    if (!settings) return action;
    const V1Settings& s = settings->get();
    
    // Honor feature toggle and BLE connectivity; when fade owns volume, do not touch it
    if (!s.speedVolumeEnabled || !ctx.bleConnected || ctx.fadeTakingControl) {
        // Safe path: if fade is active, skip any restore to avoid fighting fade ownership
        if (!ctx.fadeTakingControl && boostActive && originalVolume != 0xFF && ctx.currentVolume != originalVolume) {
            action.type = SpeedVolumeAction::Type::RESTORE;
            action.volume = originalVolume;
            action.muteVolume = ctx.currentMuteVolume;
        }
        reset();
        return action;
    }
    
    // Rate-limit checks to reduce BLE chatter
    if (ctx.now - lastCheckMs < CHECK_INTERVAL_MS) {
        return action;
    }
    lastCheckMs = ctx.now;
    
    if (!loggedSettings) {
        Serial.printf("[SpeedVolume] Settings: enabled=%d threshold=%d boost=%d\n",
                      s.speedVolumeEnabled, s.speedVolumeThresholdMph, s.speedVolumeBoost);
        loggedSettings = true;
    }
    
    bool shouldBoost = ctx.speedMph >= s.speedVolumeThresholdMph;
    
    if (shouldBoost && !boostActive) {
        originalVolume = ctx.currentVolume;
        uint8_t boostedVol = (uint8_t)std::min<int>(ctx.currentVolume + s.speedVolumeBoost, 9);
        if (boostedVol > ctx.currentVolume) {
            action.type = SpeedVolumeAction::Type::BOOST;
            action.volume = boostedVol;
            action.muteVolume = ctx.currentMuteVolume;
            boostActive = true;
        }
        return action;
    }
    
    if (!shouldBoost && boostActive) {
        if (originalVolume != 0xFF && ctx.currentVolume != originalVolume) {
            action.type = SpeedVolumeAction::Type::RESTORE;
            action.volume = originalVolume;
            action.muteVolume = ctx.currentMuteVolume;
        }
        reset();
        return action;
    }
    
    return action;  // NONE
}

void SpeedVolumeModule::reset() {
    boostActive = false;
    originalVolume = 0xFF;
    lastCheckMs = 0;
    // keep loggedSettings to avoid spamming logs across resets
}

