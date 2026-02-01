/**
 * test_camera_alert.cpp - Unit tests for CameraAlertModule
 * 
 * Tests the camera alert detection and display logic.
 * Uses testable wrappers to test the core algorithms without
 * actual hardware dependencies.
 */
#include <unity.h>
#include <cmath>
#include <vector>

// ============================================================================
// Test Types and Mocks
// ============================================================================

// Camera types
enum CameraAlertType {
    SPEED = 0,
    REDLIGHT = 1,
    ALPR = 2
};

// GPS fix data
struct GPSFix {
    float lat = 0.0f;
    float lon = 0.0f;
    float speed_mph = 0.0f;
    float heading = 0.0f;
    bool hasValidFix = false;
    unsigned long fixAgeMs = 0;
};

// Camera result from database
struct NearbyCameraResult {
    float lat = 0.0f;
    float lon = 0.0f;
    float distance_m = 0.0f;
    float bearing = 0.0f;
    const char* typeName = "";
    CameraAlertType type = SPEED;
    float approaching_speed_mph = 0.0f;
    bool approaching = false;
    uint16_t color = 0xFFFF;
    
    NearbyCameraResult() = default;
    NearbyCameraResult(float la, float lo, float dist, float bear, const char* name, 
                       CameraAlertType t, float speed, bool appr, uint16_t col)
        : lat(la), lon(lo), distance_m(dist), bearing(bear), typeName(name),
          type(t), approaching_speed_mph(speed), approaching(appr), color(col) {}
};

// ============================================================================
// Testable Camera Alert Logic (extracted from CameraAlertModule)
// ============================================================================

class TestableCameraAlertLogic {
public:
    // Constants matching production
    static constexpr int MAX_ACTIVE_CAMERAS = 3;
    static constexpr float CAMERA_ALERT_COOLDOWN_M = 200.0f;
    static constexpr unsigned long PASSED_CAMERA_MEMORY_MS = 60000;
    static constexpr unsigned long ALERT_MAX_DURATION_MS = 120000;
    
    struct PassedCameraTracker {
        float lat = 0.0f;
        float lon = 0.0f;
        unsigned long passedTimeMs = 0;
    };
    
    // Active cameras tracking
    std::vector<NearbyCameraResult> activeCameras;
    std::vector<PassedCameraTracker> recentlyPassed;
    unsigned long alertStartedAtMs = 0;
    
    void reset() {
        activeCameras.clear();
        recentlyPassed.clear();
        alertStartedAtMs = 0;
    }
    
    // Check if a camera was recently passed (within cooldown distance)
    bool isRecentlyPassed(float lat, float lon, unsigned long now) {
        // Clean up old entries first
        cleanupPassedCameras(now);
        
        for (const auto& passed : recentlyPassed) {
            float dist = haversineDistance(lat, lon, passed.lat, passed.lon);
            if (dist < CAMERA_ALERT_COOLDOWN_M) {
                return true;
            }
        }
        return false;
    }
    
    // Mark a camera as passed
    void markPassed(float lat, float lon, unsigned long now) {
        PassedCameraTracker tracker;
        tracker.lat = lat;
        tracker.lon = lon;
        tracker.passedTimeMs = now;
        recentlyPassed.push_back(tracker);
    }
    
    // Clean up old passed camera entries
    void cleanupPassedCameras(unsigned long now) {
        recentlyPassed.erase(
            std::remove_if(recentlyPassed.begin(), recentlyPassed.end(),
                [now](const PassedCameraTracker& p) {
                    return (now - p.passedTimeMs) > PASSED_CAMERA_MEMORY_MS;
                }),
            recentlyPassed.end()
        );
    }
    
    // Add an approaching camera to active list
    bool addActiveCamera(const NearbyCameraResult& camera, unsigned long now) {
        // Don't add if at capacity
        if (activeCameras.size() >= MAX_ACTIVE_CAMERAS) {
            return false;
        }
        
        // Don't add if recently passed
        if (isRecentlyPassed(camera.lat, camera.lon, now)) {
            return false;
        }
        
        // Check for duplicate (same location)
        for (const auto& existing : activeCameras) {
            float dist = haversineDistance(camera.lat, camera.lon, existing.lat, existing.lon);
            if (dist < 10.0f) {  // Within 10m = same camera
                return false;
            }
        }
        
        activeCameras.push_back(camera);
        
        // Track alert start time for timeout
        if (alertStartedAtMs == 0) {
            alertStartedAtMs = now;
        }
        
        return true;
    }
    
    // Update camera distance and remove if passed
    void updateCameraDistance(float newDistance, size_t index, float lat, float lon, unsigned long now) {
        if (index >= activeCameras.size()) return;
        
        float oldDistance = activeCameras[index].distance_m;
        activeCameras[index].distance_m = newDistance;
        
        // Check if we passed it (distance increased after being close)
        if (newDistance > oldDistance && oldDistance < 50.0f) {
            // Passed the camera
            markPassed(lat, lon, now);
            activeCameras.erase(activeCameras.begin() + index);
        }
    }
    
    // Check for alert timeout (stale alerts)
    bool shouldTimeoutAlerts(unsigned long now) const {
        return alertStartedAtMs > 0 && 
               (now - alertStartedAtMs) > ALERT_MAX_DURATION_MS;
    }
    
    // Clear all alerts (for timeout or manual clear)
    void clearAllAlerts(unsigned long now) {
        // Mark all active cameras as passed
        for (const auto& camera : activeCameras) {
            markPassed(camera.lat, camera.lon, now);
        }
        activeCameras.clear();
        alertStartedAtMs = 0;
    }
    
    // Sort cameras by distance (closest first)
    void sortByDistance() {
        std::sort(activeCameras.begin(), activeCameras.end(),
            [](const NearbyCameraResult& a, const NearbyCameraResult& b) {
                return a.distance_m < b.distance_m;
            });
    }
    
private:
    // Haversine distance calculation (meters)
    float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
        constexpr float R = 6371000.0f; // Earth radius in meters
        
        float dLat = (lat2 - lat1) * M_PI / 180.0f;
        float dLon = (lon2 - lon1) * M_PI / 180.0f;
        
        lat1 = lat1 * M_PI / 180.0f;
        lat2 = lat2 * M_PI / 180.0f;
        
        float a = sin(dLat/2) * sin(dLat/2) +
                  cos(lat1) * cos(lat2) * sin(dLon/2) * sin(dLon/2);
        float c = 2 * atan2(sqrt(a), sqrt(1-a));
        
        return R * c;
    }
};

// Global test instance
static TestableCameraAlertLogic cameraLogic;

// ============================================================================
// Test Setup/Teardown
// ============================================================================

void setUp() {
    cameraLogic.reset();
}

void tearDown() {
    // Nothing to clean up
}

// ============================================================================
// Test Cases: Active Camera Management
// ============================================================================

void test_initial_state_no_cameras() {
    TEST_ASSERT_EQUAL(0, cameraLogic.activeCameras.size());
    TEST_ASSERT_EQUAL(0, cameraLogic.recentlyPassed.size());
}

void test_add_active_camera() {
    NearbyCameraResult camera(37.0f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    
    bool added = cameraLogic.addActiveCamera(camera, 1000);
    
    TEST_ASSERT_TRUE(added);
    TEST_ASSERT_EQUAL(1, cameraLogic.activeCameras.size());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 37.0f, cameraLogic.activeCameras[0].lat);
}

void test_add_camera_respects_max_limit() {
    // Add MAX cameras
    for (int i = 0; i < TestableCameraAlertLogic::MAX_ACTIVE_CAMERAS; i++) {
        NearbyCameraResult camera(37.0f + i * 0.01f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
        bool added = cameraLogic.addActiveCamera(camera, 1000);
        TEST_ASSERT_TRUE(added);
    }
    
    // Try to add one more
    NearbyCameraResult extra(37.1f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    bool added = cameraLogic.addActiveCamera(extra, 1000);
    
    TEST_ASSERT_FALSE(added);
    TEST_ASSERT_EQUAL(TestableCameraAlertLogic::MAX_ACTIVE_CAMERAS, cameraLogic.activeCameras.size());
}

void test_add_duplicate_camera_rejected() {
    NearbyCameraResult camera1(37.0f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    cameraLogic.addActiveCamera(camera1, 1000);
    
    // Try to add same location (within 10m)
    NearbyCameraResult camera2(37.0f + 0.00005f, -122.0f, 400.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    bool added = cameraLogic.addActiveCamera(camera2, 1000);
    
    TEST_ASSERT_FALSE(added);
    TEST_ASSERT_EQUAL(1, cameraLogic.activeCameras.size());
}

// ============================================================================
// Test Cases: Passed Camera Tracking
// ============================================================================

void test_mark_camera_passed() {
    cameraLogic.markPassed(37.0f, -122.0f, 1000);
    
    TEST_ASSERT_EQUAL(1, cameraLogic.recentlyPassed.size());
}

void test_recently_passed_camera_rejected() {
    // Mark a location as passed
    cameraLogic.markPassed(37.0f, -122.0f, 1000);
    
    // Try to add camera at same location
    NearbyCameraResult camera(37.0f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    bool added = cameraLogic.addActiveCamera(camera, 2000);
    
    TEST_ASSERT_FALSE(added);
}

void test_passed_camera_memory_expires() {
    cameraLogic.markPassed(37.0f, -122.0f, 1000);
    
    // After memory timeout, should be able to add again
    unsigned long afterTimeout = 1000 + TestableCameraAlertLogic::PASSED_CAMERA_MEMORY_MS + 1;
    
    NearbyCameraResult camera(37.0f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    bool added = cameraLogic.addActiveCamera(camera, afterTimeout);
    
    TEST_ASSERT_TRUE(added);
}

void test_cleanup_old_passed_cameras() {
    // Add multiple passed cameras
    cameraLogic.markPassed(37.0f, -122.0f, 1000);
    cameraLogic.markPassed(37.1f, -122.1f, 2000);
    cameraLogic.markPassed(37.2f, -122.2f, 3000);
    
    TEST_ASSERT_EQUAL(3, cameraLogic.recentlyPassed.size());
    
    // Cleanup - first one should be removed
    unsigned long afterFirst = 1000 + TestableCameraAlertLogic::PASSED_CAMERA_MEMORY_MS + 1;
    cameraLogic.cleanupPassedCameras(afterFirst);
    
    TEST_ASSERT_EQUAL(2, cameraLogic.recentlyPassed.size());
}

// ============================================================================
// Test Cases: Camera Distance Tracking
// ============================================================================

void test_camera_removed_when_passed() {
    // Add a camera
    NearbyCameraResult camera(37.0f, -122.0f, 100.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    cameraLogic.addActiveCamera(camera, 1000);
    
    // Simulate getting close then moving away (passed it)
    cameraLogic.updateCameraDistance(30.0f, 0, 37.0f, -122.0f, 2000);  // Getting close
    cameraLogic.updateCameraDistance(60.0f, 0, 37.0f, -122.0f, 3000);  // Moving away - passed!
    
    // Camera should be removed and marked as passed
    TEST_ASSERT_EQUAL(0, cameraLogic.activeCameras.size());
    TEST_ASSERT_EQUAL(1, cameraLogic.recentlyPassed.size());
}

void test_camera_not_removed_when_still_approaching() {
    NearbyCameraResult camera(37.0f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    cameraLogic.addActiveCamera(camera, 1000);
    
    // Distance decreasing (still approaching)
    cameraLogic.updateCameraDistance(400.0f, 0, 37.0f, -122.0f, 2000);
    cameraLogic.updateCameraDistance(300.0f, 0, 37.0f, -122.0f, 3000);
    
    // Should still be active
    TEST_ASSERT_EQUAL(1, cameraLogic.activeCameras.size());
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 300.0f, cameraLogic.activeCameras[0].distance_m);
}

// ============================================================================
// Test Cases: Alert Timeout
// ============================================================================

void test_alert_timeout_detection() {
    NearbyCameraResult camera(37.0f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800);
    cameraLogic.addActiveCamera(camera, 1000);
    
    // Just before timeout
    unsigned long justBefore = 1000 + TestableCameraAlertLogic::ALERT_MAX_DURATION_MS - 1;
    TEST_ASSERT_FALSE(cameraLogic.shouldTimeoutAlerts(justBefore));
    
    // After timeout
    unsigned long afterTimeout = 1000 + TestableCameraAlertLogic::ALERT_MAX_DURATION_MS + 1;
    TEST_ASSERT_TRUE(cameraLogic.shouldTimeoutAlerts(afterTimeout));
}

void test_clear_all_alerts() {
    // Add multiple cameras
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.0f, -122.0f, 500.0f, 0.0f, "Speed", SPEED, 30.0f, true, 0xF800), 1000);
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.1f, -122.1f, 600.0f, 0.0f, "Red Light", REDLIGHT, 25.0f, true, 0x07E0), 1000);
    
    TEST_ASSERT_EQUAL(2, cameraLogic.activeCameras.size());
    
    // Clear all
    cameraLogic.clearAllAlerts(5000);
    
    TEST_ASSERT_EQUAL(0, cameraLogic.activeCameras.size());
    TEST_ASSERT_EQUAL(2, cameraLogic.recentlyPassed.size());  // Both should be marked as passed
}

// ============================================================================
// Test Cases: Sorting
// ============================================================================

void test_sort_cameras_by_distance() {
    // Add cameras in non-sorted order
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.0f, -122.0f, 500.0f, 0.0f, "Far", SPEED, 30.0f, true, 0xF800), 1000);
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.1f, -122.1f, 100.0f, 0.0f, "Close", SPEED, 25.0f, true, 0x07E0), 1000);
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.2f, -122.2f, 300.0f, 0.0f, "Medium", SPEED, 28.0f, true, 0x001F), 1000);
    
    cameraLogic.sortByDistance();
    
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, cameraLogic.activeCameras[0].distance_m);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 300.0f, cameraLogic.activeCameras[1].distance_m);
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 500.0f, cameraLogic.activeCameras[2].distance_m);
}

// ============================================================================
// Test Cases: Different Camera Types
// ============================================================================

void test_different_camera_types() {
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.0f, -122.0f, 500.0f, 0.0f, "Speed Camera", SPEED, 30.0f, true, 0xF800), 1000);
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.1f, -122.1f, 600.0f, 0.0f, "Red Light", REDLIGHT, 25.0f, true, 0x07E0), 1000);
    cameraLogic.addActiveCamera(
        NearbyCameraResult(37.2f, -122.2f, 700.0f, 0.0f, "ALPR", ALPR, 45.0f, true, 0x001F), 1000);
    
    TEST_ASSERT_EQUAL(3, cameraLogic.activeCameras.size());
    TEST_ASSERT_EQUAL(SPEED, cameraLogic.activeCameras[0].type);
    TEST_ASSERT_EQUAL(REDLIGHT, cameraLogic.activeCameras[1].type);
    TEST_ASSERT_EQUAL(ALPR, cameraLogic.activeCameras[2].type);
}

// ============================================================================
// Main Test Runner
// ============================================================================

void runAllTests() {
    // Active camera management
    RUN_TEST(test_initial_state_no_cameras);
    RUN_TEST(test_add_active_camera);
    RUN_TEST(test_add_camera_respects_max_limit);
    RUN_TEST(test_add_duplicate_camera_rejected);
    
    // Passed camera tracking
    RUN_TEST(test_mark_camera_passed);
    RUN_TEST(test_recently_passed_camera_rejected);
    RUN_TEST(test_passed_camera_memory_expires);
    RUN_TEST(test_cleanup_old_passed_cameras);
    
    // Distance tracking
    RUN_TEST(test_camera_removed_when_passed);
    RUN_TEST(test_camera_not_removed_when_still_approaching);
    
    // Alert timeout
    RUN_TEST(test_alert_timeout_detection);
    RUN_TEST(test_clear_all_alerts);
    
    // Sorting
    RUN_TEST(test_sort_cameras_by_distance);
    
    // Camera types
    RUN_TEST(test_different_camera_types);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
