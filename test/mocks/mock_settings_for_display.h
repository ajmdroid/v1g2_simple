#pragma once
/**
 * mock_settings_for_display.h
 *
 * Phase 2 Task 2.2 — Helper that pre-populates a SettingsManager mock with
 * known, deterministic display-color values for use in rendering tests.
 *
 * Usage:
 *   #include "../mocks/settings.h"
 *   #include "../mocks/mock_settings_for_display.h"
 *   ...
 *   SettingsManager settingsManager;
 *   initDisplayTestSettings(settingsManager);
 */

#include "settings.h"

/// Populate sm with deterministic display-relevant values.
inline void initDisplayTestSettings(SettingsManager& sm) {
    sm.settings.colorBandL    = 0xF800;  // Red  for Laser
    sm.settings.colorBandKa   = 0x07E0;  // Green for Ka
    sm.settings.colorBandK    = 0x001F;  // Blue  for K
    sm.settings.colorBandX    = 0xFFE0;  // Yellow for X
    sm.settings.colorVolumeMain = 0x001F;  // Blue
    sm.settings.colorVolumeMute = 0xFFE0;  // Yellow
    sm.settings.brightness     = 200;
    sm.settings.activeSlot     = 1;
    sm.settings.hideRssiIndicator   = false;
    sm.settings.hideVolumeIndicator = false;
}
