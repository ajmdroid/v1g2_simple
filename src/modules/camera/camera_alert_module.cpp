#include "camera_alert_module.h"

#include <algorithm>
#include <LittleFS.h>

#include "../../perf_metrics.h"

CameraAlertModule::CameraAlertModule() {}

void CameraAlertModule::begin(V1Display* disp,
                              SettingsManager* settingsMgr,
                              CameraManager* cameraMgr,
                              GPSHandler* gps) {
    display = disp;
    settings = settingsMgr;
    cameraManager = cameraMgr;
    gpsHandler = gps;
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

    Serial.printf("[Camera] Test alert: %s - cycling 1→2→3 cameras over 9s\n", params.typeName);
}

bool CameraAlertModule::ensureTestActive() {
    if (cameraTestActive && millis() >= cameraTestEndMs) {
        cameraTestActive = false;
        cameraTestEnded = true;  // Signal caller to restore display
        cameraTestPhase = 0;
        if (display) {
            display->clearAllCameraAlerts();
            display->clearCameraAlerts();
        }
        Serial.println("[Camera] Test alert ended");
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
            Serial.printf("[Camera] Background load: %d%% (%d cameras)\n",
                          cameraManager->getLoadProgress(), cameraManager->getLoadedCount());
            lastProgressLog = millis();
        }
        bgLoadLoggedComplete = false;
    } else if (!bgLoadLoggedComplete && cameraManager->getCameraCount() > 0) {
        bgLoadLoggedComplete = true;
        PERF_INC(cameraBgLoads);
        Serial.printf("[Camera] Background load complete: %d cameras ready\n",
                      cameraManager->getCameraCount());
        // Cache rebuild deferred to refreshRegionalCacheIfNeeded (respects GPS cooldown)
    }

    const V1Settings& camSettings = settings->get();
    if (!camSettings.cameraAlertsEnabled || !cameraManager->isLoaded() || !gpsHandler || !gpsHandler->isEnabled()) {
        // Feature off or GPS unavailable: clear any stale camera UI/state
        activeCameraAlerts.clear();
        recentlyPassedCameras.clear();
        if (display) {
            display->clearAllCameraAlerts();
            display->clearCameraAlerts();
        }
        return;
    }

    refreshRegionalCacheIfNeeded(now, camSettings);
    detectApproachingCameras(now, camSettings);
}

void CameraAlertModule::clearActiveCamerasAndMarkPassed(unsigned long now) {
    // Mark all active cameras as passed so they don't re-alert if we return
    for (const auto& cam : activeCameraAlerts) {
        PassedCameraTracker passed;
        passed.lat = cam.camera.latitude;
        passed.lon = cam.camera.longitude;
        passed.passedTimeMs = now;
        recentlyPassedCameras.push_back(passed);
    }
    activeCameraAlerts.clear();
    alertStartedAtMs = 0;
    if (display) {
        display->clearAllCameraAlerts();
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
        Serial.println("[Camera] GPS ready - deferring cache operations for 3s");
    }
    wasGpsReady = gpsReady;

    if (!gpsReady) return;

    // Defer heavy operations during GPS ready cooldown (display lag prevention)
    if (gpsReadyAtMs > 0 && (now - gpsReadyAtMs) < GPS_READY_COOLDOWN_MS) {
        return;
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
        Serial.printf("[Camera] Building regional cache at %.4f, %.4f\n", cacheFix.latitude, cacheFix.longitude);
        if (cameraManager->buildRegionalCache(cacheFix.latitude, cacheFix.longitude, CACHE_RADIUS_MILES)) {
            PERF_INC(cameraCacheRefreshes);
            cameraManager->saveRegionalCache(&LittleFS, "/cameras_cache.json");
        }
    }
}

void CameraAlertModule::detectApproachingCameras(unsigned long now, const V1Settings& camSettings) {
    if (!gpsHandler || !gpsHandler->isReadyForNavigation()) {
        // Safety: clear stale alerts if GPS lost for too long
        if (!activeCameraAlerts.empty() && alertStartedAtMs > 0 &&
            (now - alertStartedAtMs) > ALERT_MAX_DURATION_MS) {
            Serial.println("[Camera] Safety timeout: clearing stale alerts (GPS lost)");
            activeCameraAlerts.clear();
            alertStartedAtMs = 0;
            if (display) {
                display->clearAllCameraAlerts();
                display->clearCameraAlerts();
            }
        }
        return;
    }

    if (now - lastCameraCheckMs < CAMERA_CHECK_INTERVAL_MS) return;
    lastCameraCheckMs = now;

    GPSFix fix = gpsHandler->getFix();
    float lat = fix.latitude;
    float lon = fix.longitude;
    float heading = fix.heading_deg;
    float alertRadius = static_cast<float>(camSettings.cameraAlertDistanceM);

    // Safety check: if we have active alerts but NO cameras nearby at all
    // (even with 2x search radius), clear immediately - we've left the area
    if (!activeCameraAlerts.empty()) {
        bool anyCamerasNearby = cameraManager->hasNearbyCamera(lat, lon, alertRadius * 2.0f);
        if (!anyCamerasNearby) {
            Serial.println("[Camera] No cameras in area - clearing stale alerts");
            clearActiveCamerasAndMarkPassed(now);
            return;
        }
    }

    // Also enforce max duration as final safety (shouldn't normally trigger)
    if (!activeCameraAlerts.empty() && alertStartedAtMs > 0 &&
        (now - alertStartedAtMs) > ALERT_MAX_DURATION_MS) {
        Serial.println("[Camera] Safety timeout: clearing stale alerts (max duration)");
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
    std::vector<NearbyCameraResult> nearbyCameras = cameraManager->findNearby(
        lat, lon, heading, alertRadius, MAX_ACTIVE_CAMERAS + 2);  // Get a few extra for filtering

    // Filter: only keep approaching cameras, exclude recently passed
    std::vector<NearbyCameraResult> approachingCameras;
    for (const auto& cam : nearbyCameras) {
        if (!cam.isApproaching) continue;

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

        approachingCameras.push_back(cam);
        if ((int)approachingCameras.size() >= MAX_ACTIVE_CAMERAS) break;
    }

    // Check for cameras that were active but are no longer approaching (passed)
    for (const auto& oldCam : activeCameraAlerts) {
        bool stillApproaching = false;
        for (const auto& newCam : approachingCameras) {
            // Match by position (within 50m)
            float dist = CameraManager::haversineDistance(
                oldCam.camera.latitude, oldCam.camera.longitude,
                newCam.camera.latitude, newCam.camera.longitude);
            if (dist < 50.0f) {
                stillApproaching = true;
                break;
            }
        }

        // Also check if we're moving away from the camera (distance increasing)
        if (!stillApproaching) {
            float currentDist = CameraManager::haversineDistance(
                lat, lon, oldCam.camera.latitude, oldCam.camera.longitude);
            if (currentDist > oldCam.distance_m + 100.0f) {
                PassedCameraTracker passed;
                passed.lat = oldCam.camera.latitude;
                passed.lon = oldCam.camera.longitude;
                passed.passedTimeMs = now;
                recentlyPassedCameras.push_back(passed);
                Serial.printf("[Camera] PASSED: %s (distance increased from %.0fm to %.0fm)\n",
                              oldCam.camera.getTypeName(), oldCam.distance_m, currentDist);
            }
        }
    }

    // Check for new cameras that weren't in the previous active list
    for (const auto& newCam : approachingCameras) {
        bool wasAlreadyActive = false;
        for (const auto& oldCam : activeCameraAlerts) {
            float dist = CameraManager::haversineDistance(
                oldCam.camera.latitude, oldCam.camera.longitude,
                newCam.camera.latitude, newCam.camera.longitude);
            if (dist < 50.0f) {
                wasAlreadyActive = true;
                break;
            }
        }

        if (!wasAlreadyActive) {
            // New camera entering alert range
            bool isPrimary = activeCameraAlerts.empty();
            Serial.printf("[Camera] ALERT: %s at %.0fm AHEAD%s\n",
                          newCam.camera.getTypeName(),
                          newCam.distance_m,
                          isPrimary ? " (PRIMARY)" : " (SECONDARY)");

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
        }
    }

    // Update active camera list
    bool wasEmpty = activeCameraAlerts.empty();
    activeCameraAlerts = approachingCameras;
    
    // Track when alerts started (for safety timeout)
    if (wasEmpty && !activeCameraAlerts.empty()) {
        alertStartedAtMs = now;  // Alerts just started
    } else if (activeCameraAlerts.empty()) {
        alertStartedAtMs = 0;    // No alerts, reset timer
    }

    // Debug: log active camera count changes
    static int lastCameraCount = 0;
    if ((int)activeCameraAlerts.size() != lastCameraCount) {
        Serial.printf("[Camera] Active cameras: %d\n", (int)activeCameraAlerts.size());
        lastCameraCount = (int)activeCameraAlerts.size();
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

    if (!activeCameraAlerts.empty()) {
        handleRealCards(dispSettings);
    } else {
        display->clearAllCameraAlerts();
    }
}

void CameraAlertModule::updateMainDisplay(bool v1HasAlerts) {
    if (!display || !settings) return;
    if (v1HasAlerts) return;  // V1 owns display when alerts are active

    const V1Settings& dispSettings = settings->get();

    if (ensureTestActive()) {
        handleTestDisplay(v1HasAlerts, dispSettings);
        return;
    }

    if (!activeCameraAlerts.empty()) {
        handleRealDisplay(v1HasAlerts, dispSettings);
    } else {
        display->clearCameraAlerts();
    }
}

void CameraAlertModule::handleTestCards(const V1Settings& dispSettings) {
    unsigned long elapsed = millis() - cameraTestPhaseStartMs;
    int newPhase = (elapsed / CAMERA_TEST_PHASE_DURATION_MS) % 3;
    if (newPhase != cameraTestPhase) {
        cameraTestPhase = newPhase;
        Serial.printf("[Camera] Test phase: %d camera(s)\n", cameraTestPhase + 1);
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
        if (i < (int)activeCameraAlerts.size()) {
            display->setCameraAlertState(i, true,
                                         activeCameraAlerts[i].camera.getShortTypeName(),
                                         activeCameraAlerts[i].distance_m,
                                         dispSettings.colorCameraAlert);
        } else {
            display->setCameraAlertState(i, false, "", 0, 0);
        }
    }
}

void CameraAlertModule::handleRealDisplay(bool v1HasAlerts, const V1Settings& dispSettings) {
    int count = std::min((int)activeCameraAlerts.size(), MAX_ACTIVE_CAMERAS);
    if (count == 0) {
        display->clearCameraAlerts();
        return;
    }

    V1Display::CameraAlertInfo camInfos[MAX_ACTIVE_CAMERAS];
    for (int i = 0; i < count; i++) {
        camInfos[i].typeName = activeCameraAlerts[i].camera.getTypeName();
        camInfos[i].distance_m = activeCameraAlerts[i].distance_m;
        camInfos[i].color = dispSettings.colorCameraAlert;
    }

    display->updateCameraAlerts(camInfos, count, v1HasAlerts);
}
