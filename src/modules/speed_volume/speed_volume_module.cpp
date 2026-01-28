#include "speed_volume_module.h"
#include "settings.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "modules/voice/voice_module.h"
#include "modules/volume_fade/volume_fade_module.h"

SpeedVolumeModule::SpeedVolumeModule() {}

void SpeedVolumeModule::begin(SettingsManager* sett,
                              V1BLEClient* bleClient,
                              PacketParser* parserPtr,
                              VoiceModule* voiceModule,
                              VolumeFadeModule* volumeFadeModule) {
    settings = sett;
    ble = bleClient;
    parser = parserPtr;
    voice = voiceModule;
    volumeFade = volumeFadeModule;
    reset();
}

void SpeedVolumeModule::process(unsigned long nowMs) {
    if (!settings || !ble || !parser) return;
    
    // Build context internally
    bool bleConnected = ble->isConnected();
    bool fadeTakingControl = volumeFade ? volumeFade->isTracking() : false;
    DisplayState state = parser->getDisplayState();
    uint8_t currentVolume = state.mainVolume;
    uint8_t currentMuteVolume = state.muteVolume;
    float speedMph = voice ? voice->getCurrentSpeedMph(nowMs) : 0.0f;
    
    const V1Settings& s = settings->get();
    
    // Honor feature toggle and BLE connectivity; when fade owns volume, do not touch it
    if (!s.speedVolumeEnabled || !bleConnected || fadeTakingControl) {
        // Safe path: if fade is active, skip any restore to avoid fighting fade ownership
        if (!fadeTakingControl && boostActive && originalVolume != 0xFF && currentVolume != originalVolume) {
            Serial.printf("[SpeedVolume] Restoring volume to %d\n", originalVolume);
            ble->setVolume(originalVolume, currentMuteVolume);
        }
        reset();
        return;
    }
    
    // Rate-limit checks to reduce BLE chatter
    if (nowMs - lastCheckMs < CHECK_INTERVAL_MS) {
        return;
    }
    lastCheckMs = nowMs;
    
    if (!loggedSettings) {
        Serial.printf("[SpeedVolume] Settings: enabled=%d threshold=%d boost=%d\n",
                      s.speedVolumeEnabled, s.speedVolumeThresholdMph, s.speedVolumeBoost);
        loggedSettings = true;
    }
    
    bool shouldBoost = speedMph >= s.speedVolumeThresholdMph;
    
    if (shouldBoost && !boostActive) {
        originalVolume = currentVolume;
        uint8_t boostedVol = (uint8_t)std::min<int>(currentVolume + s.speedVolumeBoost, 9);
        if (boostedVol > currentVolume) {
            Serial.printf("[SpeedVolume] Boosting volume to %d (speed=%.0f)\n", boostedVol, speedMph);
            ble->setVolume(boostedVol, currentMuteVolume);
            boostActive = true;
        }
        return;
    }
    
    if (!shouldBoost && boostActive) {
        if (originalVolume != 0xFF && currentVolume != originalVolume) {
            Serial.printf("[SpeedVolume] Restoring volume to %d\n", originalVolume);
            ble->setVolume(originalVolume, currentMuteVolume);
        }
        reset();
        return;
    }
}

void SpeedVolumeModule::reset() {
    boostActive = false;
    originalVolume = 0xFF;
    lastCheckMs = 0;
    // keep loggedSettings to avoid spamming logs across resets
}

