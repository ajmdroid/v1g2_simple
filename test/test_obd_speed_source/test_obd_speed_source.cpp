#include <unity.h>
#include <cstdint>
#include <cmath>

// ── Minimal stubs for GpsRuntimeModule and ObdRuntimeModule ───────
// These provide getFreshSpeed() for SpeedSourceSelector to call.

class GpsRuntimeModule {
public:
    bool freshResult = false;
    float freshSpeedMph = 0.0f;
    uint32_t freshTsMs = 0;

    bool getFreshSpeed(uint32_t /*nowMs*/, float& speedMphOut, uint32_t& tsMsOut) const {
        if (!freshResult) return false;
        speedMphOut = freshSpeedMph;
        tsMsOut = freshTsMs;
        return true;
    }
};

class ObdRuntimeModule {
public:
    bool freshResult = false;
    float freshSpeedMph = 0.0f;
    uint32_t freshTsMs = 0;

    bool getFreshSpeed(uint32_t /*nowMs*/, float& speedMphOut, uint32_t& tsMsOut) const {
        if (!freshResult) return false;
        speedMphOut = freshSpeedMph;
        tsMsOut = freshTsMs;
        return true;
    }
};

GpsRuntimeModule gpsRuntimeModule;
ObdRuntimeModule obdRuntimeModule;

#include "../../src/modules/speed/speed_source_selector.h"
#include "../../src/modules/speed/speed_source_selector.cpp"

static void resetAll() {
    speedSourceSelector = SpeedSourceSelector();
    gpsRuntimeModule = GpsRuntimeModule();
    obdRuntimeModule = ObdRuntimeModule();
}

void setUp() { resetAll(); }
void tearDown() {}

// ── Enum value tests ──────────────────────────────────────────────

void test_obd_enum_value_is_3() {
    TEST_ASSERT_EQUAL(3, static_cast<uint8_t>(SpeedSource::OBD));
}

void test_source_name_obd() {
    TEST_ASSERT_EQUAL_STRING("obd", SpeedSourceSelector::sourceName(SpeedSource::OBD));
}

void test_source_name_gps() {
    TEST_ASSERT_EQUAL_STRING("gps", SpeedSourceSelector::sourceName(SpeedSource::GPS));
}

void test_source_name_none() {
    TEST_ASSERT_EQUAL_STRING("none", SpeedSourceSelector::sourceName(SpeedSource::NONE));
}

// ── No sources ────────────────────────────────────────────────────

void test_no_sources_enabled_selects_none() {
    speedSourceSelector.begin(false, false);
    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsFresh);
    TEST_ASSERT_FALSE(s.obdFresh);
}

void test_gps_enabled_no_data_selects_none() {
    speedSourceSelector.begin(true, false);
    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsFresh);
}

void test_obd_enabled_no_data_selects_none() {
    speedSourceSelector.begin(false, true);
    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

// ── GPS only ──────────────────────────────────────────────────────

void test_gps_only_fresh_selects_gps() {
    speedSourceSelector.begin(true, false);
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 65.0f;
    gpsRuntimeModule.freshTsMs = 900;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 65.0f, s.selectedSpeedMph);
    TEST_ASSERT_EQUAL(100, s.selectedAgeMs);
    TEST_ASSERT_TRUE(s.gpsFresh);
    TEST_ASSERT_FALSE(s.obdFresh);
}

void test_gps_disabled_not_polled() {
    speedSourceSelector.begin(false, false);
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 65.0f;
    gpsRuntimeModule.freshTsMs = 900;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsFresh);
}

// ── OBD only ──────────────────────────────────────────────────────

void test_obd_only_fresh_selects_obd() {
    speedSourceSelector.begin(false, true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 72.0f;
    obdRuntimeModule.freshTsMs = 950;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 72.0f, s.selectedSpeedMph);
    TEST_ASSERT_EQUAL(50, s.selectedAgeMs);
    TEST_ASSERT_TRUE(s.obdFresh);
    TEST_ASSERT_FALSE(s.gpsFresh);
}

void test_obd_disabled_not_polled() {
    speedSourceSelector.begin(false, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 72.0f;
    obdRuntimeModule.freshTsMs = 950;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

// ── Both sources: OBD preferred ───────────────────────────────────

void test_both_fresh_prefers_obd() {
    speedSourceSelector.begin(true, true);
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 60.0f;
    gpsRuntimeModule.freshTsMs = 800;

    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 900;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 62.0f, s.selectedSpeedMph);
    TEST_ASSERT_TRUE(s.gpsFresh);
    TEST_ASSERT_TRUE(s.obdFresh);
}

void test_obd_stale_falls_back_to_gps() {
    speedSourceSelector.begin(true, true);
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 60.0f;
    gpsRuntimeModule.freshTsMs = 800;

    obdRuntimeModule.freshResult = false;  // stale

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, s.selectedSpeedMph);
    TEST_ASSERT_TRUE(s.gpsFresh);
    TEST_ASSERT_FALSE(s.obdFresh);
}

void test_gps_stale_uses_obd() {
    speedSourceSelector.begin(true, true);
    gpsRuntimeModule.freshResult = false;  // stale

    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 55.0f;
    obdRuntimeModule.freshTsMs = 950;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 55.0f, s.selectedSpeedMph);
}

void test_both_stale_selects_none() {
    speedSourceSelector.begin(true, true);
    gpsRuntimeModule.freshResult = false;
    obdRuntimeModule.freshResult = false;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
}

// ── Speed validation (MAX_VALID_SPEED_MPH) ────────────────────────

void test_gps_over_max_speed_rejected() {
    speedSourceSelector.begin(true, false);
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 260.0f;  // > 250
    gpsRuntimeModule.freshTsMs = 900;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.gpsFresh);
}

void test_obd_over_max_speed_rejected() {
    speedSourceSelector.begin(false, true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 260.0f;  // > 250
    obdRuntimeModule.freshTsMs = 900;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

void test_speed_at_max_is_valid() {
    speedSourceSelector.begin(false, true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 250.0f;  // == MAX
    obdRuntimeModule.freshTsMs = 900;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, s.selectedSpeedMph);
}

// ── Zero speed ────────────────────────────────────────────────────

void test_obd_zero_speed_is_valid() {
    speedSourceSelector.begin(false, true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 0.0f;
    obdRuntimeModule.freshTsMs = 900;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, s.selectedSpeedMph);
}

// ── Source switching counter ──────────────────────────────────────

void test_source_switch_counter_increments() {
    speedSourceSelector.begin(true, true);

    // First: GPS only
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 60.0f;
    gpsRuntimeModule.freshTsMs = 900;
    obdRuntimeModule.freshResult = false;
    auto s1 = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s1.selectedSource);
    TEST_ASSERT_EQUAL(0, s1.sourceSwitches);  // first selection, no switch

    // Second: OBD replaces GPS
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 1400;
    auto s2 = speedSourceSelector.snapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s2.selectedSource);
    TEST_ASSERT_EQUAL(1, s2.sourceSwitches);  // GPS→OBD

    // Third: still OBD, no switch
    auto s3 = speedSourceSelector.snapshot(2000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s3.selectedSource);
    TEST_ASSERT_EQUAL(1, s3.sourceSwitches);  // no change
}

void test_no_switch_from_none_to_source() {
    speedSourceSelector.begin(true, true);

    // Start with NONE
    auto s1 = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s1.selectedSource);
    TEST_ASSERT_EQUAL(0, s1.sourceSwitches);

    // Add GPS — NONE→GPS doesn't count as a switch
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 60.0f;
    gpsRuntimeModule.freshTsMs = 1400;
    auto s2 = speedSourceSelector.snapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s2.selectedSource);
    TEST_ASSERT_EQUAL(0, s2.sourceSwitches);
}

// ── Selection counters ────────────────────────────────────────────

void test_selection_counters() {
    speedSourceSelector.begin(true, true);

    // GPS selection
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 60.0f;
    gpsRuntimeModule.freshTsMs = 900;
    speedSourceSelector.snapshot(1000);

    // OBD selection
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 1400;
    speedSourceSelector.snapshot(1500);

    // No source
    gpsRuntimeModule.freshResult = false;
    obdRuntimeModule.freshResult = false;
    auto s = speedSourceSelector.snapshot(2000);

    TEST_ASSERT_EQUAL(1, s.gpsSelections);
    TEST_ASSERT_EQUAL(1, s.obdSelections);
    TEST_ASSERT_EQUAL(1, s.noSourceSelections);
}

// ── setObdEnabled / setGpsEnabled ─────────────────────────────────

void test_set_obd_enabled_activates_obd() {
    speedSourceSelector.begin(false, false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 55.0f;
    obdRuntimeModule.freshTsMs = 900;

    auto s1 = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s1.selectedSource);

    speedSourceSelector.setObdEnabled(true);
    auto s2 = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s2.selectedSource);
}

void test_set_gps_enabled_activates_gps() {
    speedSourceSelector.begin(false, false);
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 60.0f;
    gpsRuntimeModule.freshTsMs = 900;

    auto s1 = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s1.selectedSource);

    speedSourceSelector.setGpsEnabled(true);
    auto s2 = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::GPS, s2.selectedSource);
}

// ── Status fields ─────────────────────────────────────────────────

void test_status_reports_enabled_flags() {
    speedSourceSelector.begin(true, true);
    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_TRUE(s.gpsEnabled);
    TEST_ASSERT_TRUE(s.obdEnabled);
}

void test_age_calculation() {
    speedSourceSelector.begin(true, true);
    gpsRuntimeModule.freshResult = true;
    gpsRuntimeModule.freshSpeedMph = 60.0f;
    gpsRuntimeModule.freshTsMs = 700;

    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 800;

    auto s = speedSourceSelector.snapshot(1000);
    TEST_ASSERT_EQUAL(300, s.gpsAgeMs);
    TEST_ASSERT_EQUAL(200, s.obdAgeMs);
    TEST_ASSERT_EQUAL(200, s.selectedAgeMs);  // OBD selected
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // Enum & names
    RUN_TEST(test_obd_enum_value_is_3);
    RUN_TEST(test_source_name_obd);
    RUN_TEST(test_source_name_gps);
    RUN_TEST(test_source_name_none);

    // No sources
    RUN_TEST(test_no_sources_enabled_selects_none);
    RUN_TEST(test_gps_enabled_no_data_selects_none);
    RUN_TEST(test_obd_enabled_no_data_selects_none);

    // GPS only
    RUN_TEST(test_gps_only_fresh_selects_gps);
    RUN_TEST(test_gps_disabled_not_polled);

    // OBD only
    RUN_TEST(test_obd_only_fresh_selects_obd);
    RUN_TEST(test_obd_disabled_not_polled);

    // Both sources
    RUN_TEST(test_both_fresh_prefers_obd);
    RUN_TEST(test_obd_stale_falls_back_to_gps);
    RUN_TEST(test_gps_stale_uses_obd);
    RUN_TEST(test_both_stale_selects_none);

    // Speed validation
    RUN_TEST(test_gps_over_max_speed_rejected);
    RUN_TEST(test_obd_over_max_speed_rejected);
    RUN_TEST(test_speed_at_max_is_valid);
    RUN_TEST(test_obd_zero_speed_is_valid);

    // Source switching
    RUN_TEST(test_source_switch_counter_increments);
    RUN_TEST(test_no_switch_from_none_to_source);
    RUN_TEST(test_selection_counters);

    // Enable/disable
    RUN_TEST(test_set_obd_enabled_activates_obd);
    RUN_TEST(test_set_gps_enabled_activates_gps);

    // Status fields
    RUN_TEST(test_status_reports_enabled_flags);
    RUN_TEST(test_age_calculation);

    return UNITY_END();
}
