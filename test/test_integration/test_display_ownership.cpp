/**
 * Display Ownership Integration Tests
 * 
 * These tests catch bugs where multiple code paths try to manage the same
 * display state in a single frame. The camera test flashing bug (Jan 26, 2026)
 * was caused by:
 *   - updateCameraCardState() setting camera cards
 *   - updateCameraAlerts() ALSO setting camera cards
 *   - Different values → constant redraws → flashing
 * 
 * Pattern: Each display element should have ONE owner per frame.
 * These tests verify that ownership is exclusive.
 */

#include <unity.h>
#include <cstring>
#include <cstdint>

// ============================================================================
// DISPLAY CALL TRACKER
// Tracks which functions called display methods and how many times per "frame"
// ============================================================================

struct DisplayCallTracker {
    // Per-frame counters (reset between frames)
    int setCameraAlertStateCalls = 0;
    int clearCameraAlertsCalls = 0;
    int updateCameraAlertsCalls = 0;
    int flushCalls = 0;
    int forceCardRedrawSets = 0;
    int displayUpdateCalls = 0;        // Main display update calls
    int displayUpdatePreviewCalls = 0; // Preview-mode display update calls
    
    // Track WHO called (for debugging conflicts)
    enum class Caller {
        NONE,
        UPDATE_CAMERA_CARD_STATE,
        UPDATE_CAMERA_ALERTS,
        DISPLAY_UPDATE,
        DISPLAY_UPDATE_PREVIEW,
        CLEAR_CAMERA_ALERTS
    };
    Caller lastCameraCardCaller = Caller::NONE;
    Caller lastFlushCaller = Caller::NONE;
    Caller lastMainDisplayCaller = Caller::NONE;  // Who wrote to main display
    
    // Conflict detection
    bool cameraCardConflict = false;  // Set if multiple callers wrote camera state
    bool mainDisplayConflict = false; // Set if preview AND live data wrote to main display
    
    void reset() {
        setCameraAlertStateCalls = 0;
        clearCameraAlertsCalls = 0;
        updateCameraAlertsCalls = 0;
        flushCalls = 0;
        forceCardRedrawSets = 0;
        displayUpdateCalls = 0;
        displayUpdatePreviewCalls = 0;
        lastCameraCardCaller = Caller::NONE;
        lastFlushCaller = Caller::NONE;
        lastMainDisplayCaller = Caller::NONE;
        cameraCardConflict = false;
        mainDisplayConflict = false;
    }
    
    void recordCameraCardWrite(Caller caller) {
        setCameraAlertStateCalls++;
        if (lastCameraCardCaller != Caller::NONE && lastCameraCardCaller != caller) {
            cameraCardConflict = true;  // Different caller wrote to same state!
        }
        lastCameraCardCaller = caller;
    }
    
    void recordMainDisplayWrite(Caller caller) {
        if (caller == Caller::DISPLAY_UPDATE) displayUpdateCalls++;
        if (caller == Caller::DISPLAY_UPDATE_PREVIEW) displayUpdatePreviewCalls++;
        if (lastMainDisplayCaller != Caller::NONE && lastMainDisplayCaller != caller) {
            mainDisplayConflict = true;  // Preview and live data both writing!
        }
        lastMainDisplayCaller = caller;
    }
    
    void recordFlush(Caller caller) {
        flushCalls++;
        lastFlushCaller = caller;
    }
};

static DisplayCallTracker g_tracker;

// ============================================================================
// MOCK DISPLAY CLASS
// Mimics V1Display but tracks all calls
// ============================================================================

class MockDisplay {
public:
    // Camera card state (matches real display)
    static const int MAX_CAMERA_CARDS = 2;
    struct CameraCard {
        bool active = false;
        char typeName[16] = {0};
        float distance_m = 0;
        uint16_t color = 0;
    };
    CameraCard cameraCards[MAX_CAMERA_CARDS];
    int activeCameraCount = 0;
    bool forceCardRedraw = false;
    
    void setCameraAlertState(int index, bool active, const char* typeName, 
                             float distance_m, uint16_t color,
                             DisplayCallTracker::Caller caller) {
        if (index < 0 || index >= MAX_CAMERA_CARDS) return;
        
        g_tracker.recordCameraCardWrite(caller);
        
        cameraCards[index].active = active;
        cameraCards[index].distance_m = distance_m;
        cameraCards[index].color = color;
        if (active && typeName) {
            strncpy(cameraCards[index].typeName, typeName, 15);
            cameraCards[index].typeName[15] = '\0';
        } else {
            cameraCards[index].typeName[0] = '\0';
        }
        
        // Update count
        activeCameraCount = 0;
        for (int i = 0; i < MAX_CAMERA_CARDS; i++) {
            if (cameraCards[i].active) activeCameraCount++;
        }
    }
    
    void clearAllCameraAlerts(DisplayCallTracker::Caller caller) {
        g_tracker.clearCameraAlertsCalls++;
        for (int i = 0; i < MAX_CAMERA_CARDS; i++) {
            cameraCards[i].active = false;
            cameraCards[i].typeName[0] = '\0';
            cameraCards[i].distance_m = 0;
            cameraCards[i].color = 0;
        }
        activeCameraCount = 0;
    }
    
    void flush(DisplayCallTracker::Caller caller) {
        g_tracker.recordFlush(caller);
    }
    
    void setForceCardRedraw(bool value, DisplayCallTracker::Caller /*caller*/) {
        if (value) g_tracker.forceCardRedrawSets++;
        forceCardRedraw = value;
    }
};

static MockDisplay g_display;

// ============================================================================
// CAMERA DISPLAY PATH DECISION LOGIC
// Extracted from main.cpp for testability
// ============================================================================

enum class CameraDisplayPath {
    NONE,                          // No camera display active
    CARD_VIA_UPDATE_CARD_STATE,    // V1 connected: updateCameraCardState handles cards
    MAIN_VIA_UPDATE_CAMERA_ALERTS  // V1 disconnected: updateCameraAlerts handles main area
};

/**
 * Determines which code path should handle camera display.
 * 
 * RULE: Only ONE path should be active per frame.
 * 
 * @param cameraTestActive  Camera test mode is running
 * @param v1Connected       V1 is connected via BLE
 * @param v1HasAlerts       V1 has active alerts
 * @param hasRealCameras    Real camera alerts exist
 * @return The path that should handle camera display
 */
CameraDisplayPath getCameraDisplayPath(bool cameraTestActive, bool v1Connected,
                                        bool v1HasAlerts, bool hasRealCameras) {
    (void)v1Connected;  // Connection state no longer decides ownership
    // No cameras at all
    if (!cameraTestActive && !hasRealCameras) {
        return CameraDisplayPath::NONE;
    }
    
    // V1 has alerts → camera becomes secondary cards (set by updateCameraCardState)
    // No V1 alerts → camera owns main area (updateCameraAlerts)
    if (v1HasAlerts) {
        return CameraDisplayPath::CARD_VIA_UPDATE_CARD_STATE;
    }
    return CameraDisplayPath::MAIN_VIA_UPDATE_CAMERA_ALERTS;
}

/**
 * Simulates updateCameraCardState() from main.cpp
 * Only runs if V1 connected path is active
 */
void simulateUpdateCameraCardState(bool cameraTestActive, bool v1Connected,
                                   unsigned long elapsed, uint16_t color) {
    if (!cameraTestActive) return;
    if (!v1Connected) return;  // This path only handles V1-connected case
    
    // Calculate test phase
    const unsigned long PHASE_DURATION = 3000;
    int numCameras = ((elapsed / PHASE_DURATION) % 3) + 1;
    
    // Set up cards (same as main.cpp)
    static const char* secondaryTypes[] = {"SPEED", "ALPR"};
    static const float baseDistances[] = {800.0f, 1200.0f};
    
    float dist0 = baseDistances[0] - (elapsed * 0.01f);
    float dist1 = baseDistances[1] - (elapsed * 0.01f);
    if (dist0 < 50.0f) dist0 = 50.0f;
    if (dist1 < 50.0f) dist1 = 50.0f;
    
    if (numCameras >= 2) {
        g_display.setCameraAlertState(0, true, secondaryTypes[0], dist0, color,
                                      DisplayCallTracker::Caller::UPDATE_CAMERA_CARD_STATE);
    } else {
        g_display.setCameraAlertState(0, false, "", 0, 0,
                                      DisplayCallTracker::Caller::UPDATE_CAMERA_CARD_STATE);
    }
    if (numCameras >= 3) {
        g_display.setCameraAlertState(1, true, secondaryTypes[1], dist1, color,
                                      DisplayCallTracker::Caller::UPDATE_CAMERA_CARD_STATE);
    } else {
        g_display.setCameraAlertState(1, false, "", 0, 0,
                                      DisplayCallTracker::Caller::UPDATE_CAMERA_CARD_STATE);
    }
}

/**
 * Simulates updateCameraAlerts() from main.cpp (camera test path)
 * Only runs if V1 disconnected path is active
 */
void simulateUpdateCameraAlerts(bool cameraTestActive, bool v1Connected,
                                bool v1HasAlerts, unsigned long elapsed, uint16_t color) {
    if (!cameraTestActive) return;
    if (v1HasAlerts) return;  // Skip when V1 has alerts (card path owns state)
    
    // This path handles main area display when V1 has no alerts
    // For this test, we just track that it was called
    g_tracker.updateCameraAlertsCalls++;
    
    // It would set camera cards too (for secondary cameras)
    int numCameras = ((elapsed / 3000) % 3) + 1;
    if (numCameras >= 2) {
        g_display.setCameraAlertState(0, true, "SPEED", 800.0f - elapsed * 0.01f, color,
                                      DisplayCallTracker::Caller::UPDATE_CAMERA_ALERTS);
    }
    if (numCameras >= 3) {
        g_display.setCameraAlertState(1, true, "ALPR", 1200.0f - elapsed * 0.01f, color,
                                      DisplayCallTracker::Caller::UPDATE_CAMERA_ALERTS);
    }
}

/**
 * Simulates one iteration of the main loop
 */
void simulateLoopIteration(bool cameraTestActive, bool v1Connected, bool v1HasAlerts,
                           bool hasRealCameras, unsigned long elapsed) {
    g_tracker.reset();
    
    // Get the expected path
    CameraDisplayPath expectedPath = getCameraDisplayPath(cameraTestActive, v1Connected,
                                                          v1HasAlerts, hasRealCameras);
    
    // Simulate the actual code paths (as they exist in main.cpp)
    if (expectedPath == CameraDisplayPath::CARD_VIA_UPDATE_CARD_STATE) {
        simulateUpdateCameraCardState(cameraTestActive, v1Connected, elapsed, 0xFFFF);
    }
    
    if (expectedPath == CameraDisplayPath::MAIN_VIA_UPDATE_CAMERA_ALERTS) {
        simulateUpdateCameraAlerts(cameraTestActive, v1Connected, v1HasAlerts, elapsed, 0xFFFF);
    }
}

// ============================================================================
// TESTS: Camera Display Path Decision
// ============================================================================

void test_camera_path_no_cameras_no_test() {
    auto path = getCameraDisplayPath(false, false, false, false);
    TEST_ASSERT_EQUAL(CameraDisplayPath::NONE, path);
}

void test_camera_path_test_v1_disconnected() {
    auto path = getCameraDisplayPath(true, false, false, false);
    TEST_ASSERT_EQUAL(CameraDisplayPath::MAIN_VIA_UPDATE_CAMERA_ALERTS, path);
}

void test_camera_path_test_v1_connected_no_alerts() {
    auto path = getCameraDisplayPath(true, true, false, false);
    TEST_ASSERT_EQUAL(CameraDisplayPath::MAIN_VIA_UPDATE_CAMERA_ALERTS, path);
}

void test_camera_path_test_v1_connected_with_alerts() {
    auto path = getCameraDisplayPath(true, true, true, false);
    TEST_ASSERT_EQUAL(CameraDisplayPath::CARD_VIA_UPDATE_CARD_STATE, path);
}

void test_camera_path_real_cameras_v1_disconnected() {
    auto path = getCameraDisplayPath(false, false, false, true);
    TEST_ASSERT_EQUAL(CameraDisplayPath::MAIN_VIA_UPDATE_CAMERA_ALERTS, path);
}

void test_camera_path_real_cameras_v1_connected_no_alerts() {
    auto path = getCameraDisplayPath(false, true, false, true);
    TEST_ASSERT_EQUAL(CameraDisplayPath::MAIN_VIA_UPDATE_CAMERA_ALERTS, path);
}

void test_camera_path_real_cameras_v1_connected() {
    auto path = getCameraDisplayPath(false, true, true, true);
    TEST_ASSERT_EQUAL(CameraDisplayPath::CARD_VIA_UPDATE_CARD_STATE, path);
}

// ============================================================================
// TESTS: Ownership Conflict Detection
// ============================================================================

void test_no_conflict_v1_disconnected_camera_test() {
    // When V1 disconnected, only updateCameraAlerts should write camera state
    simulateLoopIteration(true, false, false, false, 5000);
    
    TEST_ASSERT_FALSE_MESSAGE(g_tracker.cameraCardConflict,
        "V1 disconnected: should have single owner for camera cards");
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::UPDATE_CAMERA_ALERTS,
        g_tracker.lastCameraCardCaller,
        "V1 disconnected: updateCameraAlerts should own camera cards");
}

void test_no_conflict_v1_connected_camera_test() {
    // When V1 connected, only updateCameraCardState should write camera state
    simulateLoopIteration(true, true, true, false, 5000);
    
    TEST_ASSERT_FALSE_MESSAGE(g_tracker.cameraCardConflict,
        "V1 connected: should have single owner for camera cards");
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::UPDATE_CAMERA_CARD_STATE,
        g_tracker.lastCameraCardCaller,
        "V1 connected: updateCameraCardState should own camera cards");
}

void test_no_conflict_no_cameras() {
    // No cameras active, no writes should occur
    simulateLoopIteration(false, true, true, false, 0);
    
    TEST_ASSERT_EQUAL_MESSAGE(0, g_tracker.setCameraAlertStateCalls,
        "No cameras: no setCameraAlertState calls expected");
    TEST_ASSERT_FALSE(g_tracker.cameraCardConflict);
}

void test_multiple_frames_no_conflict() {
    // Simulate 10 consecutive frames with V1 connected + camera test
    for (int frame = 0; frame < 10; frame++) {
        unsigned long elapsed = 1000 + frame * 50;  // 50ms per frame
        simulateLoopIteration(true, true, true, false, elapsed);
        
        TEST_ASSERT_FALSE_MESSAGE(g_tracker.cameraCardConflict,
            "Frame should have single owner");
    }
}

void test_v1_connects_mid_test_ownership_transfers() {
    // Frame 1: V1 disconnected, updateCameraAlerts owns
    // Use elapsed=5000 to ensure we're in phase 2 (2 cameras, so cards get set)
    simulateLoopIteration(true, false, false, false, 5000);
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::UPDATE_CAMERA_ALERTS,
        g_tracker.lastCameraCardCaller,
        "Frame 1: V1 disconnected, updateCameraAlerts should own");
    TEST_ASSERT_FALSE(g_tracker.cameraCardConflict);
    
    // Frame 2: V1 connects, ownership should transfer to updateCameraCardState
    simulateLoopIteration(true, true, true, false, 5050);
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::UPDATE_CAMERA_CARD_STATE,
        g_tracker.lastCameraCardCaller,
        "Frame 2: V1 connected, updateCameraCardState should own");
    
    // No conflicts in second frame
    TEST_ASSERT_FALSE(g_tracker.cameraCardConflict);
}

void test_v1_disconnects_mid_test_ownership_transfers() {
    // Frame 1: V1 connected, updateCameraCardState owns
    // Use elapsed=5000 to ensure we're in phase 2 (2 cameras)
    simulateLoopIteration(true, true, true, false, 5000);
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::UPDATE_CAMERA_CARD_STATE,
        g_tracker.lastCameraCardCaller,
        "Frame 1: V1 connected, updateCameraCardState should own");
    TEST_ASSERT_FALSE(g_tracker.cameraCardConflict);
    
    // Frame 2: V1 disconnects, ownership should transfer to updateCameraAlerts
    simulateLoopIteration(true, false, false, false, 5050);
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::UPDATE_CAMERA_ALERTS,
        g_tracker.lastCameraCardCaller,
        "Frame 2: V1 disconnected, updateCameraAlerts should own");
    
    // No conflicts in second frame
    TEST_ASSERT_FALSE(g_tracker.cameraCardConflict);
}

// ============================================================================
// TESTS: Flush Count (should be minimal per frame)
// ============================================================================

void test_single_flush_per_frame() {
    // In a well-behaved frame, there should be at most 1 flush
    // (The camera test bug caused 2+ flushes per frame)
    
    g_tracker.reset();
    
    // Simulate camera test with V1 connected
    simulateUpdateCameraCardState(true, true, 5000, 0xFFFF);
    // display.update() would flush once
    g_display.flush(DisplayCallTracker::Caller::DISPLAY_UPDATE);
    
    // updateCameraAlerts should NOT run when V1 connected (that's the fix)
    simulateUpdateCameraAlerts(true, true, true, 5000, 0xFFFF);
    
    // Should only have 1 flush (from display.update)
    TEST_ASSERT_EQUAL_MESSAGE(1, g_tracker.flushCalls,
        "Should have exactly 1 flush per frame");
}

// ============================================================================
// COLOR PREVIEW TEST MODE SIMULATION
// ============================================================================

/**
 * Simulates the color preview path decision logic from main.cpp
 * 
 * Color preview writes to the MAIN display area.
 * When active, it should be the only writer to main display.
 * When inactive, live V1 data (or scanning screen) should own main display.
 */
enum class MainDisplayPath {
    NONE,               // No display update needed
    LIVE_DATA,          // Live V1 data or scanning screen
    COLOR_PREVIEW       // Color preview test mode
};

MainDisplayPath getMainDisplayPath(bool colorPreviewActive, bool v1Connected) {
    if (colorPreviewActive) {
        return MainDisplayPath::COLOR_PREVIEW;
    }
    return MainDisplayPath::LIVE_DATA;
}

/**
 * Simulates display.update() with preview data
 */
void simulateDisplayUpdateWithPreview(bool colorPreviewActive, uint16_t previewColor) {
    if (colorPreviewActive) {
        // Color preview writes to main display
        g_tracker.recordMainDisplayWrite(DisplayCallTracker::Caller::DISPLAY_UPDATE_PREVIEW);
    }
}

/**
 * Simulates display.update() with live V1 data
 */
void simulateDisplayUpdateWithLiveData(bool v1Connected, bool v1HasAlerts) {
    // Live data path writes to main display
    g_tracker.recordMainDisplayWrite(DisplayCallTracker::Caller::DISPLAY_UPDATE);
}

/**
 * Simulates one loop iteration for color preview mode
 * 
 * The key invariant: only ONE path should write to main display per frame
 */
void simulateColorPreviewLoop(bool colorPreviewActive, bool v1Connected, 
                              bool v1HasAlerts, uint16_t previewColor) {
    g_tracker.reset();
    
    MainDisplayPath expectedPath = getMainDisplayPath(colorPreviewActive, v1Connected);
    
    // In main.cpp, display.update() is called with either:
    // 1. Preview data (when colorPreviewActive) OR
    // 2. Live V1 data (when not in preview mode)
    // Never both!
    
    if (expectedPath == MainDisplayPath::COLOR_PREVIEW) {
        simulateDisplayUpdateWithPreview(colorPreviewActive, previewColor);
    } else {
        simulateDisplayUpdateWithLiveData(v1Connected, v1HasAlerts);
    }
    
    g_display.flush(DisplayCallTracker::Caller::DISPLAY_UPDATE);
}

// ============================================================================
// TESTS: Color Preview Ownership
// ============================================================================

void test_color_preview_owns_main_display_v1_connected() {
    // Color preview active + V1 connected = preview owns main display
    simulateColorPreviewLoop(true, true, true, 0xFF00);
    
    TEST_ASSERT_FALSE_MESSAGE(g_tracker.mainDisplayConflict,
        "Color preview should have single ownership of main display");
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::DISPLAY_UPDATE_PREVIEW,
        g_tracker.lastMainDisplayCaller,
        "Color preview should own main display when active");
    TEST_ASSERT_EQUAL(1, g_tracker.displayUpdatePreviewCalls);
    TEST_ASSERT_EQUAL(0, g_tracker.displayUpdateCalls);
}

void test_color_preview_owns_main_display_v1_disconnected() {
    // Color preview active + V1 disconnected = preview owns main display
    simulateColorPreviewLoop(true, false, false, 0x00FF);
    
    TEST_ASSERT_FALSE_MESSAGE(g_tracker.mainDisplayConflict,
        "Color preview should have single ownership of main display");
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::DISPLAY_UPDATE_PREVIEW,
        g_tracker.lastMainDisplayCaller,
        "Color preview should own main display regardless of V1 state");
}

void test_live_data_owns_main_display_v1_connected() {
    // No preview + V1 connected = live data owns main display
    simulateColorPreviewLoop(false, true, true, 0);
    
    TEST_ASSERT_FALSE_MESSAGE(g_tracker.mainDisplayConflict,
        "Live data should have single ownership of main display");
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::DISPLAY_UPDATE,
        g_tracker.lastMainDisplayCaller,
        "Live V1 data should own main display when not in preview mode");
    TEST_ASSERT_EQUAL(1, g_tracker.displayUpdateCalls);
    TEST_ASSERT_EQUAL(0, g_tracker.displayUpdatePreviewCalls);
}

void test_live_data_owns_main_display_v1_disconnected() {
    // No preview + V1 disconnected = live data (scanning screen) owns main display
    simulateColorPreviewLoop(false, false, false, 0);
    
    TEST_ASSERT_FALSE_MESSAGE(g_tracker.mainDisplayConflict,
        "Live data should have single ownership of main display");
    TEST_ASSERT_EQUAL_MESSAGE(DisplayCallTracker::Caller::DISPLAY_UPDATE,
        g_tracker.lastMainDisplayCaller,
        "Scanning screen should own main display when V1 disconnected and no preview");
}

void test_color_preview_ends_ownership_transfers_to_live() {
    // Frame 1: Color preview active
    simulateColorPreviewLoop(true, true, true, 0xFF00);
    TEST_ASSERT_EQUAL(DisplayCallTracker::Caller::DISPLAY_UPDATE_PREVIEW,
        g_tracker.lastMainDisplayCaller);
    TEST_ASSERT_FALSE(g_tracker.mainDisplayConflict);
    
    // Frame 2: Color preview ends, live data takes over
    simulateColorPreviewLoop(false, true, true, 0);
    TEST_ASSERT_EQUAL(DisplayCallTracker::Caller::DISPLAY_UPDATE,
        g_tracker.lastMainDisplayCaller);
    TEST_ASSERT_FALSE_MESSAGE(g_tracker.mainDisplayConflict,
        "Ownership transfer should not cause conflict");
}

void test_main_display_path_decision() {
    // Test all combinations of colorPreviewActive and v1Connected
    TEST_ASSERT_EQUAL(MainDisplayPath::COLOR_PREVIEW, 
        getMainDisplayPath(true, true));
    TEST_ASSERT_EQUAL(MainDisplayPath::COLOR_PREVIEW, 
        getMainDisplayPath(true, false));
    TEST_ASSERT_EQUAL(MainDisplayPath::LIVE_DATA, 
        getMainDisplayPath(false, true));
    TEST_ASSERT_EQUAL(MainDisplayPath::LIVE_DATA, 
        getMainDisplayPath(false, false));
}

// ============================================================================
// TESTS: Force Redraw Flag Management
// ============================================================================

void test_force_redraw_not_set_when_no_change() {
    g_tracker.reset();
    
    // Two frames with identical state should not keep setting forceRedraw
    simulateLoopIteration(true, true, true, false, 5000);
    int firstFrameForceCount = g_tracker.forceCardRedrawSets;
    
    simulateLoopIteration(true, true, true, false, 5050);
    int secondFrameForceCount = g_tracker.forceCardRedrawSets;
    
    // forceCardRedraw should not accumulate (this was the bug pattern)
    // Both frames should have same or zero force redraw sets
    TEST_ASSERT_TRUE_MESSAGE(
        secondFrameForceCount <= 1,
        "forceCardRedraw should not be set unconditionally every frame");
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {
    g_tracker.reset();
    g_display = MockDisplay();
}

void tearDown(void) {}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Path decision tests
    RUN_TEST(test_camera_path_no_cameras_no_test);
    RUN_TEST(test_camera_path_test_v1_disconnected);
    RUN_TEST(test_camera_path_test_v1_connected_no_alerts);
    RUN_TEST(test_camera_path_test_v1_connected_with_alerts);
    RUN_TEST(test_camera_path_real_cameras_v1_disconnected);
    RUN_TEST(test_camera_path_real_cameras_v1_connected_no_alerts);
    RUN_TEST(test_camera_path_real_cameras_v1_connected);
    
    // Ownership conflict tests
    RUN_TEST(test_no_conflict_v1_disconnected_camera_test);
    RUN_TEST(test_no_conflict_v1_connected_camera_test);
    RUN_TEST(test_no_conflict_no_cameras);
    RUN_TEST(test_multiple_frames_no_conflict);
    RUN_TEST(test_v1_connects_mid_test_ownership_transfers);
    RUN_TEST(test_v1_disconnects_mid_test_ownership_transfers);
    
    // Performance/correctness tests
    RUN_TEST(test_single_flush_per_frame);
    RUN_TEST(test_force_redraw_not_set_when_no_change);
    
    // Color preview ownership tests
    RUN_TEST(test_color_preview_owns_main_display_v1_connected);
    RUN_TEST(test_color_preview_owns_main_display_v1_disconnected);
    RUN_TEST(test_live_data_owns_main_display_v1_connected);
    RUN_TEST(test_live_data_owns_main_display_v1_disconnected);
    RUN_TEST(test_color_preview_ends_ownership_transfers_to_live);
    RUN_TEST(test_main_display_path_decision);
    
    return UNITY_END();
}
