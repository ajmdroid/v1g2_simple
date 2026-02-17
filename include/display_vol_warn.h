// display_vol_warn.h — VolumeZeroWarning state machine for VOL 0 flashing alert
// Shared between display.cpp (setBLEProxyStatus reset) and display_update.cpp (evaluate/draw).
#ifndef DISPLAY_VOL_WARN_H
#define DISPLAY_VOL_WARN_H

#include <Arduino.h>

struct VolumeZeroWarning {
    unsigned long detectedMs      = 0;      // When volume=0 was first detected
    unsigned long warningStartMs  = 0;      // When warning display actually started
    bool          shown           = false;
    bool          acknowledged    = false;

    static constexpr unsigned long DELAY_MS    = 15000;   // Wait before showing
    static constexpr unsigned long DURATION_MS = 10000;   // Show for this long

    /// Reset all state (call when volume goes non-zero or app connects/disconnects).
    void reset() {
        detectedMs     = 0;
        warningStartMs = 0;
        shown          = false;
        acknowledged   = false;
    }

    /// Evaluate the state machine.  Returns true when the warning should be drawn.
    /// @param volZero          true when mainVolume == 0 && hasVolumeData
    /// @param proxyConnected   true when BLE proxy client is connected
    /// @param preQuietActive   true when lockout pre-quiet is suppressing the warning
    /// @param playBeepFn       called once when the warning first appears
    bool evaluate(bool volZero, bool proxyConnected, bool preQuietActive,
                  void (*playBeepFn)() = nullptr) {
        if (!volZero || proxyConnected || preQuietActive) {
            reset();
            return false;
        }
        if (acknowledged) {
            return false;
        }

        const unsigned long now = millis();
        if (detectedMs == 0) {
            detectedMs = now;
        }

        if ((now - detectedMs) < DELAY_MS) {
            return false;
        }

        if (warningStartMs == 0) {
            warningStartMs = now;
            shown = true;
            if (playBeepFn) playBeepFn();
        }

        if ((now - warningStartMs) < DURATION_MS) {
            return true;   // Warning is active — caller should draw
        }

        // Duration expired
        acknowledged = true;
        shown = false;
        return false;
    }

    /// Return true when a flashing redraw is needed even on the incremental path.
    /// Used in the resting-screen early-exit check.
    bool needsFlashRedraw(bool volZero, bool proxyConnected, bool preQuietActive) const {
        if (!volZero || proxyConnected || preQuietActive || acknowledged) {
            return false;
        }
        if (detectedMs == 0) {
            return true;   // Timer not started yet — force full redraw to start it
        }
        if ((millis() - detectedMs) >= DELAY_MS) {
            if (warningStartMs == 0 || (millis() - warningStartMs) < DURATION_MS) {
                return true;
            }
        }
        return shown;
    }
};

// Single shared instance — defined in display.cpp
extern VolumeZeroWarning volZeroWarn;

#endif // DISPLAY_VOL_WARN_H
