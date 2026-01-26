/**
 * test_haversine.cpp - Unit tests for GPS haversine distance calculation
 * 
 * Tests the great-circle distance formula used for lockout geofencing.
 * This is critical for correctly determining if an alert is within a lockout zone.
 */
#include <unity.h>
#include <cmath>

// Inline implementation for testing (mirrors gps_handler.cpp)
static float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
    const float R = 6371000.0f;  // Earth radius in meters
    const float PI = 3.14159265358979323846f;
    
    float dLat = (lat2 - lat1) * PI / 180.0f;
    float dLon = (lon2 - lon1) * PI / 180.0f;
    
    float a = sin(dLat/2) * sin(dLat/2) +
              cos(lat1 * PI / 180.0f) * cos(lat2 * PI / 180.0f) *
              sin(dLon/2) * sin(dLon/2);
    
    float c = 2 * atan2(sqrt(a), sqrt(1-a));
    
    return R * c;
}

// ============================================================================
// Test Cases
// ============================================================================

void test_haversine_same_point_returns_zero() {
    // Same coordinates should return 0 distance
    float d = haversineDistance(37.7749f, -122.4194f, 37.7749f, -122.4194f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, d);
}

void test_haversine_sf_to_la() {
    // San Francisco to Los Angeles is approximately 559 km
    float d = haversineDistance(37.7749f, -122.4194f, 34.0522f, -118.2437f);
    TEST_ASSERT_FLOAT_WITHIN(5000.0f, 559000.0f, d);  // Within 5km accuracy
}

void test_haversine_nyc_to_london() {
    // NYC to London is approximately 5,570 km
    float d = haversineDistance(40.7128f, -74.0060f, 51.5074f, -0.1278f);
    TEST_ASSERT_FLOAT_WITHIN(50000.0f, 5570000.0f, d);  // Within 50km accuracy
}

void test_haversine_small_distance_100m() {
    // Two points ~100m apart (typical lockout radius)
    // Moving ~0.0009 degrees latitude is roughly 100m
    float lat1 = 37.7749f;
    float lon1 = -122.4194f;
    float lat2 = lat1 + 0.0009f;  // ~100m north
    float lon2 = lon1;
    
    float d = haversineDistance(lat1, lon1, lat2, lon2);
    TEST_ASSERT_FLOAT_WITHIN(10.0f, 100.0f, d);  // Within 10m accuracy
}

void test_haversine_small_distance_50m() {
    // Two points ~50m apart (minimum lockout radius)
    float lat1 = 37.7749f;
    float lon1 = -122.4194f;
    float lat2 = lat1 + 0.00045f;  // ~50m north
    float lon2 = lon1;
    
    float d = haversineDistance(lat1, lon1, lat2, lon2);
    TEST_ASSERT_FLOAT_WITHIN(5.0f, 50.0f, d);
}

void test_haversine_equator() {
    // At equator, 1 degree longitude = ~111km
    float d = haversineDistance(0.0f, 0.0f, 0.0f, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1000.0f, 111000.0f, d);
}

void test_haversine_poles() {
    // At poles, longitude doesn't matter
    float d1 = haversineDistance(90.0f, 0.0f, 90.0f, 180.0f);
    TEST_ASSERT_FLOAT_WITHIN(1.0f, 0.0f, d1);  // Should be ~0
}

void test_haversine_negative_coordinates() {
    // Southern hemisphere / Western coordinates
    // Sydney to Auckland is ~2,156 km
    float d = haversineDistance(-33.8688f, 151.2093f, -36.8485f, 174.7633f);
    TEST_ASSERT_FLOAT_WITHIN(50000.0f, 2156000.0f, d);
}

void test_haversine_antipodal_points() {
    // Points on opposite sides of Earth should be ~20,000km (half circumference)
    float d = haversineDistance(0.0f, 0.0f, 0.0f, 180.0f);
    TEST_ASSERT_FLOAT_WITHIN(100000.0f, 20015000.0f, d);
}

void test_haversine_symmetry() {
    // Distance A→B should equal B→A
    float d1 = haversineDistance(37.7749f, -122.4194f, 34.0522f, -118.2437f);
    float d2 = haversineDistance(34.0522f, -118.2437f, 37.7749f, -122.4194f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, d1, d2);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    UNITY_BEGIN();
    
    // Core functionality
    RUN_TEST(test_haversine_same_point_returns_zero);
    RUN_TEST(test_haversine_sf_to_la);
    RUN_TEST(test_haversine_nyc_to_london);
    
    // Lockout-relevant distances (50m-200m typical)
    RUN_TEST(test_haversine_small_distance_100m);
    RUN_TEST(test_haversine_small_distance_50m);
    
    // Edge cases
    RUN_TEST(test_haversine_equator);
    RUN_TEST(test_haversine_poles);
    RUN_TEST(test_haversine_negative_coordinates);
    RUN_TEST(test_haversine_antipodal_points);
    
    // Properties
    RUN_TEST(test_haversine_symmetry);
    
    return UNITY_END();
}
