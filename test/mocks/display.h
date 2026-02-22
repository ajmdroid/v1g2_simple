// Mock display.h for native unit testing
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
    int updateCameraAlertCalls = 0;
    int flushCalls = 0;
    int forceNextRedrawCalls = 0;
    int drawWiFiIndicatorCalls = 0;
    int drawBatteryIndicatorCalls = 0;
    int drawProfileIndicatorCalls = 0;
    int setLockoutMutedCalls = 0;
    bool lastLockoutMutedValue = false;
    int setLockoutIndicatorCalls = 0;
    bool lastLockoutIndicatorValue = false;
    int setPreQuietActiveCalls = 0;
    bool lastPreQuietActiveValue = false;
    int setBleContextCalls = 0;
    DisplayBleContext lastBleContext{};
    int setBLEProxyStatusCalls = 0;
    bool lastBleProxyEnabled = false;
    bool lastBleProxyConnected = false;
    bool lastBleReceiving = true;
    int setGpsSatellitesCalls = 0;
    bool lastGpsEnabled = false;
    bool lastGpsHasFix = false;
    uint8_t lastGpsSatellites = 0;
    int setObdConnectedCalls = 0;
    bool lastObdEnabled = false;
    bool lastObdConnected = false;
    bool lastObdHasData = false;
    int refreshFrequencyOnlyCalls = 0;
    uint32_t lastFrequencyMHz = 0;
    int lastFrequencyBand = 0;
    bool lastFrequencyMuted = false;
    bool lastFrequencyPhotoRadar = false;
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
        updateCameraAlertCalls = 0;
        flushCalls = 0;
        forceNextRedrawCalls = 0;
        drawWiFiIndicatorCalls = 0;
        drawBatteryIndicatorCalls = 0;
        drawProfileIndicatorCalls = 0;
        setLockoutMutedCalls = 0;
        lastLockoutMutedValue = false;
        setLockoutIndicatorCalls = 0;
        lastLockoutIndicatorValue = false;
        setPreQuietActiveCalls = 0;
        lastPreQuietActiveValue = false;
        setBleContextCalls = 0;
        lastBleContext = DisplayBleContext{};
        setBLEProxyStatusCalls = 0;
        lastBleProxyEnabled = false;
        lastBleProxyConnected = false;
        lastBleReceiving = true;
        setGpsSatellitesCalls = 0;
        lastGpsEnabled = false;
        lastGpsHasFix = false;
        lastGpsSatellites = 0;
        setObdConnectedCalls = 0;
        lastObdEnabled = false;
        lastObdConnected = false;
        lastObdHasData = false;
        refreshFrequencyOnlyCalls = 0;
        lastFrequencyMHz = 0;
        lastFrequencyBand = 0;
        lastFrequencyMuted = false;
        lastFrequencyPhotoRadar = false;
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
                int /*count*/, const DisplayState& /*state*/) { updateCalls++; }
    void updatePersisted(const AlertData& /*alert*/, const DisplayState& /*state*/) { 
        updatePersistedCalls++; 
    }
    void updateCameraAlert(uint8_t /*cameraType*/, bool /*muted*/ = false) {
        updateCameraAlertCalls++;
    }
    
    void flush() { flushCalls++; }
    void forceNextRedraw() { forceNextRedrawCalls++; }
    
    void drawWiFiIndicator() { drawWiFiIndicatorCalls++; }
    void drawBatteryIndicator() { drawBatteryIndicatorCalls++; }
    void drawProfileIndicator(int /*slot*/) { drawProfileIndicatorCalls++; }
    
    void setLockoutMuted(bool muted) { 
        setLockoutMutedCalls++; 
        lastLockoutMutedValue = muted;
    }

    void setLockoutIndicator(bool show) {
        setLockoutIndicatorCalls++;
        lastLockoutIndicatorValue = show;
    }

    void setPreQuietActive(bool active) {
        setPreQuietActiveCalls++;
        lastPreQuietActiveValue = active;
    }
    
    void setBleContext(const DisplayBleContext& ctx) {
        setBleContextCalls++;
        lastBleContext = ctx;
    }

    const DisplayBleContext& getBleContext() const {
        return lastBleContext;
    }

    void setBLEProxyStatus(bool proxyEnabled, bool proxyConnected, bool receiving) {
        setBLEProxyStatusCalls++;
        lastBleProxyEnabled = proxyEnabled;
        lastBleProxyConnected = proxyConnected;
        lastBleReceiving = receiving;
    }

    void setGpsSatellites(bool enabled, bool hasFix, uint8_t satellites) {
        setGpsSatellitesCalls++;
        lastGpsEnabled = enabled;
        lastGpsHasFix = hasFix;
        lastGpsSatellites = satellites;
    }

    void setObdConnected(bool enabled, bool connected, bool hasData) {
        setObdConnectedCalls++;
        lastObdEnabled = enabled;
        lastObdConnected = connected;
        lastObdHasData = hasData;
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
    
    // Static methods
    static void resetChangeTracking() { resetChangeTrackingCalls++; }
};

// Definition of static member
inline int V1Display::resetChangeTrackingCalls = 0;

#endif  // DISPLAY_H
