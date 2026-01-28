#include "speed_volume_module.h"
#include "settings.h"
#include "packet_parser.h"
#ifndef UNIT_TEST
#include "ble_client.h"
#include "modules/voice/voice_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#else
#include "../../test/mocks/ble_client.h"
#include "../../test/mocks/modules/voice/voice_module.h"
#include "../../test/mocks/modules/volume_fade/volume_fade_module.h"
#endif

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

void SpeedVolumeModule::begin(SettingsManager* sett) {
    settings = sett;
    ble = nullptr;
    parser = nullptr;
    voice = nullptr;
    volumeFade = nullptr;
    reset();
}

void SpeedVolumeModule::process(unsigned long nowMs) {
    if (!settings || !ble || !parser) return;

    SpeedVolumeContext ctx;
    ctx.bleConnected = ble->isConnected();
    ctx.fadeTakingControl = volumeFade ? volumeFade->isTracking() : false;
    DisplayState state = parser->getDisplayState();
    ctx.currentVolume = state.mainVolume;
    ctx.currentMuteVolume = state.muteVolume;
    ctx.speedMph = voice ? voice->getCurrentSpeedMph(nowMs) : 0.0f;
    ctx.now = nowMs;

    SpeedVolumeAction action = process(ctx);

    // Execute action on hardware
    if (action.hasAction() && ble) {
        switch (action.type) {
            case SpeedVolumeAction::Type::BOOST:
            case SpeedVolumeAction::Type::RESTORE:
                ble->setVolume(action.volume, action.muteVolume);
                break;
            case SpeedVolumeAction::Type::NONE:
            default:
                break;
        }
    }
}

void SpeedVolumeModule::reset() {
    boostActive = false;
    originalVolume = 0xFF;
    lastCheckMs = 0;
    // keep loggedSettings to avoid spamming logs across resets
}

SpeedVolumeAction SpeedVolumeModule::process(const SpeedVolumeContext& ctx) {
    SpeedVolumeAction action;
    if (!settings) return action;

    const V1Settings& s = settings->get();

    // Honor feature toggle and BLE connectivity; when fade owns volume, do not touch it
    if (!s.speedVolumeEnabled || !ctx.bleConnected || ctx.fadeTakingControl) {
        if (!ctx.fadeTakingControl && boostActive && originalVolume != 0xFF &&
            ctx.currentVolume != originalVolume) {
            // Restore if we own the boost
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

    return action;
}
