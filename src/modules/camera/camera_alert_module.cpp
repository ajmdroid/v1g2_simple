#include "camera_alert_module.h"

#include <algorithm>
#include <LittleFS.h>

#include "../../debug_logger.h"
#include "../../perf_metrics.h"

// Camera alert logging macro - logs to SD when category enabled
#define CAMERA_LOG(...) do { \
    Serial.printf(__VA_ARGS__); \
    if (debugLogger.isEnabledFor(DebugLogCategory::Camera)) debugLogger.logf(DebugLogCategory::Camera, __VA_ARGS__); \
} while(0)

CameraAlertModule::CameraAlertModule() {}

void CameraAlertModule::begin(V1Display* disp,
                              SettingsManager* settingsMgr,
                              CameraManager* cameraMgr,
                              GPSHandler* gps) {
    display = disp;
    settings = settingsMgr;
    cameraManager = cameraMgr;
    gpsHandler = gps;
    
    // Pre-allocate scratch vectors to avoid heap fragmentation during detection loops.
    // These are cleared (but not deallocated) each cycle, so capacity stays reserved.
    scratchNearbyCameras.reserve(MAX_ACTIVE_CAMERAS + 4);
    scratchApproachingCameras.reserve(MAX_ACTIVE_CAMERAS + 2);
    scratchUpdatedActive.reserve(MAX_ACTIVE_CAMERAS);
    activeCameras.reserve(MAX_ACTIVE_CAMERAS);
    recentlyPassedCameras.reserve(8);  // Track a few recently passed cameras
}

bool CameraAlertModule::consumeTestEnded() {
    if (cameraTestEnded) {
        cameraTestEnded = false;
        return true;
    }
    return false;
}

void CameraAlertModule::startTest(int type) {
    if (!settings || !display) return;

    // Map type param to display + voice
    CameraTestParams params{};
    switch (type) {
        case 0:
            params.typeName = "REDLIGHT";
            params.voiceType = CameraAlertType::RED_LIGHT;
            break;
        case 1:
            params.typeName = "SPEED";
            params.voiceType = CameraAlertType::SPEED;
            break;
        case 2:
            params.typeName = "ALPR";
            params.voiceType = CameraAlertType::ALPR;
            break;
        case 3:
            params.typeName = "RLS";
            params.voiceType = CameraAlertType::RED_LIGHT_SPEED;
            break;
        default:
            params.typeName = "CAM";
            params.voiceType = CameraAlertType::SPEED;
            break;
    }

    cameraTestParams = params;
    cameraTestActive = true;
    cameraTestEnded = false;
    cameraTestEndMs = millis() + 9000;  // 3 phases × 3 seconds each
    cameraTestPhase = 0;
    cameraTestPhaseStartMs = millis();

    play_camera_voice(params.voiceType);

    CAMERA_LOG("[Camera] Test alert: %s - cycling 1→2→3 cameras over 9s\n", params.typeName);
}

bool CameraAlertModule::ensureTestActive() {
    if (cameraTestActive && millis() >= cameraTestEndMs) {
        cameraTestActive = false;
        cameraTestEnded = true;  // Signal caller to restore display
        cameraTestPhase = 0;
        if (display) {
            display->clearCameraAlerts();
        }
        CAMERA_LOG("[Camera] Test alert ended\n");
    }
    return cameraTestActive;
}

void CameraAlertModule::process() {
    if (!settings || !cameraManager) return;

    unsigned long now = millis();

    // Monitor background load progress (log + cache rebuild once complete)
    if (cameraManager->isBackgroundLoading()) {
        static unsigned long lastProgressLog = 0;
        if (millis() - lastProgressLog > 10000) {  // Every 10 seconds
            CAMERA_LOG("[Camera] Background load: %d%% (%d cameras)\n",
                       cameraManager->getLoadProgress(), cameraManager->getLoadedCount());
            lastProgressLog = millis();
        }
        bgLoadLoggedComplete = false;
    } else if (!bgLoadLoggedComplete && cameraManager->getCameraCount() > 0) {
        bgLoadLoggedComplete = true;
        PERF_INC(cameraBgLoads);
        CAMERA_LOG("[Camera] Background load complete: %d cameras ready\n",
                   cameraManager->getCameraCount());
        // Cache rebuild deferred to refreshRegionalCacheIfNeeded (respects GPS cooldown)
    }

    const V1Settings& camSettings = settings->get();
    if (!camSettings.cameraAlertsEnabled || !cameraManager->isLoaded() || !gpsHandler || !gpsHandler->isEnabled()) {
        // Feature off or GPS unavailable: clear any stale camera UI/state
        if (!activeCameras.empty()) {
            CAMERA_LOG("[Camera] Feature disabled/GPS unavail - clearing %d active alerts (enabled=%d loaded=%d gps=%d)\n",
                       (int)activeCameras.size(), camSettings.cameraAlertsEnabled, 
                       cameraManager->isLoaded(), gpsHandler && gpsHandler->isEnabled());
        }
        activeCameras.clear();
        recentlyPassedCameras.clear();
        if (display) {
            display->clearCameraAlerts();
        }
        return;
    }

    refreshRegionalCacheIfNeeded(now, camSettings);
    detectApproachingCameras(now, camSettings);
}

void CameraAlertModule::clearActiveCamerasAndMarkPassed(unsigned long now) {
    // Mark all active cameras as passed so they don't re-alert if we return
    for (const auto& state : activeCameras) {
        PassedCameraTracker passed;
        passed.lat = state.camera.camera.latitude;
        passed.lon = state.camera.camera.longitude;
        passed.passedTimeMs = now;
        recentlyPassedCameras.push_back(passed);
    }
    activeCameras.clear();
    alertStartedAtMs = 0;
    if (display) {
        display->clearCameraAlerts();
    }
}

void CameraAlertModule::refreshRegionalCacheIfNeeded(unsigned long now, const V1Settings& camSettings) {
    if (!gpsHandler) return;

    // Track GPS ready transition for cooldown
    bool gpsReady = gpsHandler->isReadyForNavigation();
    if (gpsReady && !wasGpsReady) {
        // GPS just became ready - start cooldown
        gpsReadyAtMs = now;
        CAMERA_LOG("[Camera] GPS ready - deferring cache operations for 3s\n");
    }
    wasGpsReady = gpsReady;

    if (!gpsReady) return;

    // Defer heavy operations during GPS ready cooldown (display lag prevention)
    if (gpsReadyAtMs > 0 && (now - gpsReadyAtMs) < GPS_READY_COOLDOWN_MS) {
        return;
    }

    // Continue any in-progress incremental cache build (non-blocking)
    if (cameraManager->isIncrementalBuildInProgress()) {
        // Process a batch of cameras each call - keeps main loop responsive
        if (cameraManager->continueIncrementalCacheBuild(500)) {
            // Build complete - finalize the cache
            cameraManager->finishIncrementalCacheBuild();
            PERF_INC(cameraCacheRefreshes);
            // Defer save to next idle period to avoid blocking
            pendingSaveCacheMs = now;
        }
        return;  // Don't check for new cache needs while building
    }

    // Handle deferred cache save (non-blocking, spread work over time)
    if (pendingSaveCacheMs > 0 && (now - pendingSaveCacheMs) >= 100) {
        cameraManager->saveRegionalCache(&LittleFS, "/cameras_cache.json");
        pendingSaveCacheMs = 0;
    }

    if (now - lastCacheCheckMs < CACHE_CHECK_INTERVAL_MS || cameraManager->isBackgroundLoading()) {
        return;
    }
    lastCacheCheckMs = now;

    GPSFix cacheFix = gpsHandler->getFix();
    bool needsRefresh = cameraManager->needsCacheRefresh(cacheFix.latitude, cacheFix.longitude, CACHE_REFRESH_DIST_MILES);

    // Also refresh if cache is older than 30 minutes
    if (!needsRefresh && cameraManager->hasRegionalCache()) {
        static unsigned long lastCacheRefreshMs = 0;
        if (lastCacheRefreshMs == 0) lastCacheRefreshMs = now;
        if (now - lastCacheRefreshMs >= CACHE_REFRESH_INTERVAL_MS) {
            needsRefresh = true;
            lastCacheRefreshMs = now;
        }
    }

    if (needsRefresh) {
        CAMERA_LOG("[Camera] Starting incremental cache build at %.4f, %.4f\n", cacheFix.latitude, cacheFix.longitude);
        cameraManager->startIncrementalCacheBuild(cacheFix.latitude, cacheFix.longitude, CACHE_RADIUS_MILES);
    }
}

void CameraAlertModule::detectApproachingCameras(unsigned long now, const V1Settings& camSettings) {
    if (!gpsHandler || !gpsHandler->isReadyForNavigation()) {
        // Safety: clear stale alerts if GPS lost for too long
        if (!activeCameras.empty() && alertStartedAtMs > 0 &&
            (now - alertStartedAtMs) > ALERT_MAX_DURATION_MS) {
            CAMERA_LOG("[Camera] CLEAR reason=gps_lost alerts=%d\n", (int)activeCameras.size());
            activeCameras.clear();
            alertStartedAtMs = 0;
            if (display) {
                display->clearCameraAlerts();
            }
        }
        return;
    }

    GPSFix fix = gpsHandler->getFix();
    float speed_mps = fix.speed_mps;
    
    // Speed validity: use last valid speed during GPS dropout (up to 3s)
    if (speed_mps > 0.1f) {
        lastValidSpeed_mps = speed_mps;
        lastValidSpeedMs = now;
    } else if ((now - lastValidSpeedMs) < SPEED_VALID_HOLDOVER_MS) {
        speed_mps = lastValidSpeed_mps;  // Use cached speed during dropout
    }
    
    // Speed gating with hysteresis (prevents flapping at threshold)
    // Fast mode: enter above 42 mph, exit below 37 mph
    if (inFastScanMode) {
        if (speed_mps < SLOW_MODE_ENTER_MPS) {
            inFastScanMode = false;
        }
    } else {
        if (speed_mps > FAST_MODE_ENTER_MPS) {
            inFastScanMode = true;
        }
    }
    
    unsigned long checkInterval = inFastScanMode 
        ? CAMERA_CHECK_INTERVAL_MS 
        : CAMERA_CHECK_INTERVAL_SLOW_MS;
    
    if (now - lastCameraCheckMs < checkInterval) return;
    lastCameraCheckMs = now;

    float lat = fix.latitude;
    float lon = fix.longitude;
    float heading = fix.heading_deg;
    float alertRadius = static_cast<float>(camSettings.cameraAlertDistanceM);

    // Safety check: if we have active alerts but NO cameras nearby at all
    // (even with 2x search radius), clear immediately - we've left the area
    if (!activeCameras.empty()) {
        bool anyCamerasNearby = cameraManager->hasNearbyCamera(lat, lon, alertRadius * 2.0f);
        if (!anyCamerasNearby) {
            CAMERA_LOG("[Camera] CLEAR reason=no_cameras_in_area alerts=%d\n", (int)activeCameras.size());
            clearActiveCamerasAndMarkPassed(now);
            return;
        }
    }

    // Also enforce max duration as final safety (shouldn't normally trigger)
    if (!activeCameras.empty() && alertStartedAtMs > 0 &&
        (now - alertStartedAtMs) > ALERT_MAX_DURATION_MS) {
        CAMERA_LOG("[Camera] CLEAR reason=max_duration alerts=%d\n", (int)activeCameras.size());
        clearActiveCamerasAndMarkPassed(now);
    }

    // Clean up old "passed camera" entries
    recentlyPassedCameras.erase(
        std::remove_if(recentlyPassedCameras.begin(), recentlyPassedCameras.end(),
                       [now](const PassedCameraTracker& p) { return (now - p.passedTimeMs) > PASSED_CAMERA_MEMORY_MS; }),
        recentlyPassedCameras.end());

    // Update camera type filters from settings
    cameraManager->setEnabledTypes(
        camSettings.cameraAlertRedLight,
        camSettings.cameraAlertSpeed,
        camSettings.cameraAlertALPR);

    // Find all nearby cameras (sorted by distance, approaching first)
    // Uses output parameter to avoid heap allocation - scratch vector is reused
    // Budget guard: stop early if scan takes too long (protects Core 1 latency)
    cameraManager->findNearby(
        lat, lon, heading, alertRadius, MAX_ACTIVE_CAMERAS + 2,
        scratchNearbyCameras, CAMERA_BUDGET_US);  // Get a few extra for filtering

    // Filter: only keep approaching cameras, exclude recently passed
    // Apply tighter heading gate when we have reliable heading (speed > 2 m/s)
    const float headingGateThreshold = 45.0f;  // CT recommendation: tighter than 60° default
    scratchApproachingCameras.clear();
    for (const auto& cam : scratchNearbyCameras) {
        if (!cam.isApproaching) continue;
        
        // Tighter heading filter when moving (heading is reliable at speed)
        if (speed_mps > 2.0f) {
            float headingErr = fabs(heading - cam.bearing_deg);
            if (headingErr > 180.0f) headingErr = 360.0f - headingErr;
            if (headingErr > headingGateThreshold) {
                CAMERA_LOG("[Camera] SKIP cam=%s dist=%.0f headingErr=%.0f > %.0f (heading gate)\n",
                           cam.camera.getTypeName(), cam.distance_m, headingErr, headingGateThreshold);
                continue;
            }
        }

        // Check if this camera was recently passed (don't re-alert)
        bool recentlyPassed = false;
        for (const auto& passed : recentlyPassedCameras) {
            float dist = CameraManager::haversineDistance(
                cam.camera.latitude, cam.camera.longitude,
                passed.lat, passed.lon);
            if (dist < CAMERA_ALERT_COOLDOWN_M) {
                recentlyPassed = true;
                break;
            }
        }
        if (recentlyPassed) continue;

        scratchApproachingCameras.push_back(cam);
        if ((int)scratchApproachingCameras.size() >= MAX_ACTIVE_CAMERAS) break;
    }

    // ========== TREND-BASED CLEARING ==========
    // Update existing active cameras and check for pass/clear conditions
    scratchUpdatedActive.clear();
    
    for (auto& state : activeCameras) {
        float currentDist = CameraManager::haversineDistance(
            lat, lon, state.camera.camera.latitude, state.camera.camera.longitude);
        float dDist = currentDist - state.lastDist_m;
        
        // Calculate heading error to camera
        float bearing = CameraManager::calculateBearing(lat, lon, 
            state.camera.camera.latitude, state.camera.camera.longitude);
        float headingErr = fabs(heading - bearing);
        if (headingErr > 180.0f) headingErr = 360.0f - headingErr;
        state.headingErr_deg = headingErr;
        
        // Update min distance
        if (currentDist < state.minDist_m) {
            state.minDist_m = currentDist;
        }
        
        // Check for increasing distance (with hysteresis)
        if (dDist > PASS_HYSTERESIS_M) {
            state.increasingCount++;
        } else if (dDist < -PASS_HYSTERESIS_M) {
            state.increasingCount = 0;  // Reset if distance decreasing
        }
        // Flat distance (within hysteresis) keeps count unchanged
        
        state.lastDist_m = currentDist;
        state.camera.distance_m = currentDist;  // Update for display
        
        // ========== CHECK CLEAR CONDITIONS ==========
        bool shouldClear = false;
        const char* clearReason = nullptr;
        
        // Condition 1: PASSED - got close and now moving away
        if (state.increasingCount >= PASS_TREND_COUNT && state.minDist_m < PASS_MIN_THRESHOLD_M) {
            shouldClear = true;
            clearReason = "passed";
        }
        
        // Condition 2: DIVERGED - heading no longer towards camera (>90 degrees)
        // Only apply when we have reliable heading (speed > 2 m/s ≈ 4.5 mph)
        if (!shouldClear && speed_mps > 2.0f && headingErr > 90.0f) {
            shouldClear = true;
            clearReason = "heading_mismatch";
        }
        
        // Condition 3: DISTANCE - too far away (with trend confirmation)
        if (!shouldClear && currentDist > alertRadius && state.increasingCount >= 2) {
            shouldClear = true;
            clearReason = "out_of_range";
        }
        
        if (shouldClear) {
            // Log transition: camera cleared
            CAMERA_LOG("[Camera] CLEAR cam=%s dist=%.0f minDist=%.0f headingErr=%.0f reason=%s\n",
                       state.camera.camera.getTypeName(), currentDist, state.minDist_m, 
                       headingErr, clearReason);
            
            // Mark as passed to prevent re-alert
            PassedCameraTracker passed;
            passed.lat = state.camera.camera.latitude;
            passed.lon = state.camera.camera.longitude;
            passed.passedTimeMs = now;
            recentlyPassedCameras.push_back(passed);
        } else {
            scratchUpdatedActive.push_back(state);
        }
    }

    // ========== ADD NEW CAMERAS ==========
    for (const auto& newCam : scratchApproachingCameras) {
        // Check if already in active list
        bool alreadyActive = false;
        for (const auto& state : scratchUpdatedActive) {
            float dist = CameraManager::haversineDistance(
                state.camera.camera.latitude, state.camera.camera.longitude,
                newCam.camera.latitude, newCam.camera.longitude);
            if (dist < 50.0f) {
                alreadyActive = true;
                break;
            }
        }
        
        if (!alreadyActive && (int)scratchUpdatedActive.size() < MAX_ACTIVE_CAMERAS) {
            // New camera entering alert range
            ActiveCameraState newState;
            newState.camera = newCam;
            newState.minDist_m = newCam.distance_m;
            newState.lastDist_m = newCam.distance_m;
            newState.increasingCount = 0;
            newState.firstSeenMs = now;
            
            // Calculate heading error
            float bearing = CameraManager::calculateBearing(lat, lon, 
                newCam.camera.latitude, newCam.camera.longitude);
            float headingErr = fabs(heading - bearing);
            if (headingErr > 180.0f) headingErr = 360.0f - headingErr;
            newState.headingErr_deg = headingErr;
            
            bool isPrimary = scratchUpdatedActive.empty();
            
            // Log transition: camera selected
            CAMERA_LOG("[Camera] SELECT cam=%s dist=%.0f headingErr=%.0f speed=%.1f %s\n",
                       newCam.camera.getTypeName(), newCam.distance_m, headingErr, speed_mps,
                       isPrimary ? "PRIMARY" : "SECONDARY");

            // Play voice alert only for new primary camera
            if (isPrimary && camSettings.cameraAudioEnabled) {
                CameraAlertType voiceType = CameraAlertType::SPEED;
                CameraType camType = newCam.camera.getCameraType();
                switch (camType) {
                    case CameraType::RedLightCamera:
                        voiceType = CameraAlertType::RED_LIGHT;
                        break;
                    case CameraType::RedLightAndSpeed:
                        voiceType = CameraAlertType::RED_LIGHT_SPEED;
                        break;
                    case CameraType::SpeedCamera:
                        voiceType = CameraAlertType::SPEED;
                        break;
                    case CameraType::ALPR:
                        voiceType = CameraAlertType::ALPR;
                        break;
                    default:
                        voiceType = CameraAlertType::SPEED;
                        break;
                }
                play_camera_voice(voiceType);
            }
            
            scratchUpdatedActive.push_back(newState);
        }
    }

    // Update active camera list
    bool wasEmpty = activeCameras.empty();
    activeCameras = scratchUpdatedActive;
    
    // Track when alerts started (for safety timeout)
    if (wasEmpty && !activeCameras.empty()) {
        alertStartedAtMs = now;  // Alerts just started
    } else if (activeCameras.empty()) {
        alertStartedAtMs = 0;    // No alerts, reset timer
    }
    
    // ========== WHILE-ACTIVE LOGGING (1/sec) ==========
    if (!activeCameras.empty() && (now - lastActiveLogMs) >= 1000) {
        lastActiveLogMs = now;
        for (const auto& state : activeCameras) {
            float dDist = state.camera.distance_m - state.lastDist_m;
            CAMERA_LOG("[Camera] ACTIVE cam=%s dist=%.0f dDist=%.0f minDist=%.0f headingErr=%.0f incr=%d spd=%.1f\n",
                       state.camera.camera.getTypeName(), state.camera.distance_m, dDist,
                       state.minDist_m, state.headingErr_deg, state.increasingCount, speed_mps);
        }
    }

    // Debug: log active camera count changes
    static int lastCameraCount = 0;
    if ((int)activeCameras.size() != lastCameraCount) {
        CAMERA_LOG("[Camera] Active cameras: %d\n", (int)activeCameras.size());
        lastCameraCount = (int)activeCameras.size();
    }
}

void CameraAlertModule::updateCardStateForV1(bool v1HasAlerts) {
    if (!display || !settings) return;
    if (!v1HasAlerts) return;  // Cards only when V1 owns main display

    const V1Settings& dispSettings = settings->get();

    if (ensureTestActive()) {
        handleTestCards(dispSettings);
        return;
    }

    if (!activeCameras.empty()) {
        handleRealCards(dispSettings);
    } else {
        display->clearAllCameraAlerts();
    }
}

void CameraAlertModule::updateMainDisplay(bool v1HasAlerts) {
    if (!display || !settings) return;
    
    // Track why clear might not fire - helps diagnose "doesn't clear" issues
    static bool hadCamerasWhenV1Active = false;
    
    if (v1HasAlerts) {
        // V1 owns display when alerts are active - log this condition
        static bool lastV1HasAlerts = false;
        if (!lastV1HasAlerts) {
            CAMERA_LOG("[Camera] V1 has alerts - deferring camera display to cards\n");
        }
        lastV1HasAlerts = v1HasAlerts;
        // Track if cameras existed while V1 owned display
        if (!activeCameras.empty()) {
            hadCamerasWhenV1Active = true;
        }
        return;
    }

    const V1Settings& dispSettings = settings->get();

    if (ensureTestActive()) {
        handleTestDisplay(v1HasAlerts, dispSettings);
        return;
    }

    // Track state transitions to avoid spammy logging
    static bool hadCameras = false;
    
    if (!activeCameras.empty()) {
        if (!hadCameras) {
            hadCameras = true;  // Cameras appeared
        }
        hadCamerasWhenV1Active = false;  // Reset - we're now handling cameras
        handleRealDisplay(v1HasAlerts, dispSettings);
    } else {
        // Log diagnostic when transitioning to zero cameras
        if (hadCameras) {
            CAMERA_LOG("[Camera] No active cameras - clearing display\n");
            hadCameras = false;
        } else if (hadCamerasWhenV1Active) {
            // Cameras appeared and disappeared while V1 owned display
            CAMERA_LOG("[Camera] Clear after V1 release (cameras passed during V1 alert)\n");
            hadCamerasWhenV1Active = false;
        }
        display->clearCameraAlerts();
    }
}

void CameraAlertModule::handleTestCards(const V1Settings& dispSettings) {
    unsigned long elapsed = millis() - cameraTestPhaseStartMs;
    int newPhase = (elapsed / CAMERA_TEST_PHASE_DURATION_MS) % 3;
    if (newPhase != cameraTestPhase) {
        cameraTestPhase = newPhase;
        CAMERA_LOG("[Camera] Test phase: %d camera(s)\n", cameraTestPhase + 1);
    }

    int numTestCameras = cameraTestPhase + 1;

    static const char* secondaryTypes[] = {"SPEED", "ALPR"};
    static const float baseDistances[] = {500.0f, 800.0f, 1200.0f};
    float dist0 = baseDistances[0] - (elapsed * 0.01f);
    float dist1 = baseDistances[1] - (elapsed * 0.01f);
    float dist2 = baseDistances[2] - (elapsed * 0.01f);
    if (dist0 < 50.0f) dist0 = 50.0f;
    if (dist1 < 50.0f) dist1 = 50.0f;
    if (dist2 < 50.0f) dist2 = 50.0f;

    if (numTestCameras >= 1) {
        display->setCameraAlertState(0, true, cameraTestParams.typeName, dist0, dispSettings.colorCameraAlert);
    } else {
        display->setCameraAlertState(0, false, "", 0, 0);
    }
    if (numTestCameras >= 2) {
        display->setCameraAlertState(1, true, secondaryTypes[0], dist1, dispSettings.colorCameraAlert);
    } else {
        display->setCameraAlertState(1, false, "", 0, 0);
    }
    // Note: 3rd camera can't show as card (only 2 slots)
}

void CameraAlertModule::handleTestDisplay(bool v1HasAlerts, const V1Settings& dispSettings) {
    unsigned long elapsed = millis() - cameraTestPhaseStartMs;
    int numCameras = ((elapsed / CAMERA_TEST_PHASE_DURATION_MS) % 3) + 1;

    if (v1HasAlerts) {
        // Cards handled separately when V1 has alerts
        return;
    }

    static const char* secondaryTypes[] = {"SPEED", "ALPR", "RED LIGHT"};
    static const float baseDistances[] = {500.0f, 800.0f, 1200.0f};

    V1Display::CameraAlertInfo camInfos[3];
    camInfos[0].typeName = cameraTestParams.typeName;
    camInfos[0].distance_m = baseDistances[0] - (elapsed * 0.01f);
    if (camInfos[0].distance_m < 50.0f) camInfos[0].distance_m = 50.0f;
    camInfos[0].color = dispSettings.colorCameraAlert;

    for (int i = 1; i < numCameras; i++) {
        camInfos[i].typeName = secondaryTypes[i - 1];
        camInfos[i].distance_m = baseDistances[i] - (elapsed * 0.01f);
        if (camInfos[i].distance_m < 50.0f) camInfos[i].distance_m = 50.0f;
        camInfos[i].color = dispSettings.colorCameraAlert;
    }

    display->updateCameraAlerts(camInfos, numCameras, v1HasAlerts);
}

void CameraAlertModule::handleRealCards(const V1Settings& dispSettings) {
    // V1 has alerts: cameras show as cards (max 2 slots)
    for (int i = 0; i < 2; i++) {
        if (i < (int)activeCameras.size()) {
            display->setCameraAlertState(i, true,
                                         activeCameras[i].camera.camera.getShortTypeName(),
                                         activeCameras[i].camera.distance_m,
                                         dispSettings.colorCameraAlert);
        } else {
            display->setCameraAlertState(i, false, "", 0, 0);
        }
    }
}

void CameraAlertModule::handleRealDisplay(bool v1HasAlerts, const V1Settings& dispSettings) {
    int count = std::min((int)activeCameras.size(), MAX_ACTIVE_CAMERAS);
    if (count == 0) {
        // This shouldn't happen - caller checks activeCameras.empty()
        display->clearCameraAlerts();
        return;
    }

    V1Display::CameraAlertInfo camInfos[MAX_ACTIVE_CAMERAS];
    for (int i = 0; i < count; i++) {
        camInfos[i].typeName = activeCameras[i].camera.camera.getTypeName();
        camInfos[i].distance_m = activeCameras[i].camera.distance_m;
        camInfos[i].color = dispSettings.colorCameraAlert;
    }

    display->updateCameraAlerts(camInfos, count, v1HasAlerts);
}
