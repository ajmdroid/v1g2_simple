#include "volume_fade_module.h"
#include "settings.h"
#include <cstring>
#include <Arduino.h>

VolumeFadeModule::VolumeFadeModule() 
    : settings(nullptr)
    , alertStartMs(0)
    , originalVolume(0xFF)
    , originalMuteVolume(0)
    , fadeActive(false)
    , commandSent(false)
    , seenCount(0) {
    memset(seenFreqs, 0, sizeof(seenFreqs));
}

void VolumeFadeModule::begin(SettingsManager* settings) {
    this->settings = settings;
}

VolumeFadeAction VolumeFadeModule::process(const VolumeFadeContext& ctx) {
    VolumeFadeAction action;
    
    if (!settings) return action;
    const V1Settings& s = settings->get();
    
    // If feature disabled, clear any tracking so we don't block speed boost
    if (!s.alertVolumeFadeEnabled) {
        reset();
        return action;
    }
    
    // No active alerts -> optionally restore, then reset tracking
    if (!ctx.hasAlert) {
        if (originalVolume != 0xFF && ctx.currentVolume != originalVolume) {
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume;
            action.restoreMuteVolume = originalMuteVolume;
            Serial.printf("[VolumeFade] RESTORE: current=%d -> original=%d\n",
                          ctx.currentVolume, originalVolume);
        } else if (originalVolume != 0xFF) {
            Serial.printf("[VolumeFade] No restore needed: current=%d == original=%d\n",
                          ctx.currentVolume, originalVolume);
        }
        reset();
        return action;
    }
    
    // Alert muted or in lockout -> restore if we had faded, then reset
    if (ctx.alertMuted || ctx.alertInLockout) {
        if (fadeActive && originalVolume != 0xFF && ctx.currentVolume != originalVolume) {
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume;
            action.restoreMuteVolume = originalMuteVolume;
        }
        reset();
        return action;
    }
    
    unsigned long now = ctx.now;
    uint16_t freq = ctx.currentFrequency;
    
    // Determine if this is a new frequency during the same alert session
    bool isNewFrequency = freq != 0;
    if (isNewFrequency) {
        for (int i = 0; i < seenCount; i++) {
            if (seenFreqs[i] == freq) {
                isNewFrequency = false;
                break;
            }
        }
    }
    
    // If we're currently faded and a new frequency shows up, restore and restart timer
    if (fadeActive && isNewFrequency) {
        if (originalVolume != 0xFF && ctx.currentVolume != originalVolume) {
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume;
            action.restoreMuteVolume = originalMuteVolume;
        }
        alertStartMs = now;
        fadeActive = false;
        commandSent = false;
        if (seenCount < MAX_FADE_SEEN_FREQS && freq != 0) {
            seenFreqs[seenCount++] = freq;
        }
        return action;
    }
    
    // First alert in session - capture baseline volumes and start timer
    if (alertStartMs == 0) {
        alertStartMs = now;
        originalVolume = (ctx.speedBoostActive && ctx.speedBoostOriginalVolume != 0xFF)
                           ? ctx.speedBoostOriginalVolume
                           : ctx.currentVolume;
        originalMuteVolume = ctx.currentMuteVolume;
        fadeActive = false;
        commandSent = false;
        seenCount = 0;
        if (seenCount < MAX_FADE_SEEN_FREQS && freq != 0) {
            seenFreqs[seenCount++] = freq;
        }
    }
    
    // Check if it's time to fade down
    unsigned long fadeDelayMs = static_cast<unsigned long>(s.alertVolumeFadeDelaySec) * 1000UL;
    if (!commandSent && (now - alertStartMs) >= fadeDelayMs) {
        uint8_t fadeVol = s.alertVolumeFadeVolume;
        if (ctx.currentVolume > fadeVol) {
            action.type = VolumeFadeAction::Type::FADE_DOWN;
            action.targetVolume = fadeVol;
            action.targetMuteVolume = originalMuteVolume;
            fadeActive = true;
        }
        commandSent = true;  // Do not retry if it fails; mirrors prior behavior
        return action;
    }
    
    return action;
}

void VolumeFadeModule::reset() {
    alertStartMs = 0;
    originalVolume = 0xFF;
    originalMuteVolume = 0;
    fadeActive = false;
    commandSent = false;
    seenCount = 0;
    memset(seenFreqs, 0, sizeof(seenFreqs));
}
