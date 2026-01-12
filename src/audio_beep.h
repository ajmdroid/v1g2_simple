// audio_beep.h
#pragma once

#include <stdint.h>

// Band types for voice alerts
enum class AlertBand : uint8_t {
    LASER = 0,
    KA = 1,
    K = 2,
    X = 3
};

// Direction types for voice alerts
enum class AlertDirection : uint8_t {
    AHEAD = 0,
    BEHIND = 1,
    SIDE = 2
};

// Set audio volume (0-100%)
void audio_set_volume(uint8_t volumePercent);

// Play "Test" for volume confirmation
void play_test_voice();

// Call to play a beep for VOL 0 warning
void play_vol0_beep();

// Play voice alert for a specific band and direction
// Returns immediately if already playing or audio disabled
void play_alert_voice(AlertBand band, AlertDirection direction);

// Test beep on startup (for debugging)
void play_test_beep();