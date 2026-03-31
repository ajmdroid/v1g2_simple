#include <unity.h>

#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
#endif

// Guard macros prevent the real module headers (included via settings_runtime_sync.h)
// from redefining these mock classes.
#define GPS_RUNTIME_MODULE_H
#define OBD_RUNTIME_MODULE_H
#define SPEED_SOURCE_SELECTOR_H
#define LOCKOUT_LEARNER_H

class GpsRuntimeModule {
public:
    void setEnabled(bool enabled) {
        ++setEnabledCalls;
        lastEnabled = enabled;
    }

    int setEnabledCalls = 0;
    bool lastEnabled = false;
};

class ObdRuntimeModule {
public:
    void setEnabled(bool enabled) {
        ++setEnabledCalls;
        lastEnabled = enabled;
    }

    void setMinRssi(int8_t minRssi) {
        ++setMinRssiCalls;
        lastMinRssi = minRssi;
    }

    int setEnabledCalls = 0;
    int setMinRssiCalls = 0;
    bool lastEnabled = false;
    int8_t lastMinRssi = 0;
};

class SpeedSourceSelector {
public:
    void syncEnabledInputs(bool gpsEnabled, bool obdEnabled) {
        ++syncEnabledInputsCalls;
        lastGpsEnabled = gpsEnabled;
        lastObdEnabled = obdEnabled;
    }

    int syncEnabledInputsCalls = 0;
    bool lastGpsEnabled = false;
    bool lastObdEnabled = false;
};

class LockoutLearner {
public:
    void setTuning(uint8_t promotionHits,
                   uint16_t radiusE5,
                   uint16_t freqToleranceMHz,
                   uint8_t learnIntervalHours,
                   uint16_t maxHdopX10,
                   uint8_t minLearnerSpeedMph) {
        ++setTuningCalls;
        lastPromotionHits = promotionHits;
        lastRadiusE5 = radiusE5;
        lastFreqToleranceMHz = freqToleranceMHz;
        lastLearnIntervalHours = learnIntervalHours;
        lastMaxHdopX10 = maxHdopX10;
        lastMinLearnerSpeedMph = minLearnerSpeedMph;
    }

    int setTuningCalls = 0;
    uint8_t lastPromotionHits = 0;
    uint16_t lastRadiusE5 = 0;
    uint16_t lastFreqToleranceMHz = 0;
    uint8_t lastLearnIntervalHours = 0;
    uint16_t lastMaxHdopX10 = 0;
    uint8_t lastMinLearnerSpeedMph = 0;
};

namespace {

int g_kaCalls = 0;
int g_kCalls = 0;
int g_xCalls = 0;
bool g_lastKaEnabled = false;
bool g_lastKEnabled = false;
bool g_lastXEnabled = false;

}  // namespace

void lockoutSetKaLearningEnabled(bool enabled) {
    ++g_kaCalls;
    g_lastKaEnabled = enabled;
}

void lockoutSetKLearningEnabled(bool enabled) {
    ++g_kCalls;
    g_lastKEnabled = enabled;
}

void lockoutSetXLearningEnabled(bool enabled) {
    ++g_xCalls;
    g_lastXEnabled = enabled;
}

#include "../../src/settings_runtime_sync.h"

void setUp() {
    g_kaCalls = 0;
    g_kCalls = 0;
    g_xCalls = 0;
    g_lastKaEnabled = false;
    g_lastKEnabled = false;
    g_lastXEnabled = false;
}

void tearDown() {}

void test_sync_gps_runtime_enabled_uses_persisted_flag() {
    V1Settings settings;
    settings.gpsEnabled = true;
    GpsRuntimeModule gpsRuntime;

    SettingsRuntimeSync::syncGpsRuntimeEnabled(settings, gpsRuntime);

    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(gpsRuntime.lastEnabled);
}

void test_sync_gps_vehicle_runtime_settings_updates_gps_and_selector() {
    V1Settings settings;
    settings.gpsEnabled = true;
    settings.obdEnabled = false;
    GpsRuntimeModule gpsRuntime;
    SpeedSourceSelector speedSourceSelector;

    SettingsRuntimeSync::syncGpsVehicleRuntimeSettings(settings,
                                                       gpsRuntime,
                                                       speedSourceSelector);

    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(gpsRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, speedSourceSelector.syncEnabledInputsCalls);
    TEST_ASSERT_TRUE(speedSourceSelector.lastGpsEnabled);
    TEST_ASSERT_FALSE(speedSourceSelector.lastObdEnabled);
}

void test_sync_obd_runtime_settings_applies_enabled_and_min_rssi() {
    V1Settings settings;
    settings.obdEnabled = true;
    settings.obdMinRssi = -55;
    ObdRuntimeModule obdRuntime;

    SettingsRuntimeSync::syncObdRuntimeSettings(settings, obdRuntime);

    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(obdRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setMinRssiCalls);
    TEST_ASSERT_EQUAL_INT8(-55, obdRuntime.lastMinRssi);
}

void test_sync_obd_vehicle_runtime_settings_updates_obd_and_selector() {
    V1Settings settings;
    settings.gpsEnabled = true;
    settings.obdEnabled = true;
    settings.obdMinRssi = -63;
    ObdRuntimeModule obdRuntime;
    SpeedSourceSelector speedSourceSelector;

    SettingsRuntimeSync::syncObdVehicleRuntimeSettings(settings,
                                                       obdRuntime,
                                                       speedSourceSelector);

    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(obdRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setMinRssiCalls);
    TEST_ASSERT_EQUAL_INT8(-63, obdRuntime.lastMinRssi);
    TEST_ASSERT_EQUAL_INT(1, speedSourceSelector.syncEnabledInputsCalls);
    TEST_ASSERT_TRUE(speedSourceSelector.lastGpsEnabled);
    TEST_ASSERT_TRUE(speedSourceSelector.lastObdEnabled);
}

void test_sync_speed_source_selector_inputs_uses_persisted_inputs() {
    V1Settings settings;
    settings.gpsEnabled = true;
    settings.obdEnabled = false;
    SpeedSourceSelector speedSourceSelector;

    SettingsRuntimeSync::syncSpeedSourceSelectorInputs(settings, speedSourceSelector);

    TEST_ASSERT_EQUAL_INT(1, speedSourceSelector.syncEnabledInputsCalls);
    TEST_ASSERT_TRUE(speedSourceSelector.lastGpsEnabled);
    TEST_ASSERT_FALSE(speedSourceSelector.lastObdEnabled);
}

void test_sync_lockout_band_learning_policy_applies_all_band_flags() {
    V1Settings settings;
    settings.gpsLockoutKaLearningEnabled = true;
    settings.gpsLockoutKLearningEnabled = false;
    settings.gpsLockoutXLearningEnabled = true;

    SettingsRuntimeSync::syncLockoutBandLearningPolicy(settings);

    TEST_ASSERT_EQUAL_INT(1, g_kaCalls);
    TEST_ASSERT_EQUAL_INT(1, g_kCalls);
    TEST_ASSERT_EQUAL_INT(1, g_xCalls);
    TEST_ASSERT_TRUE(g_lastKaEnabled);
    TEST_ASSERT_FALSE(g_lastKEnabled);
    TEST_ASSERT_TRUE(g_lastXEnabled);
}

void test_sync_vehicle_runtime_inputs_applies_gps_obd_and_selector() {
    V1Settings settings;
    settings.gpsEnabled = true;
    settings.obdEnabled = true;
    settings.obdMinRssi = -58;
    GpsRuntimeModule gpsRuntime;
    ObdRuntimeModule obdRuntime;
    SpeedSourceSelector speedSourceSelector;

    SettingsRuntimeSync::syncVehicleRuntimeInputs(settings,
                                                  gpsRuntime,
                                                  obdRuntime,
                                                  speedSourceSelector);

    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(gpsRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(obdRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, obdRuntime.setMinRssiCalls);
    TEST_ASSERT_EQUAL_INT8(-58, obdRuntime.lastMinRssi);
    TEST_ASSERT_EQUAL_INT(1, speedSourceSelector.syncEnabledInputsCalls);
    TEST_ASSERT_TRUE(speedSourceSelector.lastGpsEnabled);
    TEST_ASSERT_TRUE(speedSourceSelector.lastObdEnabled);
}

void test_sync_lockout_learner_tuning_applies_all_persisted_fields() {
    V1Settings settings;
    settings.gpsLockoutLearnerPromotionHits = 4;
    settings.gpsLockoutLearnerRadiusE5 = 200;
    settings.gpsLockoutLearnerFreqToleranceMHz = 12;
    settings.gpsLockoutLearnerLearnIntervalHours = 4;
    settings.gpsLockoutMaxHdopX10 = 80;
    settings.gpsLockoutMinLearnerSpeedMph = 7;
    LockoutLearner lockoutLearner;

    SettingsRuntimeSync::syncLockoutLearnerTuning(settings, lockoutLearner);

    TEST_ASSERT_EQUAL_INT(1, lockoutLearner.setTuningCalls);
    TEST_ASSERT_EQUAL_UINT8(4, lockoutLearner.lastPromotionHits);
    TEST_ASSERT_EQUAL_UINT16(200, lockoutLearner.lastRadiusE5);
    TEST_ASSERT_EQUAL_UINT16(12, lockoutLearner.lastFreqToleranceMHz);
    TEST_ASSERT_EQUAL_UINT8(4, lockoutLearner.lastLearnIntervalHours);
    TEST_ASSERT_EQUAL_UINT16(80, lockoutLearner.lastMaxHdopX10);
    TEST_ASSERT_EQUAL_UINT8(7, lockoutLearner.lastMinLearnerSpeedMph);
}

void test_sync_gps_lockout_runtime_settings_applies_band_policy_and_tuning() {
    V1Settings settings;
    settings.gpsLockoutKaLearningEnabled = true;
    settings.gpsLockoutKLearningEnabled = false;
    settings.gpsLockoutXLearningEnabled = true;
    settings.gpsLockoutLearnerPromotionHits = 5;
    settings.gpsLockoutLearnerRadiusE5 = 225;
    settings.gpsLockoutLearnerFreqToleranceMHz = 10;
    settings.gpsLockoutLearnerLearnIntervalHours = 12;
    settings.gpsLockoutMaxHdopX10 = 65;
    settings.gpsLockoutMinLearnerSpeedMph = 9;
    LockoutLearner lockoutLearner;

    SettingsRuntimeSync::syncGpsLockoutRuntimeSettings(settings, lockoutLearner);

    TEST_ASSERT_EQUAL_INT(1, g_kaCalls);
    TEST_ASSERT_EQUAL_INT(1, g_kCalls);
    TEST_ASSERT_EQUAL_INT(1, g_xCalls);
    TEST_ASSERT_TRUE(g_lastKaEnabled);
    TEST_ASSERT_FALSE(g_lastKEnabled);
    TEST_ASSERT_TRUE(g_lastXEnabled);
    TEST_ASSERT_EQUAL_INT(1, lockoutLearner.setTuningCalls);
    TEST_ASSERT_EQUAL_UINT8(5, lockoutLearner.lastPromotionHits);
    TEST_ASSERT_EQUAL_UINT16(225, lockoutLearner.lastRadiusE5);
    TEST_ASSERT_EQUAL_UINT16(10, lockoutLearner.lastFreqToleranceMHz);
    TEST_ASSERT_EQUAL_UINT8(12, lockoutLearner.lastLearnIntervalHours);
    TEST_ASSERT_EQUAL_UINT16(65, lockoutLearner.lastMaxHdopX10);
    TEST_ASSERT_EQUAL_UINT8(9, lockoutLearner.lastMinLearnerSpeedMph);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sync_gps_runtime_enabled_uses_persisted_flag);
    RUN_TEST(test_sync_gps_vehicle_runtime_settings_updates_gps_and_selector);
    RUN_TEST(test_sync_obd_runtime_settings_applies_enabled_and_min_rssi);
    RUN_TEST(test_sync_obd_vehicle_runtime_settings_updates_obd_and_selector);
    RUN_TEST(test_sync_speed_source_selector_inputs_uses_persisted_inputs);
    RUN_TEST(test_sync_lockout_band_learning_policy_applies_all_band_flags);
    RUN_TEST(test_sync_vehicle_runtime_inputs_applies_gps_obd_and_selector);
    RUN_TEST(test_sync_lockout_learner_tuning_applies_all_persisted_fields);
    RUN_TEST(test_sync_gps_lockout_runtime_settings_applies_band_policy_and_tuning);
    return UNITY_END();
}
