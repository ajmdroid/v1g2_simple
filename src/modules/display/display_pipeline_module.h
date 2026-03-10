#pragma once

#include <Arduino.h>
#include <vector>

#include "display.h"
#include "display_mode.h"
#include "packet_parser.h"
#include "settings.h"
#include "ble_client.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/voice/voice_module.h"

class GpsRuntimeModule;
class CameraAlertModule;

class DisplayPipelineModule {
public:
    void begin(DisplayMode* displayModePtr,
               V1Display* displayPtr,
               PacketParser* parserPtr,
               SettingsManager* settingsMgr,
               V1BLEClient* bleClient,
               AlertPersistenceModule* alertPersistenceModule,
               VolumeFadeModule* volumeFadeModule,
               VoiceModule* voiceModule,
               GpsRuntimeModule* gpsModule = nullptr,
               CameraAlertModule* cameraModule = nullptr);

    // Process after a successful parser.parse(); expects parser state already updated.
    // prioritySuppressed is a per-frame software suppression flag (e.g. lockout ENFORCE match).
    void handleParsed(unsigned long nowMs, bool prioritySuppressed);
    bool isCameraAlertActive() const;
    bool debugRenderCameraPayload(uint32_t nowMs,
                                  const CameraAlertDisplayPayload& payload,
                                  uint32_t holdMs = 0);
    void clearDebugCameraOverride();
    void restoreCurrentOwner(uint32_t nowMs);

private:
    enum class RenderOwner : uint8_t {
        Unknown = 0,
        Scanning,
        Live,
        Persisted,
        Camera,
        Resting
    };

    DisplayMode* displayMode = nullptr;
    V1Display* display = nullptr;
    PacketParser* parser = nullptr;
    SettingsManager* settings = nullptr;
    V1BLEClient* ble = nullptr;
    AlertPersistenceModule* alertPersistence = nullptr;
    VolumeFadeModule* volumeFade = nullptr;
    VoiceModule* voice = nullptr;
    GpsRuntimeModule* gpsModule = nullptr;
    CameraAlertModule* cameraModule = nullptr;
    bool cameraAlertActive_ = false;

    // Mute debounce
    bool debouncedMuteState = false;
    unsigned long lastMuteChangeMs = 0;
    static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;

    // Display throttling
    unsigned long lastDisplayDraw = 0;
    static constexpr unsigned long DISPLAY_DRAW_MIN_MS = 25;

    // Alert gap recovery
    unsigned long displayLatencySum = 0;
    unsigned long displayLatencyCount = 0;
    unsigned long displayLatencyMax = 0;
    unsigned long displayLatencyLastLog = 0;
    static constexpr unsigned long DISPLAY_LOG_INTERVAL_MS = 10000;
    static constexpr bool PERF_TIMING_LOGS = false;
    unsigned long perfTimingAccum = 0;
    unsigned long perfTimingCount = 0;
    unsigned long perfTimingMax = 0;
    unsigned long perfLastReport = 0;
    int lastPersistenceSlot = -1;
    RenderOwner lastRenderedOwner_ = RenderOwner::Unknown;
    bool debugCameraOverrideEnabled_ = false;
    uint32_t debugCameraOverrideUntilMs_ = 0;
    CameraAlertDisplayPayload debugCameraPayload_{};
    CameraAlertDisplayPayload lastDebugCameraRenderedPayload_{};
    char lastDebugCameraRenderedBogeyChar_ = 0;
    bool lastDebugCameraRenderedBogeyDot_ = false;
    uint32_t lastDebugCameraRenderMs_ = 0;
    bool debugCameraFrameValid_ = false;
    static constexpr uint32_t DEBUG_CAMERA_REDRAW_MIN_MS = 1000;

    void recordDisplayTiming(const char* label, unsigned long startUs, unsigned long endUs);
    void recordPerfTiming(const char* label, unsigned long startUs, unsigned long endUs);
    void processCameraState(uint32_t nowMs);
    void dispatchCameraVoice();
    bool debugCameraOverrideActiveAt(uint32_t nowMs) const;
    void renderIdleOwner(uint32_t nowMs, const DisplayState& state, bool forceRedraw);
};
