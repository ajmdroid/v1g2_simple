#pragma once

#include <Arduino.h>
#include <vector>
#include "audio_beep.h"      // play_camera_voice
#include "camera_manager.h" // NearbyCameraResult, CameraManager
#include "display.h"        // V1Display
#include "gps_handler.h"    // GPSFix, GPSHandler
#include "settings.h"       // SettingsManager

/**
 * CameraAlertModule - owns camera alert detection, test/demo, and display wiring.
 *
 * Responsibilities:
 * - Track approaching cameras using GPS + camera database
 * - Drive camera test/demo flow (from web UI)
 * - Update camera card/main display state depending on V1 alert ownership
 *
 * Does NOT:
 * - Load camera databases (handled by main + CameraManager)
 * - Own WiFi callbacks (main wires setCameraTestCallback to startTest())
 */
class CameraAlertModule {
public:
    CameraAlertModule();

    void begin(V1Display* display,
               SettingsManager* settings,
               CameraManager* cameraMgr,
               GPSHandler* gps);

    // Process detection + cache maintenance; call once per loop.
    void process();

    // Update card slots when V1 owns the main display area.
    void updateCardStateForV1(bool v1HasAlerts);

    // Update camera main display (only when V1 does NOT have alerts).
    void updateMainDisplay(bool v1HasAlerts);

    // Camera test/demo control (invoked from WiFi API callback).
    void startTest(int type);
    bool isTestRunning() const { return cameraTestActive; }
    bool consumeTestEnded();

private:
    // Tracked state for each active camera alert (for trend-based clearing)
    struct ActiveCameraState {
        NearbyCameraResult camera;      // Current camera data
        float minDist_m = 9999.0f;      // Minimum distance achieved (for pass detection)
        float lastDist_m = 9999.0f;     // Distance at last check (for trend)
        uint8_t increasingCount = 0;    // Consecutive samples where distance increased
        unsigned long firstSeenMs = 0;  // When this camera alert started
        float headingErr_deg = 0.0f;    // Heading error to camera bearing
    };
    
    struct PassedCameraTracker {
        float lat = 0.0f;
        float lon = 0.0f;
        unsigned long passedTimeMs = 0;
    };

    struct CameraTestParams {
        const char* typeName = "CAM";
        CameraAlertType voiceType = CameraAlertType::SPEED;
    };

    bool ensureTestActive();
    void handleTestCards(const V1Settings& dispSettings);
    void handleTestDisplay(bool v1HasAlerts, const V1Settings& dispSettings);
    void handleRealCards(const V1Settings& dispSettings);
    void handleRealDisplay(bool v1HasAlerts, const V1Settings& dispSettings);
    void refreshRegionalCacheIfNeeded(unsigned long now, const V1Settings& camSettings);
    void detectApproachingCameras(unsigned long now, const V1Settings& camSettings);
    void clearActiveCamerasAndMarkPassed(unsigned long now);

    V1Display* display = nullptr;
    SettingsManager* settings = nullptr;
    CameraManager* cameraManager = nullptr;
    GPSHandler* gpsHandler = nullptr;

    // Active approaching cameras with tracking state (for trend-based clearing)
    std::vector<ActiveCameraState> activeCameras;
    std::vector<PassedCameraTracker> recentlyPassedCameras;
    
    // While-active logging throttle (1/sec max)
    unsigned long lastActiveLogMs = 0;

    // Timing + cache tracking
    unsigned long lastCameraCheckMs = 0;
    unsigned long lastCacheCheckMs = 0;
    
    // GPS ready cooldown - defer heavy operations after fix acquired
    bool wasGpsReady = false;
    unsigned long gpsReadyAtMs = 0;
    
    // Deferred cache save (non-blocking)
    unsigned long pendingSaveCacheMs = 0;

    // Test/demo state
    bool cameraTestActive = false;
    bool cameraTestEnded = false;
    unsigned long cameraTestEndMs = 0;
    int cameraTestPhase = 0;
    unsigned long cameraTestPhaseStartMs = 0;
    CameraTestParams cameraTestParams{};

    // Background load tracking (for logging)
    bool bgLoadLoggedComplete = false;

    // Safety timeout - auto-clear stale alerts
    unsigned long alertStartedAtMs = 0;

    // Constants
    static constexpr int MAX_ACTIVE_CAMERAS = 3;
    static constexpr unsigned long CAMERA_CHECK_INTERVAL_MS = 500;  // 500ms
    static constexpr float CAMERA_ALERT_COOLDOWN_M = 200.0f;        // 200m
    static constexpr unsigned long PASSED_CAMERA_MEMORY_MS = 60000; // 1 minute
    static constexpr unsigned long CAMERA_TEST_PHASE_DURATION_MS = 3000; // 3s
    static constexpr unsigned long CACHE_CHECK_INTERVAL_MS = 30000;      // 30s
    static constexpr unsigned long CACHE_REFRESH_INTERVAL_MS = 1800000;  // 30 min
    static constexpr unsigned long GPS_READY_COOLDOWN_MS = 10000;        // 10s after fix (prevents display stalls)
    static constexpr float CACHE_RADIUS_MILES = 100.0f;
    static constexpr float CACHE_REFRESH_DIST_MILES = 50.0f;
    static constexpr unsigned long ALERT_MAX_DURATION_MS = 120000;  // 2 min max alert
    
    // Trend-based clearing parameters
    static constexpr uint8_t PASS_TREND_COUNT = 3;        // N consecutive increasing distance samples
    static constexpr float PASS_MIN_THRESHOLD_M = 50.0f;  // Must get within this distance to be "passed"
    static constexpr float PASS_HYSTERESIS_M = 5.0f;      // Distance must increase by this to count as increasing
};
