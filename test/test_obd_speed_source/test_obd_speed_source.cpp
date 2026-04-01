#include <unity.h>
#include <cstdint>
#include <cmath>

// ── Minimal stub for ObdRuntimeModule ────────────────────────────
// Provides getFreshSpeed() for SpeedSourceSelector to call.

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

ObdRuntimeModule obdRuntimeModule;

#include "../../src/modules/speed/speed_source_selector.h"
#include "../../src/modules/speed/speed_source_selector.cpp"

static void resetAll() {
    speedSourceSelector = SpeedSourceSelector();
    obdRuntimeModule = ObdRuntimeModule();
    speedSourceSelector.wireSpeedSources(&obdRuntimeModule);
}

void setUp() { resetAll(); }
void tearDown() {}

static SpeedSelectorStatus snapshotAt(uint32_t nowMs) {
    return speedSourceSelector.snapshotAt(nowMs);
}

static SpeedSelectorStatus updateAndSnapshot(uint32_t nowMs) {
    speedSourceSelector.update(nowMs);
    return speedSourceSelector.snapshot();
}

// ── Enum value tests ──────────────────────────────────────────────

void test_obd_enum_value_is_3() {
    TEST_ASSERT_EQUAL(3, static_cast<uint8_t>(SpeedSource::OBD));
}

void test_source_name_obd() {
    TEST_ASSERT_EQUAL_STRING("obd", SpeedSourceSelector::sourceName(SpeedSource::OBD));
}

void test_source_name_none() {
    TEST_ASSERT_EQUAL_STRING("none", SpeedSourceSelector::sourceName(SpeedSource::NONE));
}

// ── No sources ────────────────────────────────────────────────────

void test_no_sources_enabled_selects_none() {
    speedSourceSelector.begin(false);
    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

void test_obd_enabled_no_data_selects_none() {
    speedSourceSelector.begin(true);
    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

// ── OBD only ──────────────────────────────────────────────────────

void test_obd_only_fresh_selects_obd() {
    speedSourceSelector.begin(true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 72.0f;
    obdRuntimeModule.freshTsMs = 950;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 72.0f, s.selectedSpeedMph);
    TEST_ASSERT_EQUAL(50, s.selectedAgeMs);
    TEST_ASSERT_TRUE(s.obdFresh);
}

void test_obd_disabled_not_polled() {
    speedSourceSelector.begin(false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 72.0f;
    obdRuntimeModule.freshTsMs = 950;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

// ── Speed validation (MAX_VALID_SPEED_MPH) ────────────────────────

void test_obd_over_max_speed_rejected() {
    speedSourceSelector.begin(true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 260.0f;  // > 250
    obdRuntimeModule.freshTsMs = 900;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s.selectedSource);
    TEST_ASSERT_FALSE(s.obdFresh);
}

void test_speed_at_max_is_valid() {
    speedSourceSelector.begin(true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 250.0f;  // == MAX
    obdRuntimeModule.freshTsMs = 900;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 250.0f, s.selectedSpeedMph);
}

// ── Zero speed ────────────────────────────────────────────────────

void test_obd_zero_speed_is_valid() {
    speedSourceSelector.begin(true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 0.0f;
    obdRuntimeModule.freshTsMs = 900;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s.selectedSource);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, s.selectedSpeedMph);
}

// ── Source switching counter ──────────────────────────────────────

void test_source_switch_counter_increments() {
    speedSourceSelector.begin(true);

    // First: OBD fresh → selects OBD
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 900;
    auto s1 = updateAndSnapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s1.selectedSource);
    TEST_ASSERT_EQUAL(0, s1.sourceSwitches);  // initial selection, not a switch

    // Second: OBD stale → selects NONE (OBD→NONE counts as switch)
    obdRuntimeModule.freshResult = false;
    auto s2 = updateAndSnapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s2.selectedSource);
    TEST_ASSERT_EQUAL(1, s2.sourceSwitches);  // OBD→NONE

    // Third: OBD fresh again → selects OBD (NONE→OBD does NOT count)
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshTsMs = 1400;
    auto s3 = updateAndSnapshot(1500);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s3.selectedSource);
    TEST_ASSERT_EQUAL(1, s3.sourceSwitches);  // no additional switch
}

// ── Selection counters ────────────────────────────────────────────

void test_snapshot_at_is_pure_and_update_commits_state() {
    speedSourceSelector.begin(true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 60.0f;
    obdRuntimeModule.freshTsMs = 900;

    const SpeedSelectorStatus preview = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, preview.selectedSource);
    TEST_ASSERT_EQUAL(0u, preview.obdSelections);
    TEST_ASSERT_FALSE(speedSourceSelector.selectedSpeed().valid);

    const SpeedSelectorStatus cachedBeforeUpdate = speedSourceSelector.snapshot();
    TEST_ASSERT_EQUAL(SpeedSource::NONE, cachedBeforeUpdate.selectedSource);
    TEST_ASSERT_EQUAL(0u, cachedBeforeUpdate.obdSelections);

    const SpeedSelectorStatus committed = updateAndSnapshot(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, committed.selectedSource);
    TEST_ASSERT_EQUAL(1u, committed.obdSelections);
    TEST_ASSERT_TRUE(speedSourceSelector.selectedSpeed().valid);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, speedSourceSelector.selectedSpeed().source);
}

void test_selection_counters() {
    speedSourceSelector.begin(true);

    // OBD selection
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 900;
    speedSourceSelector.update(1000);

    // No source
    obdRuntimeModule.freshResult = false;
    auto s = updateAndSnapshot(2000);

    TEST_ASSERT_EQUAL(1, s.obdSelections);
    TEST_ASSERT_EQUAL(1, s.noSourceSelections);
}

// ── Enabled input sync ────────────────────────────────────────────

void test_sync_enabled_inputs_activates_obd() {
    speedSourceSelector.begin(false);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 55.0f;
    obdRuntimeModule.freshTsMs = 900;

    auto s1 = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::NONE, s1.selectedSource);

    speedSourceSelector.syncEnabledInputs(true);
    auto s2 = snapshotAt(1000);
    TEST_ASSERT_EQUAL(SpeedSource::OBD, s2.selectedSource);
    TEST_ASSERT_TRUE(s2.obdEnabled);
}

// ── Status fields ─────────────────────────────────────────────────

void test_status_reports_enabled_flags() {
    speedSourceSelector.begin(true);
    auto s = snapshotAt(1000);
    TEST_ASSERT_TRUE(s.obdEnabled);
}

void test_age_calculation() {
    speedSourceSelector.begin(true);
    obdRuntimeModule.freshResult = true;
    obdRuntimeModule.freshSpeedMph = 62.0f;
    obdRuntimeModule.freshTsMs = 800;

    auto s = snapshotAt(1000);
    TEST_ASSERT_EQUAL(200, s.obdAgeMs);
    TEST_ASSERT_EQUAL(200, s.selectedAgeMs);  // OBD selected
}

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();

    // Enum & names
    RUN_TEST(test_obd_enum_value_is_3);
    RUN_TEST(test_source_name_obd);
    RUN_TEST(test_source_name_none);

    // No sources
    RUN_TEST(test_no_sources_enabled_selects_none);
    RUN_TEST(test_obd_enabled_no_data_selects_none);

    // OBD only
    RUN_TEST(test_obd_only_fresh_selects_obd);
    RUN_TEST(test_obd_disabled_not_polled);

    // Speed validation
    RUN_TEST(test_obd_over_max_speed_rejected);
    RUN_TEST(test_speed_at_max_is_valid);
    RUN_TEST(test_obd_zero_speed_is_valid);

    // Source switching
    RUN_TEST(test_source_switch_counter_increments);
    RUN_TEST(test_snapshot_at_is_pure_and_update_commits_state);
    RUN_TEST(test_selection_counters);

    // Enabled input sync
    RUN_TEST(test_sync_enabled_inputs_activates_obd);

    // Status fields
    RUN_TEST(test_status_reports_enabled_flags);
    RUN_TEST(test_age_calculation);

    return UNITY_END();
}
