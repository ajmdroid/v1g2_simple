// Mock display.h for native unit testing
#pragma once
#ifndef DISPLAY_H
#define DISPLAY_H

#include <cstdint>
#ifdef ARDUINO
#include <Arduino.h>
#else
#include "Arduino.h"
#endif

// Forward declare AlertData
struct AlertData;
struct DisplayState;

#ifndef DISPLAY_BLE_CONTEXT_H
#define DISPLAY_BLE_CONTEXT_H
struct DisplayBleContext {
    bool v1Connected = false;
    bool proxyConnected = false;
    int v1Rssi = 0;
    int proxyRssi = 0;
};
#endif

// Screen dimensions (needed by some tests)
#ifndef SCREEN_WIDTH
#define SCREEN_WIDTH 640
#endif
#ifndef SCREEN_HEIGHT
#define SCREEN_HEIGHT 172
#endif

/**
 * Mock V1Display - tracks method calls for verification
 */
class V1Display {
public:
    // Call tracking
    int showScanningCalls = 0;
    int showRestingCalls = 0;
    int showDisconnectedCalls = 0;
    int updateCalls = 0;
    int updatePersistedCalls = 0;
    int flushCalls = 0;
    int forceNextRedrawCalls = 0;
    int drawWiFiIndicatorCalls = 0;
    int drawObdIndicatorCalls = 0;
    int drawBatteryIndicatorCalls = 0;
    int drawProfileIndicatorCalls = 0;
    int lastProfileIndicatorSlot = -1;
    int showLowBatteryCalls = 0;
    int showShutdownCalls = 0;
    int setSpeedVolZeroActiveCalls = 0;
    bool lastSpeedVolZeroActiveValue = false;
    int setBleContextCalls = 0;
    DisplayBleContext lastBleContext{};
    int setBLEProxyStatusCalls = 0;
    bool lastBleProxyEnabled = false;
    bool lastBleProxyConnected = false;
    bool lastBleReceiving = true;
    int setObdStatusCalls = 0;
    bool lastObdEnabled = false;
    bool lastObdConnected = false;
    bool lastObdScanAttention = false;
    int setObdAttentionCalls = 0;
    bool lastObdAttention = false;
    int syncTopIndicatorsCalls = 0;
    uint32_t lastSyncTopIndicatorsNowMs = 0;
    int flushRegionCalls = 0;
    int16_t lastFlushX = 0;
    int16_t lastFlushY = 0;
    int16_t lastFlushW = 0;
    int16_t lastFlushH = 0;
    int showSettingsSlidersCalls = 0;
    int updateSettingsSlidersCalls = 0;
    int hideBrightnessSliderCalls = 0;
    int lastSettingsBrightness = 0;
    int lastSettingsVolume = 0;
    int lastSettingsActiveSlider = -1;
    int activeSliderFromTouch = -1;
    int refreshFrequencyOnlyCalls = 0;
    uint32_t lastFrequencyMHz = 0;
    int lastFrequencyBand = 0;
    bool lastFrequencyMuted = false;
    bool lastFrequencyPhotoRadar = false;
    int lastAlertUpdateCount = 0;
    int refreshSecondaryAlertCardsCalls = 0;
    int lastSecondaryAlertCount = 0;
    bool lastSecondaryMuted = false;

    // Static method tracking
    static int resetChangeTrackingCalls;

    void reset() {
        showScanningCalls = 0;
        showRestingCalls = 0;
        showDisconnectedCalls = 0;
        updateCalls = 0;
        updatePersistedCalls = 0;
        flushCalls = 0;
        forceNextRedrawCalls = 0;
        drawWiFiIndicatorCalls = 0;
        drawObdIndicatorCalls = 0;
        drawBatteryIndicatorCalls = 0;
        drawProfileIndicatorCalls = 0;
        lastProfileIndicatorSlot = -1;
        showLowBatteryCalls = 0;
        showShutdownCalls = 0;
        setSpeedVolZeroActiveCalls = 0;
        lastSpeedVolZeroActiveValue = false;
        setBleContextCalls = 0;
        lastBleContext = DisplayBleContext{};
        setBLEProxyStatusCalls = 0;
        lastBleProxyEnabled = false;
        lastBleProxyConnected = false;
        lastBleReceiving = true;
        setObdStatusCalls = 0;
        lastObdEnabled = false;
        lastObdConnected = false;
        lastObdScanAttention = false;
        setObdAttentionCalls = 0;
        lastObdAttention = false;
        syncTopIndicatorsCalls = 0;
        lastSyncTopIndicatorsNowMs = 0;
        flushRegionCalls = 0;
        lastFlushX = 0;
        lastFlushY = 0;
        lastFlushW = 0;
        lastFlushH = 0;
        showSettingsSlidersCalls = 0;
        updateSettingsSlidersCalls = 0;
        hideBrightnessSliderCalls = 0;
        lastSettingsBrightness = 0;
        lastSettingsVolume = 0;
        lastSettingsActiveSlider = -1;
        activeSliderFromTouch = -1;
        refreshFrequencyOnlyCalls = 0;
        lastFrequencyMHz = 0;
        lastFrequencyBand = 0;
        lastFrequencyMuted = false;
        lastFrequencyPhotoRadar = false;
        lastAlertUpdateCount = 0;
        refreshSecondaryAlertCardsCalls = 0;
        lastSecondaryAlertCount = 0;
        lastSecondaryMuted = false;
        resetChangeTrackingCalls = 0;
    }

    // Display methods
    void showScanning() { showScanningCalls++; }
    void showResting() { showRestingCalls++; }
    void showDisconnected() { showDisconnectedCalls++; }
    
    void update(const DisplayState& /*state*/) { updateCalls++; }
    void update(const AlertData& /*priority*/, const AlertData* /*alerts*/,
                int count, const DisplayState& /*state*/) {
        updateCalls++;
        lastAlertUpdateCount = count;
    }
    void updatePersisted(const AlertData& /*alert*/, const DisplayState& /*state*/) { 
        updatePersistedCalls++; 
    }
    
    void flush() { flushCalls++; }
    void forceNextRedraw() { forceNextRedrawCalls++; }
    
    void drawWiFiIndicator() { drawWiFiIndicatorCalls++; }
    void drawObdIndicator() { drawObdIndicatorCalls++; }
    void drawBatteryIndicator() { drawBatteryIndicatorCalls++; }
    void showLowBattery() { showLowBatteryCalls++; }
    void showShutdown() { showShutdownCalls++; }
    void drawProfileIndicator(int slot) {
        drawProfileIndicatorCalls++;
        lastProfileIndicatorSlot = slot;
    }
    
    void setSpeedVolZeroActive(bool active) {
        setSpeedVolZeroActiveCalls++;
        lastSpeedVolZeroActiveValue = active;
    }
    
    void setBleContext(const DisplayBleContext& ctx) {
        setBleContextCalls++;
        lastBleContext = ctx;
    }

    void setBLEProxyStatus(bool proxyEnabled, bool proxyConnected, bool receiving) {
        setBLEProxyStatusCalls++;
        lastBleProxyEnabled = proxyEnabled;
        lastBleProxyConnected = proxyConnected;
        lastBleReceiving = receiving;
    }

    void setObdStatus(bool enabled, bool connected, bool scanAttention = false) {
        setObdStatusCalls++;
        lastObdEnabled = enabled;
        lastObdConnected = connected;
        lastObdScanAttention = scanAttention;
    }

    void setObdAttention(bool attention) {
        setObdAttentionCalls++;
        lastObdAttention = attention;
    }

    void refreshObdIndicator(uint32_t nowMs) {
        syncTopIndicators(nowMs);
        drawObdIndicator();
    }

    void syncTopIndicators(uint32_t nowMs) {
        syncTopIndicatorsCalls++;
        lastSyncTopIndicatorsNowMs = nowMs;
    }

    void flushRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
        flushRegionCalls++;
        lastFlushX = x;
        lastFlushY = y;
        lastFlushW = w;
        lastFlushH = h;
    }

    void refreshFrequencyOnly(uint32_t freqMHz, int band, bool muted, bool isPhotoRadar = false) {
        refreshFrequencyOnlyCalls++;
        lastFrequencyMHz = freqMHz;
        lastFrequencyBand = band;
        lastFrequencyMuted = muted;
        lastFrequencyPhotoRadar = isPhotoRadar;
    }

    void refreshSecondaryAlertCards(const AlertData* /*alerts*/, int alertCount,
                                    const AlertData& /*priority*/, bool muted = false) {
        refreshSecondaryAlertCardsCalls++;
        lastSecondaryAlertCount = alertCount;
        lastSecondaryMuted = muted;
    }

    void setBrightness(uint8_t /*level*/) {}
    void showSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel) {
        showSettingsSlidersCalls++;
        lastSettingsBrightness = brightnessLevel;
        lastSettingsVolume = volumeLevel;
    }
    void updateSettingsSliders(uint8_t brightnessLevel, uint8_t volumeLevel, int activeSlider) {
        updateSettingsSlidersCalls++;
        lastSettingsBrightness = brightnessLevel;
        lastSettingsVolume = volumeLevel;
        lastSettingsActiveSlider = activeSlider;
    }
    void hideBrightnessSlider() { hideBrightnessSliderCalls++; }
    int getActiveSliderFromTouch(int16_t /*touchY*/) { return activeSliderFromTouch; }
    
    // Static methods
    static void resetChangeTracking() { resetChangeTrackingCalls++; }
};

// Definition of static member
inline int V1Display::resetChangeTrackingCalls = 0;

#endif  // DISPLAY_H
