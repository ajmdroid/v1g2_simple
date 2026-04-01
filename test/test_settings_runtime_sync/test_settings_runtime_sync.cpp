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

#include "../../src/settings_runtime_sync.h"

void setUp() {}

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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_sync_gps_runtime_enabled_uses_persisted_flag);
    RUN_TEST(test_sync_gps_vehicle_runtime_settings_updates_gps_and_selector);
    RUN_TEST(test_sync_obd_runtime_settings_applies_enabled_and_min_rssi);
    RUN_TEST(test_sync_obd_vehicle_runtime_settings_updates_obd_and_selector);
    RUN_TEST(test_sync_speed_source_selector_inputs_uses_persisted_inputs);
    RUN_TEST(test_sync_vehicle_runtime_inputs_applies_gps_obd_and_selector);
    return UNITY_END();
}
