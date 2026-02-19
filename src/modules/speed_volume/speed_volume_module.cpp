#include "speed_volume_module.h"
#include "settings.h"
#include "packet_parser.h"
#ifndef UNIT_TEST
#include "perf_metrics.h"
#define SPEED_VOL_PERF_INC(counter) PERF_INC(counter)
#else
#define SPEED_VOL_PERF_INC(counter) do { } while (0)
#endif
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

    DisplayState state = parser->getDisplayState();

    // Proxy-connected sessions let the phone app own volume behavior.
    if (ble->isProxyClientConnected()) {
        if (boostActive && originalVolume != 0xFF && state.mainVolume != originalVolume) {
            ble->setVolume(originalVolume, state.muteVolume);
            SPEED_VOL_PERF_INC(speedVolRestores);
        }
        if (quietActive && originalQuietVolume != 0xFF && state.mainVolume != originalQuietVolume) {
            ble->setVolume(originalQuietVolume, state.muteVolume);
        }
        reset();
        return;
    }

    SpeedVolumeContext ctx;
    ctx.bleConnected = ble->isConnected();
    ctx.fadeTakingControl = volumeFade ? volumeFade->isTracking() : false;
    ctx.currentVolume = state.mainVolume;
    ctx.currentMuteVolume = state.muteVolume;
    ctx.speedMph = voice ? voice->getCurrentSpeedMph(nowMs) : 0.0f;
    ctx.hasValidSpeed = voice ? voice->hasValidSpeedSource(nowMs) : false;
    ctx.now = nowMs;

    SpeedVolumeAction action = process(ctx);

    // Execute action on hardware
    if (action.hasAction() && ble) {
        switch (action.type) {
            case SpeedVolumeAction::Type::BOOST:
            case SpeedVolumeAction::Type::QUIET:
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
    quietActive = false;
    originalQuietVolume = 0xFF;
    lastCheckMs = 0;
    // keep loggedSettings to avoid spamming logs across resets
}

uint8_t SpeedVolumeModule::getQuietVolume() const {
    if (!quietActive || !settings) return 0xFF;
    return settings->get().lowSpeedVolume;
}

SpeedVolumeAction SpeedVolumeModule::process(const SpeedVolumeContext& ctx) {
    SpeedVolumeAction action;
    if (!settings) return action;

    const V1Settings& s = settings->get();

    // Honor feature toggle and BLE connectivity; when fade owns volume, do not touch it
    if (!s.speedVolumeEnabled || !ctx.bleConnected || ctx.fadeTakingControl) {
        if (ctx.fadeTakingControl && boostActive) {
            SPEED_VOL_PERF_INC(speedVolFadeTakeovers);
        }
        if (!ctx.fadeTakingControl && boostActive && originalVolume != 0xFF &&
            ctx.currentVolume != originalVolume) {
            // Restore if we own the boost
            action.type = SpeedVolumeAction::Type::RESTORE;
            action.volume = originalVolume;
            action.muteVolume = ctx.currentMuteVolume;
            SPEED_VOL_PERF_INC(speedVolRestores);
        }
        if (!ctx.fadeTakingControl && quietActive && originalQuietVolume != 0xFF &&
            ctx.currentVolume != originalQuietVolume) {
            action.type = SpeedVolumeAction::Type::RESTORE;
            action.volume = originalQuietVolume;
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
        Serial.printf("[SpeedVolume] Settings: enabled=%d hiThresh=%d boost=%d loEnabled=%d loThresh=%d loVol=%d\n",
                      s.speedVolumeEnabled, s.speedVolumeThresholdMph, s.speedVolumeBoost,
                      s.lowSpeedMuteEnabled, s.lowSpeedMuteThresholdMph, s.lowSpeedVolume);
        loggedSettings = true;
    }

    // === Low-speed quiet logic ===
    bool shouldQuiet = s.lowSpeedMuteEnabled && ctx.hasValidSpeed &&
                       ctx.speedMph < s.lowSpeedMuteThresholdMph;

    if (shouldQuiet && !quietActive) {
        originalQuietVolume = ctx.currentVolume;
        uint8_t targetVol = s.lowSpeedVolume;
        if (ctx.currentVolume != targetVol) {
            action.type = SpeedVolumeAction::Type::QUIET;
            action.volume = targetVol;
            action.muteVolume = ctx.currentMuteVolume;
            quietActive = true;
            Serial.printf("[SpeedVolume] LOW-SPEED QUIET: %d -> %d (%.1f mph < %d)\n",
                          ctx.currentVolume, targetVol, ctx.speedMph, s.lowSpeedMuteThresholdMph);
        } else {
            quietActive = true;  // Already at target, just track
        }
        return action;
    }

    if (!shouldQuiet && quietActive) {
        if (originalQuietVolume != 0xFF && ctx.currentVolume != originalQuietVolume) {
            action.type = SpeedVolumeAction::Type::RESTORE;
            action.volume = originalQuietVolume;
            action.muteVolume = ctx.currentMuteVolume;
            Serial.printf("[SpeedVolume] LOW-SPEED RESTORE: %d -> %d\n",
                          ctx.currentVolume, originalQuietVolume);
        }
        quietActive = false;
        originalQuietVolume = 0xFF;
        return action;
    }

    // === High-speed boost logic ===

    bool shouldBoost = ctx.speedMph >= s.speedVolumeThresholdMph;

    if (shouldBoost && !boostActive) {
        originalVolume = ctx.currentVolume;
        uint8_t boostedVol = (uint8_t)std::min<int>(ctx.currentVolume + s.speedVolumeBoost, 9);
        if (boostedVol > ctx.currentVolume) {
            action.type = SpeedVolumeAction::Type::BOOST;
            action.volume = boostedVol;
            action.muteVolume = ctx.currentMuteVolume;
            boostActive = true;
            SPEED_VOL_PERF_INC(speedVolBoosts);
            Serial.printf("[SpeedVolume] BOOST: %d -> %d (fadeBlocked=%d)\n",
                          ctx.currentVolume, boostedVol, ctx.fadeTakingControl);
        } else {
            SPEED_VOL_PERF_INC(speedVolNoHeadroom);
        }
        return action;
    }

    if (!shouldBoost && boostActive) {
        if (originalVolume != 0xFF && ctx.currentVolume != originalVolume) {
            action.type = SpeedVolumeAction::Type::RESTORE;
            action.volume = originalVolume;
            action.muteVolume = ctx.currentMuteVolume;
            SPEED_VOL_PERF_INC(speedVolRestores);
        }
        reset();
        return action;
    }

    return action;
}
