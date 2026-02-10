/**
 * Auto-Lockout Manager Unit Tests
 * 
 * Tests cluster matching, frequency tolerance, promotion/demotion logic, and heading calculations.
 * These tests catch bugs where:
 * - Door openers merge with speed signs (frequency tolerance)
 * - Clusters promote too fast or too slow
 * - Heading wraparound fails at 0/360 boundary
 * - Wrong day counting (same day counted twice)
 */

#include <unity.h>
#ifdef ARDUINO
#include <Arduino.h>
#endif
#include <cmath>
#include <cstdint>
#include <ctime>
#include <vector>

// ============================================================================
// MOCK DEFINITIONS
// ============================================================================

// Band enum from packet_parser.h
enum Band {
    BAND_NONE = 0,
    BAND_X = 1,
    BAND_K = 2,
    BAND_KA = 3,
    BAND_LASER = 4
};

// Simplified AlertEvent for testing
struct AlertEvent {
    float latitude;
    float longitude;
    float heading;
    Band band;
    uint32_t frequency_khz;
    uint8_t signalStrength;
    uint16_t duration_ms;
    time_t timestamp;
    bool isMoving;
    bool isPersistent;
};

// Simplified LearningCluster for testing
struct LearningCluster {
    float centerLat;
    float centerLon;
    float radius_m;
    Band band;
    uint32_t frequency_khz;
    float frequency_tolerance_khz;
    
    std::vector<AlertEvent> events;
    
    int hitCount;
    int stoppedHitCount;
    int movingHitCount;
    time_t firstSeen;
    time_t lastSeen;
    
    int passWithoutAlertCount;
    time_t lastPassthrough;
    time_t lastCountedHit;
    time_t lastCountedMiss;
    
    float createdHeading;
    
    bool isPromoted;
    int promotedLockoutIndex;
};

// Constants from auto_lockout_manager.cpp
static constexpr float CLUSTER_RADIUS_M = 150.0f;
static constexpr int PROMOTION_TIME_WINDOW_DAYS = 2;
static constexpr float DIRECTIONAL_UNLEARN_TOLERANCE_DEG = 90.0f;
static constexpr size_t MAX_CLUSTERS = 50;
static constexpr uint8_t MIN_SIGNAL_STRENGTH = 3;

// ============================================================================
// PURE FUNCTIONS EXTRACTED FOR TESTING
// ============================================================================

/**
 * Haversine distance calculation
 */
static float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
    constexpr float R = 6371000.0f;
    float dLat = (lat2 - lat1) * M_PI / 180.0f;
    float dLon = (lon2 - lon1) * M_PI / 180.0f;
    float a = sin(dLat / 2) * sin(dLat / 2) +
              cos(lat1 * M_PI / 180.0f) * cos(lat2 * M_PI / 180.0f) *
              sin(dLon / 2) * sin(dLon / 2);
    float c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return R * c;
}

/**
 * Calculate angular difference between two headings (0-180 degrees)
 * Handles wraparound at 360 degrees correctly
 */
static float headingDifference(float h1, float h2) {
    if (h1 < 0 || h2 < 0) return 0.0f;  // Unknown heading = no check
    float diff = fabs(h1 - h2);
    if (diff > 180.0f) diff = 360.0f - diff;
    return diff;
}

/**
 * Find matching cluster based on location, band, and frequency tolerance
 * Returns -1 if no match found
 */
int findCluster(const std::vector<LearningCluster>& clusters, 
                float lat, float lon, Band band, uint32_t frequency_khz,
                float freqToleranceKHz) {
    for (size_t i = 0; i < clusters.size(); i++) {
        // Must match band
        if (clusters[i].band != band) continue;
        
        // Check frequency tolerance
        int32_t freqDiff = (int32_t)frequency_khz - (int32_t)clusters[i].frequency_khz;
        if (freqDiff < 0) freqDiff = -freqDiff;
        if ((float)freqDiff > freqToleranceKHz) continue;
        
        // Check distance to cluster center
        float dist = haversineDistance(lat, lon, clusters[i].centerLat, clusters[i].centerLon);
        if (dist <= CLUSTER_RADIUS_M) {
            return i;
        }
    }
    return -1;
}

/**
 * Count unique days in event list
 */
size_t countUniqueDays(const std::vector<AlertEvent>& events) {
    std::vector<int> uniqueDays;
    for (const auto& event : events) {
        int daysSinceEpoch = event.timestamp / (24 * 3600);
        bool found = false;
        for (int day : uniqueDays) {
            if (day == daysSinceEpoch) {
                found = true;
                break;
            }
        }
        if (!found) uniqueDays.push_back(daysSinceEpoch);
    }
    return uniqueDays.size();
}

/**
 * Check if cluster should be promoted based on hit counts and time window
 */
bool shouldPromoteCluster(const LearningCluster& cluster, int requiredHits) {
    if (cluster.isPromoted) return false;
    
    // Check hit count threshold
    bool hasEnoughStoppedHits = cluster.stoppedHitCount >= requiredHits;
    bool hasEnoughMovingHits = cluster.movingHitCount >= requiredHits;
    
    if (!hasEnoughStoppedHits && !hasEnoughMovingHits) return false;
    
    // Check time window
    time_t timeSpan = cluster.lastSeen - cluster.firstSeen;
    time_t maxTimeSpan = PROMOTION_TIME_WINDOW_DAYS * 24 * 3600;
    
    if (timeSpan > maxTimeSpan) return false;
    
    // Require alerts on at least 2 different days
    if (countUniqueDays(cluster.events) < 2) return false;
    
    return true;
}

/**
 * Check if passthrough should count as miss (directional unlearn)
 */
bool shouldCountMiss(float passthroughHeading, float clusterHeading) {
    if (passthroughHeading < 0 || clusterHeading < 0) {
        return true;  // Unknown heading = always count
    }
    float hdgDiff = headingDifference(passthroughHeading, clusterHeading);
    return hdgDiff <= DIRECTIONAL_UNLEARN_TOLERANCE_DEG;
}

// ============================================================================
// TEST HELPERS
// ============================================================================

static LearningCluster createCluster(float lat, float lon, Band band, uint32_t freq_khz) {
    LearningCluster cluster;
    cluster.centerLat = lat;
    cluster.centerLon = lon;
    cluster.radius_m = CLUSTER_RADIUS_M;
    cluster.band = band;
    cluster.frequency_khz = freq_khz;
    cluster.frequency_tolerance_khz = 8000.0f;  // Default 8 MHz
    cluster.hitCount = 0;
    cluster.stoppedHitCount = 0;
    cluster.movingHitCount = 0;
    cluster.firstSeen = 0;
    cluster.lastSeen = 0;
    cluster.passWithoutAlertCount = 0;
    cluster.lastPassthrough = 0;
    cluster.lastCountedHit = 0;
    cluster.lastCountedMiss = 0;
    cluster.createdHeading = -1.0f;
    cluster.isPromoted = false;
    cluster.promotedLockoutIndex = -1;
    return cluster;
}

static AlertEvent createEvent(time_t timestamp, bool isMoving) {
    AlertEvent event;
    event.latitude = 37.7749f;
    event.longitude = -122.4194f;
    event.heading = 90.0f;
    event.band = BAND_K;
    event.frequency_khz = 24150000;
    event.signalStrength = 5;
    event.duration_ms = 1000;
    event.timestamp = timestamp;
    event.isMoving = isMoving;
    event.isPersistent = false;
    return event;
}

// ============================================================================
// TESTS: Heading Difference (Wraparound at 360)
// ============================================================================

void test_headingDifference_same_heading() {
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, headingDifference(90.0f, 90.0f));
}

void test_headingDifference_small_difference() {
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 30.0f, headingDifference(90.0f, 120.0f));
}

void test_headingDifference_opposite_directions() {
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 180.0f, headingDifference(0.0f, 180.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 180.0f, headingDifference(90.0f, 270.0f));
}

void test_headingDifference_wraparound_350_to_10() {
    // This is the CRITICAL test - 350° to 10° should be 20°, not 340°
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 20.0f, headingDifference(350.0f, 10.0f));
}

void test_headingDifference_wraparound_10_to_350() {
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 20.0f, headingDifference(10.0f, 350.0f));
}

void test_headingDifference_unknown_heading_returns_zero() {
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, headingDifference(-1.0f, 90.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, headingDifference(90.0f, -1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 0.0f, headingDifference(-1.0f, -1.0f));
}

void test_headingDifference_180_boundary() {
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 170.0f, headingDifference(5.0f, 175.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 170.0f, headingDifference(175.0f, 5.0f));
}

// ============================================================================
// TESTS: Frequency Tolerance (Prevents Door Opener / Speed Sign Merge)
// ============================================================================

void test_findCluster_matches_within_frequency_tolerance() {
    std::vector<LearningCluster> clusters;
    LearningCluster c = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    c.frequency_tolerance_khz = 8000.0f;  // 8 MHz
    clusters.push_back(c);
    
    // Same frequency
    int idx = findCluster(clusters, 37.7749f, -122.4194f, BAND_K, 24150000, 8000.0f);
    TEST_ASSERT_EQUAL(0, idx);
    
    // 5 MHz different (within tolerance)
    idx = findCluster(clusters, 37.7749f, -122.4194f, BAND_K, 24155000, 8000.0f);
    TEST_ASSERT_EQUAL(0, idx);
}

void test_findCluster_rejects_outside_frequency_tolerance() {
    std::vector<LearningCluster> clusters;
    LearningCluster c = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    clusters.push_back(c);
    
    // 10 MHz different (outside 8 MHz tolerance)
    // This is the door opener vs speed sign case from CLAUDE.md
    int idx = findCluster(clusters, 37.7749f, -122.4194f, BAND_K, 24160000, 8000.0f);
    TEST_ASSERT_EQUAL(-1, idx);
}

void test_findCluster_door_opener_vs_speed_sign() {
    // Real-world scenario: Door opener at 24.150 GHz, speed sign at 24.125 GHz
    std::vector<LearningCluster> clusters;
    LearningCluster doorOpener = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    clusters.push_back(doorOpener);
    
    // Speed sign 25 MHz away - should NOT match with 8 MHz tolerance
    int idx = findCluster(clusters, 37.7749f, -122.4194f, BAND_K, 24125000, 8000.0f);
    TEST_ASSERT_EQUAL(-1, idx);
}

void test_findCluster_rejects_different_band() {
    std::vector<LearningCluster> clusters;
    LearningCluster c = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    clusters.push_back(c);
    
    // Same location, same frequency, different band
    int idx = findCluster(clusters, 37.7749f, -122.4194f, BAND_KA, 24150000, 8000.0f);
    TEST_ASSERT_EQUAL(-1, idx);
}

void test_findCluster_rejects_outside_distance() {
    std::vector<LearningCluster> clusters;
    LearningCluster c = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    clusters.push_back(c);
    
    // 200m away (outside 150m cluster radius)
    float lat = 37.7749f + (200.0f / 111320.0f);
    int idx = findCluster(clusters, lat, -122.4194f, BAND_K, 24150000, 8000.0f);
    TEST_ASSERT_EQUAL(-1, idx);
}

// ============================================================================
// TESTS: Promotion Logic
// ============================================================================

void test_shouldPromoteCluster_requires_minimum_hits() {
    LearningCluster cluster = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    cluster.stoppedHitCount = 2;  // Below threshold of 3
    cluster.firstSeen = 1000000;
    cluster.lastSeen = 1000000 + 3600;  // 1 hour later
    
    // Add events on different days
    cluster.events.push_back(createEvent(1000000, false));
    cluster.events.push_back(createEvent(1000000 + 86400, false));  // Next day
    
    TEST_ASSERT_FALSE(shouldPromoteCluster(cluster, 3));
}

void test_shouldPromoteCluster_promotes_at_threshold() {
    LearningCluster cluster = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    cluster.stoppedHitCount = 3;  // Exactly at threshold
    cluster.firstSeen = 1000000;
    cluster.lastSeen = 1000000 + 86400;  // 1 day later
    
    // Add events on different days
    cluster.events.push_back(createEvent(1000000, false));
    cluster.events.push_back(createEvent(1000000 + 86400, false));  // Day 2
    cluster.events.push_back(createEvent(1000000 + 86400 + 3600, false));  // Day 2 (different time)
    
    TEST_ASSERT_TRUE(shouldPromoteCluster(cluster, 3));
}

void test_shouldPromoteCluster_requires_multiple_days() {
    LearningCluster cluster = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    cluster.stoppedHitCount = 5;  // Above threshold
    cluster.firstSeen = 1000000;
    cluster.lastSeen = 1000000 + 3600;  // Same day
    
    // All events on same day
    cluster.events.push_back(createEvent(1000000, false));
    cluster.events.push_back(createEvent(1000000 + 3600, false));
    cluster.events.push_back(createEvent(1000000 + 7200, false));
    
    // Should NOT promote - all same day
    TEST_ASSERT_FALSE(shouldPromoteCluster(cluster, 3));
}

void test_shouldPromoteCluster_rejects_outside_time_window() {
    LearningCluster cluster = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    cluster.stoppedHitCount = 5;
    cluster.firstSeen = 1000000;
    cluster.lastSeen = 1000000 + (3 * 86400);  // 3 days later (outside 2-day window)
    
    // Events on different days but spread too far
    cluster.events.push_back(createEvent(1000000, false));
    cluster.events.push_back(createEvent(1000000 + (3 * 86400), false));
    
    TEST_ASSERT_FALSE(shouldPromoteCluster(cluster, 3));
}

void test_shouldPromoteCluster_already_promoted_returns_false() {
    LearningCluster cluster = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    cluster.stoppedHitCount = 10;
    cluster.isPromoted = true;
    
    TEST_ASSERT_FALSE(shouldPromoteCluster(cluster, 3));
}

void test_shouldPromoteCluster_moving_hits_count_separately() {
    LearningCluster cluster = createCluster(37.7749f, -122.4194f, BAND_K, 24150000);
    cluster.movingHitCount = 3;  // Moving hits only
    cluster.stoppedHitCount = 0;
    cluster.firstSeen = 1000000;
    cluster.lastSeen = 1000000 + 86400;
    
    // Add events on different days
    cluster.events.push_back(createEvent(1000000, true));  // Moving
    cluster.events.push_back(createEvent(1000000 + 86400, true));
    cluster.events.push_back(createEvent(1000000 + 86400 + 3600, true));
    
    TEST_ASSERT_TRUE(shouldPromoteCluster(cluster, 3));
}

// ============================================================================
// TESTS: Directional Unlearn
// ============================================================================

void test_shouldCountMiss_same_direction() {
    // Passing through in same direction should count
    TEST_ASSERT_TRUE(shouldCountMiss(90.0f, 90.0f));
}

void test_shouldCountMiss_within_tolerance() {
    // 45° difference is within 90° tolerance
    TEST_ASSERT_TRUE(shouldCountMiss(90.0f, 135.0f));
    TEST_ASSERT_TRUE(shouldCountMiss(90.0f, 45.0f));
}

void test_shouldCountMiss_rejects_opposite_direction() {
    // 180° difference is outside 90° tolerance
    TEST_ASSERT_FALSE(shouldCountMiss(90.0f, 270.0f));
    TEST_ASSERT_FALSE(shouldCountMiss(0.0f, 180.0f));
}

void test_shouldCountMiss_unknown_heading_always_counts() {
    // Unknown headings should always count (fail open)
    TEST_ASSERT_TRUE(shouldCountMiss(-1.0f, 90.0f));
    TEST_ASSERT_TRUE(shouldCountMiss(90.0f, -1.0f));
    TEST_ASSERT_TRUE(shouldCountMiss(-1.0f, -1.0f));
}

void test_shouldCountMiss_wraparound() {
    // 350° to 10° = 20° difference, within tolerance
    TEST_ASSERT_TRUE(shouldCountMiss(350.0f, 10.0f));
    TEST_ASSERT_TRUE(shouldCountMiss(10.0f, 350.0f));
}

// ============================================================================
// TESTS: Unique Day Counting
// ============================================================================

void test_countUniqueDays_single_day() {
    std::vector<AlertEvent> events;
    events.push_back(createEvent(1000000, false));
    events.push_back(createEvent(1000000 + 3600, false));  // 1 hour later, same day
    events.push_back(createEvent(1000000 + 7200, false));  // 2 hours later, same day
    
    TEST_ASSERT_EQUAL(1, countUniqueDays(events));
}

void test_countUniqueDays_two_days() {
    std::vector<AlertEvent> events;
    events.push_back(createEvent(1000000, false));
    events.push_back(createEvent(1000000 + 86400, false));  // Next day
    
    TEST_ASSERT_EQUAL(2, countUniqueDays(events));
}

void test_countUniqueDays_multiple_events_same_day() {
    std::vector<AlertEvent> events;
    // Day 1: 3 events
    events.push_back(createEvent(1000000, false));
    events.push_back(createEvent(1000000 + 3600, false));
    events.push_back(createEvent(1000000 + 7200, false));
    // Day 2: 2 events
    events.push_back(createEvent(1000000 + 86400, false));
    events.push_back(createEvent(1000000 + 86400 + 3600, false));
    
    TEST_ASSERT_EQUAL(2, countUniqueDays(events));
}

void test_countUniqueDays_empty_list() {
    std::vector<AlertEvent> events;
    TEST_ASSERT_EQUAL(0, countUniqueDays(events));
}

// ============================================================================
// TEST RUNNER
// ============================================================================

void setUp(void) {}
void tearDown(void) {}

void runAllTests() {
    // Heading difference tests (7 tests)
    RUN_TEST(test_headingDifference_same_heading);
    RUN_TEST(test_headingDifference_small_difference);
    RUN_TEST(test_headingDifference_opposite_directions);
    RUN_TEST(test_headingDifference_wraparound_350_to_10);
    RUN_TEST(test_headingDifference_wraparound_10_to_350);
    RUN_TEST(test_headingDifference_unknown_heading_returns_zero);
    RUN_TEST(test_headingDifference_180_boundary);
    
    // Frequency tolerance tests (5 tests)
    RUN_TEST(test_findCluster_matches_within_frequency_tolerance);
    RUN_TEST(test_findCluster_rejects_outside_frequency_tolerance);
    RUN_TEST(test_findCluster_door_opener_vs_speed_sign);
    RUN_TEST(test_findCluster_rejects_different_band);
    RUN_TEST(test_findCluster_rejects_outside_distance);
    
    // Promotion logic tests (6 tests)
    RUN_TEST(test_shouldPromoteCluster_requires_minimum_hits);
    RUN_TEST(test_shouldPromoteCluster_promotes_at_threshold);
    RUN_TEST(test_shouldPromoteCluster_requires_multiple_days);
    RUN_TEST(test_shouldPromoteCluster_rejects_outside_time_window);
    RUN_TEST(test_shouldPromoteCluster_already_promoted_returns_false);
    RUN_TEST(test_shouldPromoteCluster_moving_hits_count_separately);
    
    // Directional unlearn tests (5 tests)
    RUN_TEST(test_shouldCountMiss_same_direction);
    RUN_TEST(test_shouldCountMiss_within_tolerance);
    RUN_TEST(test_shouldCountMiss_rejects_opposite_direction);
    RUN_TEST(test_shouldCountMiss_unknown_heading_always_counts);
    RUN_TEST(test_shouldCountMiss_wraparound);
    
    // Unique day counting tests (4 tests)
    RUN_TEST(test_countUniqueDays_single_day);
    RUN_TEST(test_countUniqueDays_two_days);
    RUN_TEST(test_countUniqueDays_multiple_events_same_day);
    RUN_TEST(test_countUniqueDays_empty_list);
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
