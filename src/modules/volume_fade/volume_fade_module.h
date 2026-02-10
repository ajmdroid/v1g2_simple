#pragma once

/**
 * VolumeFadeModule - V1 volume fade logic
 * 
 * Responsibilities:
 * - Track when alert starts (for fade delay)
 * - Decide when to fade volume down
 * - Track original volume for restoration
 * - Decide when to restore volume
 * 
 * Does NOT:
 * - Send BLE commands (returns action for main to execute)
 */

#include <stdint.h>

// Forward declarations
class SettingsManager;

/**
 * Context for volume fade decisions
 */
struct VolumeFadeContext {
    bool hasAlert;              // Currently have active alert
    bool alertMuted;            // Alert is muted by user
    bool alertSuppressed;       // Alert is software-suppressed
    uint8_t currentVolume;      // Current V1 volume
    uint8_t currentMuteVolume;  // Current V1 mute volume
    uint16_t currentFrequency;  // Current priority frequency (MHz*10) for dedup
    bool speedBoostActive;      // Speed volume boost currently active
    uint8_t speedBoostOriginalVolume; // Pre-boost volume (0xFF = unknown)
    unsigned long now;          // Current timestamp
    
    VolumeFadeContext() : 
        hasAlert(false), alertMuted(false), alertSuppressed(false),
        currentVolume(0), currentMuteVolume(0), currentFrequency(0),
        speedBoostActive(false), speedBoostOriginalVolume(0xFF), now(0) {}
};

/**
 * Action returned by volume fade module
 */
struct VolumeFadeAction {
    enum class Type : uint8_t {
        NONE = 0,
        FADE_DOWN,      // Reduce volume
        RESTORE         // Restore original volume
    };
    
    Type type = Type::NONE;
    uint8_t targetVolume;       // Volume to set (FADE_DOWN)
    uint8_t targetMuteVolume;   // Mute volume to set (FADE_DOWN)
    uint8_t restoreVolume;      // Volume to restore to (RESTORE)
    uint8_t restoreMuteVolume;  // Mute volume to restore to (RESTORE)
    
    VolumeFadeAction() : 
        type(Type::NONE), targetVolume(0), targetMuteVolume(0),
        restoreVolume(0), restoreMuteVolume(0) {}
    
    bool hasAction() const { return type != Type::NONE; }
};

/**
 * VolumeFadeModule class
 */
class VolumeFadeModule {
public:
    VolumeFadeModule();
    
    void begin(SettingsManager* settings);
    
    // Main decision method
    VolumeFadeAction process(const VolumeFadeContext& ctx);
    
    // State management  
    void reset();               // Reset all tracking state
    
    // Returns true if fade logic has captured an original volume (blocks speed boost)
    bool isTracking() const { return originalVolume != 0xFF; }
    
private:
    SettingsManager* settings;
    
    // Tracking state
    unsigned long alertStartMs;
    uint8_t originalVolume;
    uint8_t originalMuteVolume;
    bool fadeActive;
    bool commandSent;
    int seenCount;
    static constexpr int MAX_FADE_SEEN_FREQS = 12;
    uint16_t seenFreqs[MAX_FADE_SEEN_FREQS];
};
