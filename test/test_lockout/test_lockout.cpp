/**
 * Lockout Manager Unit Tests
 * 
 * Tests geofence calculations, band-specific muting, and validation logic.
 * These tests catch bugs where:
 * - Wrong band is muted (miss real threat)
 * - Invalid lockouts corrupt memory
 * - Duplicate detection fails (wasted memory)
 * - Geofence boundary conditions (miss lockout or false trigger)
 */

#include <unity.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <cmath>
#include <cstring>
#include <cstdio>

// ============================================================================
// MOCK DEFINITIONS (before including production headers)
// ============================================================================

// Minimal Band enum (from packet_parser.h)
enum Band {
    BAND_NONE = 0,
    BAND_X = 1,
    BAND_K = 2,
    BAND_KA = 3,
    BAND_LASER = 4
};

// Minimal Lockout struct (from lockout_manager.h)
struct Lockout {
    char name[64];
    float latitude;
    float longitude;
    float radius_m;
    bool enabled;
    bool muteX;
    bool muteK;
    bool muteKa;
    bool muteLaser;
};

// Constants from lockout_manager.cpp
static constexpr float MIN_RADIUS_M = 5.0f;
static constexpr float MAX_RADIUS_M = 5000.0f;
static constexpr float DUP_EPSILON = 1e-4f;  // ~11m at equator
static constexpr size_t MAX_LOCKOUTS = 500;

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * Haversine distance calculation (same as gps_handler.cpp)
 */
static float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
    constexpr float R = 6371000.0f;  // Earth's radius in meters
    
    float dLat = (lat2 - lat1) * M_PI / 180.0f;
    float dLon = (lon2 - lon1) * M_PI / 180.0f;
    
    float a = sin(dLat / 2) * sin(dLat / 2) +
              cos(lat1 * M_PI / 180.0f) * cos(lat2 * M_PI / 180.0f) *
              sin(dLon / 2) * sin(dLon / 2);
    
    float c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

/**
 * Lockout validation logic (from lockout_manager.cpp)
 */
bool isValidLockout(const Lockout& lockout) {
    if (!std::isfinite(lockout.latitude) || !std::isfinite(lockout.longitude) || !std::isfinite(lockout.radius_m)) {
        return false;
    }
    if (lockout.latitude < -90.0f || lockout.latitude > 90.0f) return false;
    if (lockout.longitude < -180.0f || lockout.longitude > 180.0f) return false;
    if (lockout.radius_m < MIN_RADIUS_M || lockout.radius_m > MAX_RADIUS_M) return false;
    return true;
}

/**
 * Check if two lockouts are duplicates
 */
bool isDuplicate(const Lockout& a, const Lockout& b) {
    if (fabs(a.latitude - b.latitude) >= DUP_EPSILON) return false;
    if (fabs(a.longitude - b.longitude) >= DUP_EPSILON) return false;
    if (fabs(a.radius_m - b.radius_m) >= 1.0f) return false;
    if (a.muteX != b.muteX || a.muteK != b.muteK || 
        a.muteKa != b.muteKa || a.muteLaser != b.muteLaser) return false;
    return true;
}

/**
 * Check if position is inside lockout geofence
 */
bool isInsideLockout(float lat, float lon, const Lockout& lockout) {
    float dist = haversineDistance(lat, lon, lockout.latitude, lockout.longitude);
    return dist <= lockout.radius_m;
}

/**
 * Should mute alert based on band and lockout
 */
bool shouldMuteForBand(const Lockout& lockout, Band band) {
    if (!lockout.enabled) return false;
    
    switch (band) {
        case BAND_X:     return lockout.muteX;
        case BAND_K:     return lockout.muteK;
        case BAND_KA:    return lockout.muteKa;
        case BAND_LASER: return lockout.muteLaser;
        default:         return false;
    }
}

// ============================================================================
// TEST HELPERS
// ============================================================================

static Lockout createLockout(float lat, float lon, float radius, 
                             bool x, bool k, bool ka, bool laser) {
    Lockout lockout;
    memset(&lockout, 0, sizeof(lockout));
    snprintf(lockout.name, sizeof(lockout.name), "Test Lockout");
    lockout.latitude = lat;
    lockout.longitude = lon;
    lockout.radius_m = radius;
    lockout.enabled = true;
    lockout.muteX = x;
    lockout.muteK = k;
    lockout.muteKa = ka;
    lockout.muteLaser = laser;
    return lockout;
}

// ============================================================================
// TESTS: Lockout Validation
// ============================================================================

void test_isValidLockout_accepts_valid_lockout() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, true, false, false);
    TEST_ASSERT_TRUE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_nan_latitude() {
    Lockout lockout = createLockout(NAN, -122.4194f, 100.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_nan_longitude() {
    Lockout lockout = createLockout(37.7749f, NAN, 100.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_nan_radius() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, NAN, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_infinite_coordinates() {
    Lockout lockout = createLockout(INFINITY, -122.4194f, 100.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_latitude_below_minus_90() {
    Lockout lockout = createLockout(-90.1f, -122.4194f, 100.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_latitude_above_90() {
    Lockout lockout = createLockout(90.1f, -122.4194f, 100.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_longitude_below_minus_180() {
    Lockout lockout = createLockout(37.7749f, -180.1f, 100.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_longitude_above_180() {
    Lockout lockout = createLockout(37.7749f, 180.1f, 100.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_radius_below_minimum() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 4.9f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_rejects_radius_above_maximum() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 5001.0f, true, true, false, false);
    TEST_ASSERT_FALSE(isValidLockout(lockout));
}

void test_isValidLockout_accepts_boundary_latitude_minus_90() {
    Lockout lockout = createLockout(-90.0f, 0.0f, 100.0f, true, true, false, false);
    TEST_ASSERT_TRUE(isValidLockout(lockout));
}

void test_isValidLockout_accepts_boundary_latitude_90() {
    Lockout lockout = createLockout(90.0f, 0.0f, 100.0f, true, true, false, false);
    TEST_ASSERT_TRUE(isValidLockout(lockout));
}

void test_isValidLockout_accepts_boundary_radius_minimum() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 5.0f, true, true, false, false);
    TEST_ASSERT_TRUE(isValidLockout(lockout));
}

void test_isValidLockout_accepts_boundary_radius_maximum() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 5000.0f, true, true, false, false);
    TEST_ASSERT_TRUE(isValidLockout(lockout));
}

// ============================================================================
// TESTS: Duplicate Detection
// ============================================================================

void test_isDuplicate_matches_identical_lockouts() {
    Lockout a = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    Lockout b = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    TEST_ASSERT_TRUE(isDuplicate(a, b));
}

void test_isDuplicate_rejects_different_latitude() {
    Lockout a = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    Lockout b = createLockout(37.7750f, -122.4194f, 100.0f, true, false, false, false);  // ~11m away
    TEST_ASSERT_FALSE(isDuplicate(a, b));
}

void test_isDuplicate_rejects_different_longitude() {
    Lockout a = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    Lockout b = createLockout(37.7749f, -122.4196f, 100.0f, true, false, false, false);  // 0.0002 deg = ~15m at this lat
    TEST_ASSERT_FALSE(isDuplicate(a, b));
}

void test_isDuplicate_rejects_different_radius() {
    Lockout a = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    Lockout b = createLockout(37.7749f, -122.4194f, 102.0f, true, false, false, false);  // 2m different
    TEST_ASSERT_FALSE(isDuplicate(a, b));
}

void test_isDuplicate_rejects_different_band_flags() {
    Lockout a = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    Lockout b = createLockout(37.7749f, -122.4194f, 100.0f, false, true, false, false);  // K instead of X
    TEST_ASSERT_FALSE(isDuplicate(a, b));
}

void test_isDuplicate_accepts_tiny_epsilon_difference() {
    Lockout a = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    // Within epsilon (1e-4 = 0.0001 degrees ≈ 11m)
    Lockout b = createLockout(37.77495f, -122.41945f, 100.5f, true, false, false, false);
    TEST_ASSERT_TRUE(isDuplicate(a, b));
}

// ============================================================================
// TESTS: Geofence Boundaries
// ============================================================================

void test_isInsideLockout_exact_center() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    TEST_ASSERT_TRUE(isInsideLockout(37.7749f, -122.4194f, lockout));
}

void test_isInsideLockout_just_inside_radius() {
    // 100m radius at SF coordinates
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    // ~90m away (within 100m radius)
    float lat = 37.7749f + (90.0f / 111320.0f);  // ~90m north
    TEST_ASSERT_TRUE(isInsideLockout(lat, -122.4194f, lockout));
}

void test_isInsideLockout_just_outside_radius() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    // ~110m away (outside 100m radius)
    float lat = 37.7749f + (110.0f / 111320.0f);  // ~110m north
    TEST_ASSERT_FALSE(isInsideLockout(lat, -122.4194f, lockout));
}

void test_isInsideLockout_at_exact_boundary() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    // Exactly 100m away should be inside (<=)
    float lat = 37.7749f + (100.0f / 111320.0f);
    TEST_ASSERT_TRUE(isInsideLockout(lat, -122.4194f, lockout));
}

// ============================================================================
// TESTS: Band-Specific Muting
// ============================================================================

void test_shouldMuteForBand_mutes_x_only() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, false, false, false);
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_X));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_K));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_KA));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_LASER));
}

void test_shouldMuteForBand_mutes_k_only() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, false, true, false, false);
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_X));
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_K));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_KA));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_LASER));
}

void test_shouldMuteForBand_mutes_ka_only() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, false, false, true, false);
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_X));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_K));
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_KA));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_LASER));
}

void test_shouldMuteForBand_mutes_laser_only() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, false, false, false, true);
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_X));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_K));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_KA));
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_LASER));
}

void test_shouldMuteForBand_mutes_multiple_bands() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, true, true, true);
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_X));
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_K));
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_KA));
    TEST_ASSERT_TRUE(shouldMuteForBand(lockout, BAND_LASER));
}

void test_shouldMuteForBand_respects_enabled_flag() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, true, true, true);
    lockout.enabled = false;
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_X));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_K));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_KA));
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_LASER));
}

void test_shouldMuteForBand_handles_band_none() {
    Lockout lockout = createLockout(37.7749f, -122.4194f, 100.0f, true, true, true, true);
    TEST_ASSERT_FALSE(shouldMuteForBand(lockout, BAND_NONE));
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

void runAllTests() {
    // Validation tests (14 tests)
    RUN_TEST(test_isValidLockout_accepts_valid_lockout);
    RUN_TEST(test_isValidLockout_rejects_nan_latitude);
    RUN_TEST(test_isValidLockout_rejects_nan_longitude);
    RUN_TEST(test_isValidLockout_rejects_nan_radius);
    RUN_TEST(test_isValidLockout_rejects_infinite_coordinates);
    RUN_TEST(test_isValidLockout_rejects_latitude_below_minus_90);
    RUN_TEST(test_isValidLockout_rejects_latitude_above_90);
    RUN_TEST(test_isValidLockout_rejects_longitude_below_minus_180);
    RUN_TEST(test_isValidLockout_rejects_longitude_above_180);
    RUN_TEST(test_isValidLockout_rejects_radius_below_minimum);
    RUN_TEST(test_isValidLockout_rejects_radius_above_maximum);
    RUN_TEST(test_isValidLockout_accepts_boundary_latitude_minus_90);
    RUN_TEST(test_isValidLockout_accepts_boundary_latitude_90);
    RUN_TEST(test_isValidLockout_accepts_boundary_radius_minimum);
    RUN_TEST(test_isValidLockout_accepts_boundary_radius_maximum);
    
    // Duplicate detection tests (6 tests)
    RUN_TEST(test_isDuplicate_matches_identical_lockouts);
    RUN_TEST(test_isDuplicate_rejects_different_latitude);
    RUN_TEST(test_isDuplicate_rejects_different_longitude);
    RUN_TEST(test_isDuplicate_rejects_different_radius);
    RUN_TEST(test_isDuplicate_rejects_different_band_flags);
    RUN_TEST(test_isDuplicate_accepts_tiny_epsilon_difference);
    
    // Geofence boundary tests (4 tests)
    RUN_TEST(test_isInsideLockout_exact_center);
    RUN_TEST(test_isInsideLockout_just_inside_radius);
    RUN_TEST(test_isInsideLockout_just_outside_radius);
    RUN_TEST(test_isInsideLockout_at_exact_boundary);
    
    // Band-specific muting tests (8 tests)
    RUN_TEST(test_shouldMuteForBand_mutes_x_only);
    RUN_TEST(test_shouldMuteForBand_mutes_k_only);
    RUN_TEST(test_shouldMuteForBand_mutes_ka_only);
    RUN_TEST(test_shouldMuteForBand_mutes_laser_only);
    RUN_TEST(test_shouldMuteForBand_mutes_multiple_bands);
    RUN_TEST(test_shouldMuteForBand_respects_enabled_flag);
    RUN_TEST(test_shouldMuteForBand_handles_band_none);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char **argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
