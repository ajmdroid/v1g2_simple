#include <unity.h>

#include <ArduinoJson.h>
#include <atomic>
#include <cmath>

#include "../mocks/Arduino.h"
#include "../mocks/WebServer.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

SettingsManager settingsManager;

struct PerfCounters {
    std::atomic<uint32_t> queueDrops{0};
    std::atomic<uint32_t> perfDrop{0};

    void reset() {
        queueDrops.store(0, std::memory_order_relaxed);
        perfDrop.store(0, std::memory_order_relaxed);
    }
};

// Use the canonical GpsRuntimeStatus definition — pure data, no Arduino dependency.
#include "../../src/modules/gps/gps_runtime_status.h"

class GpsRuntimeModule {
public:
    void setEnabled(bool enabled) {
        ++setEnabledCalls;
        lastEnabled = enabled;
        snapshotStatus.enabled = enabled;
    }

    void setScaffoldSample(float speedMph,
                           bool hasFix,
                           uint8_t satellites,
                           float hdop,
                           uint32_t timestampMs,
                           float latitudeDeg = NAN,
                           float longitudeDeg = NAN,
                           float courseDeg = NAN) {
        ++setScaffoldSampleCalls;
        lastScaffoldSpeedMph = speedMph;
        lastScaffoldHasFix = hasFix;
        lastScaffoldSatellites = satellites;
        lastScaffoldHdop = hdop;
        lastScaffoldTimestampMs = timestampMs;
        lastScaffoldLatitudeDeg = latitudeDeg;
        lastScaffoldLongitudeDeg = longitudeDeg;
        lastScaffoldCourseDeg = courseDeg;

        snapshotStatus.enabled = lastEnabled;
        snapshotStatus.sampleValid = true;
        snapshotStatus.hasFix = hasFix;
        snapshotStatus.stableHasFix = hasFix;
        snapshotStatus.speedMph = speedMph;
        snapshotStatus.satellites = satellites;
        snapshotStatus.stableSatellites = satellites;
        snapshotStatus.hdop = hdop;
        snapshotStatus.sampleTsMs = timestampMs;
        snapshotStatus.sampleAgeMs = 0;
        snapshotStatus.fixAgeMs = 0;
        snapshotStatus.stableFixAgeMs = 0;
        snapshotStatus.injectedSamples += 1;
        snapshotStatus.locationValid = std::isfinite(latitudeDeg) && std::isfinite(longitudeDeg);
        snapshotStatus.latitudeDeg = latitudeDeg;
        snapshotStatus.longitudeDeg = longitudeDeg;
        snapshotStatus.courseValid = std::isfinite(courseDeg);
        snapshotStatus.courseDeg = courseDeg;
        snapshotStatus.courseSampleTsMs = snapshotStatus.courseValid ? timestampMs : 0;
        snapshotStatus.courseAgeMs = snapshotStatus.courseValid ? 0 : UINT32_MAX;
    }

    GpsRuntimeStatus snapshot(uint32_t nowMs) const {
        ++snapshotCalls;
        lastSnapshotNowMs = nowMs;
        return snapshotStatus;
    }

    int setEnabledCalls = 0;
    bool lastEnabled = false;
    int setScaffoldSampleCalls = 0;
    float lastScaffoldSpeedMph = 0.0f;
    bool lastScaffoldHasFix = false;
    uint8_t lastScaffoldSatellites = 0;
    float lastScaffoldHdop = NAN;
    uint32_t lastScaffoldTimestampMs = 0;
    float lastScaffoldLatitudeDeg = NAN;
    float lastScaffoldLongitudeDeg = NAN;
    float lastScaffoldCourseDeg = NAN;
    mutable int snapshotCalls = 0;
    mutable uint32_t lastSnapshotNowMs = 0;
    GpsRuntimeStatus snapshotStatus = {};
};

enum class SpeedSource : uint8_t {
    NONE = 0,
    GPS = 2,
    OBD = 3
};

struct SpeedSelectorStatus {
    bool gpsEnabled = false;
    bool obdEnabled = false;
    SpeedSource selectedSource = SpeedSource::NONE;
    float selectedSpeedMph = 0.0f;
    uint32_t selectedAgeMs = UINT32_MAX;
    bool gpsFresh = false;
    float gpsSpeedMph = 0.0f;
    uint32_t gpsAgeMs = UINT32_MAX;
    bool obdFresh = false;
    float obdSpeedMph = 0.0f;
    uint32_t obdAgeMs = UINT32_MAX;
    uint32_t sourceSwitches = 0;
    uint32_t gpsSelections = 0;
    uint32_t obdSelections = 0;
    uint32_t noSourceSelections = 0;
};

class SpeedSourceSelector {
public:
    static constexpr float MAX_VALID_SPEED_MPH = 250.0f;

    void syncEnabledInputs(bool gpsEnabled, bool obdEnabled) {
        ++syncEnabledInputsCalls;
        lastGpsEnabled = gpsEnabled;
        lastObdEnabled = obdEnabled;
        snapshotStatus.gpsEnabled = gpsEnabled;
        snapshotStatus.obdEnabled = obdEnabled;
    }

    SpeedSelectorStatus snapshot() const {
        ++snapshotCalls;
        return snapshotStatus;
    }

    static const char* sourceName(SpeedSource source) {
        switch (source) {
            case SpeedSource::GPS: return "gps";
            case SpeedSource::OBD: return "obd";
            case SpeedSource::NONE:
            default:
                return "none";
        }
    }

    int syncEnabledInputsCalls = 0;
    bool lastGpsEnabled = false;
    bool lastObdEnabled = false;
    mutable int snapshotCalls = 0;
    SpeedSelectorStatus snapshotStatus = {};
};

class LockoutLearner {
public:
    void setTuning(uint8_t promotionHits,
                   uint16_t radiusE5,
                   uint16_t freqToleranceMHz,
                   uint8_t learnIntervalHours = 0,
                   uint16_t maxHdopX10 = 0,
                   uint8_t minLearnerSpeedMph = 0) {
        ++setTuningCalls;
        promotionHits_ = promotionHits;
        radiusE5_ = radiusE5;
        freqToleranceMHz_ = freqToleranceMHz;
        learnIntervalHours_ = learnIntervalHours;
        lastMaxHdopX10 = maxHdopX10;
        lastMinLearnerSpeedMph = minLearnerSpeedMph;
    }

    uint8_t promotionHits() const { return promotionHits_; }
    uint16_t radiusE5() const { return radiusE5_; }
    uint16_t freqToleranceMHz() const { return freqToleranceMHz_; }
    uint8_t learnIntervalHours() const { return learnIntervalHours_; }

    void reset(uint8_t promotionHits,
               uint16_t radiusE5,
               uint16_t freqToleranceMHz,
               uint8_t learnIntervalHours) {
        setTuningCalls = 0;
        promotionHits_ = promotionHits;
        radiusE5_ = radiusE5;
        freqToleranceMHz_ = freqToleranceMHz;
        learnIntervalHours_ = learnIntervalHours;
        lastMaxHdopX10 = 0;
        lastMinLearnerSpeedMph = 0;
    }

    int setTuningCalls = 0;
    uint16_t lastMaxHdopX10 = 0;
    uint8_t lastMinLearnerSpeedMph = 0;

private:
    uint8_t promotionHits_ = LOCKOUT_LEARNER_HITS_DEFAULT;
    uint16_t radiusE5_ = LOCKOUT_LEARNER_RADIUS_E5_DEFAULT;
    uint16_t freqToleranceMHz_ = LOCKOUT_LEARNER_FREQ_TOL_DEFAULT;
    uint8_t learnIntervalHours_ = LOCKOUT_LEARNER_LEARN_INTERVAL_HOURS_DEFAULT;
};

#include "../../src/modules/gps/gps_lockout_safety.h"
#include "../../src/modules/gps/gps_lockout_safety.cpp"
#include "../../src/modules/gps/gps_observation_log.h"
#include "../../src/modules/gps/gps_observation_log.cpp"
#include "../../src/modules/system/system_event_bus.h"

namespace {

bool lockoutKaLearningEnabledState = false;
bool lockoutKLearningEnabledState = true;
bool lockoutXLearningEnabledState = true;
int lockoutSetKaLearningEnabledCalls = 0;
int lockoutSetKLearningEnabledCalls = 0;
int lockoutSetXLearningEnabledCalls = 0;

void resetBandPolicyState() {
    lockoutKaLearningEnabledState = false;
    lockoutKLearningEnabledState = true;
    lockoutXLearningEnabledState = true;
    lockoutSetKaLearningEnabledCalls = 0;
    lockoutSetKLearningEnabledCalls = 0;
    lockoutSetXLearningEnabledCalls = 0;
}

GpsRuntimeModule gpsRuntime;
SpeedSourceSelector speedSelector;
LockoutLearner lockoutLearner;
GpsObservationLog gpsLog;
PerfCounters perfCounters;
SystemEventBus eventBus;

void setTime(unsigned long nowMs) {
    mockMillis = nowMs;
    mockMicros = nowMs * 1000;
}

void resetGpsRuntimeStatus() {
    gpsRuntime = GpsRuntimeModule{};
    gpsRuntime.snapshotStatus.hdop = NAN;
    gpsRuntime.snapshotStatus.sampleAgeMs = UINT32_MAX;
    gpsRuntime.snapshotStatus.fixAgeMs = UINT32_MAX;
    gpsRuntime.snapshotStatus.stableFixAgeMs = UINT32_MAX;
    gpsRuntime.snapshotStatus.courseAgeMs = UINT32_MAX;
    gpsRuntime.snapshotStatus.latitudeDeg = NAN;
    gpsRuntime.snapshotStatus.longitudeDeg = NAN;
    gpsRuntime.snapshotStatus.courseDeg = NAN;
}

void parseBody(const WebServer& server, JsonDocument& doc) {
    const DeserializationError error = deserializeJson(doc, server.lastBody.c_str());
    TEST_ASSERT_FALSE_MESSAGE(error, server.lastBody.c_str());
}

GpsObservation makeObservation(uint32_t tsMs,
                               bool hasFix,
                               float speedMph,
                               uint8_t satellites,
                               float hdop,
                               bool locationValid,
                               float latitudeDeg,
                               float longitudeDeg) {
    GpsObservation observation;
    observation.tsMs = tsMs;
    observation.hasFix = hasFix;
    observation.speedValid = true;
    observation.speedMph = speedMph;
    observation.satellites = satellites;
    observation.hdop = hdop;
    observation.locationValid = locationValid;
    observation.latitudeDeg = latitudeDeg;
    observation.longitudeDeg = longitudeDeg;
    return observation;
}

}  // namespace

uint8_t lockoutSupportedBandMask() {
    return static_cast<uint8_t>(0x02 | 0x04 | 0x08);
}

bool lockoutKaLearningEnabled() {
    return lockoutKaLearningEnabledState;
}

bool lockoutKLearningEnabled() {
    return lockoutKLearningEnabledState;
}

bool lockoutXLearningEnabled() {
    return lockoutXLearningEnabledState;
}

void lockoutSetKaLearningEnabled(bool enabled) {
    ++lockoutSetKaLearningEnabledCalls;
    lockoutKaLearningEnabledState = enabled;
}

void lockoutSetKLearningEnabled(bool enabled) {
    ++lockoutSetKLearningEnabledCalls;
    lockoutKLearningEnabledState = enabled;
}

void lockoutSetXLearningEnabled(bool enabled) {
    ++lockoutSetXLearningEnabledCalls;
    lockoutXLearningEnabledState = enabled;
}

uint8_t lockoutSanitizeBandMask(uint8_t bandMask) {
    return bandMask & lockoutSupportedBandMask();
}

bool lockoutBandSupported(uint8_t bandMask) {
    return (bandMask & lockoutSupportedBandMask()) != 0;
}

#include "../../src/modules/gps/gps_api_service.cpp"
#include "../../src/modules/gps/gps_api_config_service.cpp"

void setUp() {
    setTime(1000);

    settingsManager = SettingsManager{};
    settingsManager.settings.gpsEnabled = true;
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_SHADOW;
    settingsManager.settings.gpsLockoutCoreGuardEnabled = true;
    settingsManager.settings.gpsLockoutMaxQueueDrops = 1;
    settingsManager.settings.gpsLockoutMaxPerfDrops = 2;
    settingsManager.settings.gpsLockoutMaxEventBusDrops = 3;
    settingsManager.settings.gpsLockoutLearnerPromotionHits = LOCKOUT_LEARNER_HITS_DEFAULT;
    settingsManager.settings.gpsLockoutLearnerRadiusE5 = LOCKOUT_LEARNER_RADIUS_E5_DEFAULT;
    settingsManager.settings.gpsLockoutLearnerFreqToleranceMHz = LOCKOUT_LEARNER_FREQ_TOL_DEFAULT;
    settingsManager.settings.gpsLockoutLearnerLearnIntervalHours = LOCKOUT_LEARNER_LEARN_INTERVAL_HOURS_DEFAULT;
    settingsManager.settings.gpsLockoutKaLearningEnabled = false;
    settingsManager.settings.gpsLockoutKLearningEnabled = true;
    settingsManager.settings.gpsLockoutXLearningEnabled = true;
    settingsManager.settings.gpsLockoutPreQuiet = false;
    settingsManager.settings.gpsLockoutPreQuietBufferE5 = LOCKOUT_PRE_QUIET_BUFFER_E5_DEFAULT;
    settingsManager.settings.gpsLockoutMaxHdopX10 = LOCKOUT_GPS_MAX_HDOP_X10_DEFAULT;
    settingsManager.settings.gpsLockoutMinLearnerSpeedMph = LOCKOUT_GPS_MIN_LEARNER_SPEED_MPH_DEFAULT;

    resetGpsRuntimeStatus();
    speedSelector = SpeedSourceSelector{};
    lockoutLearner.reset(settingsManager.settings.gpsLockoutLearnerPromotionHits,
                         settingsManager.settings.gpsLockoutLearnerRadiusE5,
                         settingsManager.settings.gpsLockoutLearnerFreqToleranceMHz,
                         settingsManager.settings.gpsLockoutLearnerLearnIntervalHours);
    gpsLog.reset();
    perfCounters.reset();
    eventBus.reset();
    resetBandPolicyState();
}


void tearDown() {}

namespace {

struct RateLimitCtx {
    int calls = 0;
    bool allow = true;
};

struct UiActivityCtx {
    int calls = 0;
};

static bool doRateLimit(void* ctx) {
    auto* c = static_cast<RateLimitCtx*>(ctx);
    c->calls++;
    return c->allow;
}

static void doUiActivity(void* ctx) {
    static_cast<UiActivityCtx*>(ctx)->calls++;
}

}  // namespace

void test_handle_api_status_marks_ui_activity_and_returns_real_status_payload() {
    WebServer server(80);
    UiActivityCtx uiCtx;

    gpsRuntime.snapshotStatus.enabled = true;
    gpsRuntime.snapshotStatus.sampleValid = true;
    gpsRuntime.snapshotStatus.hasFix = true;
    gpsRuntime.snapshotStatus.stableHasFix = true;
    gpsRuntime.snapshotStatus.speedMph = 42.5f;
    gpsRuntime.snapshotStatus.satellites = 7;
    gpsRuntime.snapshotStatus.stableSatellites = 6;
    gpsRuntime.snapshotStatus.hdop = NAN;
    gpsRuntime.snapshotStatus.locationValid = true;
    gpsRuntime.snapshotStatus.latitudeDeg = 42.1234f;
    gpsRuntime.snapshotStatus.longitudeDeg = -71.5678f;
    gpsRuntime.snapshotStatus.sampleTsMs = 950;
    gpsRuntime.snapshotStatus.sampleAgeMs = 50;
    gpsRuntime.snapshotStatus.fixAgeMs = 50;
    gpsRuntime.snapshotStatus.stableFixAgeMs = UINT32_MAX;
    gpsRuntime.snapshotStatus.injectedSamples = 3;
    gpsRuntime.snapshotStatus.moduleDetected = true;
    gpsRuntime.snapshotStatus.bytesRead = 128;
    gpsRuntime.snapshotStatus.sentencesSeen = 8;
    gpsRuntime.snapshotStatus.sentencesParsed = 7;
    gpsRuntime.snapshotStatus.parseFailures = 1;
    gpsRuntime.snapshotStatus.lastSentenceTsMs = 990;

    speedSelector.snapshotStatus.gpsEnabled = true;
    speedSelector.snapshotStatus.gpsFresh = true;
    speedSelector.snapshotStatus.selectedSource = SpeedSource::GPS;
    speedSelector.snapshotStatus.selectedSpeedMph = 42.5f;
    speedSelector.snapshotStatus.selectedAgeMs = 25;
    speedSelector.snapshotStatus.gpsAgeMs = 25;
    speedSelector.snapshotStatus.sourceSwitches = 2;

    perfCounters.queueDrops.store(2, std::memory_order_relaxed);
    gpsLog.publish(makeObservation(900, true, 30.0f, 5, 1.2f, true, 41.0f, -70.0f));
    gpsLog.publish(makeObservation(950, true, 42.5f, 7, 0.9f, true, 42.1234f, -71.5678f));

    GpsApiService::handleApiStatus(
        server,
        gpsRuntime,
        speedSelector,
        settingsManager,
        gpsLog,
        lockoutLearner,
        perfCounters,
        eventBus,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.snapshotCalls);
    TEST_ASSERT_EQUAL_INT(1, speedSelector.snapshotCalls);

    JsonDocument doc;
    parseBody(server, doc);

    TEST_ASSERT_TRUE(doc["enabled"].as<bool>());
    TEST_ASSERT_TRUE(doc["runtimeEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("runtime", doc["mode"].as<const char*>());
    TEST_ASSERT_TRUE(doc["sampleValid"].as<bool>());
    TEST_ASSERT_TRUE(doc["hasFix"].as<bool>());
    TEST_ASSERT_TRUE(doc["stableHasFix"].as<bool>());
    TEST_ASSERT_TRUE(doc["hdop"].isNull());
    TEST_ASSERT_TRUE(doc["locationValid"].as<bool>());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 42.1234f, doc["latitude"].as<float>());
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -71.5678f, doc["longitude"].as<float>());
    TEST_ASSERT_EQUAL_UINT32(3, doc["injectedSamples"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["observations"]["published"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["observations"]["size"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("gps", doc["speedSource"]["selected"].as<const char*>());
    TEST_ASSERT_TRUE(doc["lockout"]["coreGuardTripped"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("queueDrops", doc["lockout"]["coreGuardReason"].as<const char*>());
    TEST_ASSERT_FALSE(doc["gpsLockoutKaLearningEnabled"].as<bool>());
    TEST_ASSERT_FALSE(doc["gpsLockoutPreQuiet"].as<bool>());
}

void test_handle_api_observations_rate_limited_short_circuits() {
    WebServer server(80);
    RateLimitCtx rlCtx{ .allow = false };
    UiActivityCtx uiCtx;

    GpsApiService::handleApiObservations(
        server,
        gpsLog,
        doRateLimit, &rlCtx,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_observations_returns_real_recent_samples() {
    WebServer server(80);
    RateLimitCtx rlCtx;
    UiActivityCtx uiCtx;

    gpsLog.publish(makeObservation(800, true, 15.0f, 4, 1.5f, false, NAN, NAN));
    gpsLog.publish(makeObservation(900, true, 35.5f, 6, 0.8f, true, 40.1f, -73.2f));
    server.setArg("limit", "1");

    GpsApiService::handleApiObservations(
        server,
        gpsLog,
        doRateLimit, &rlCtx,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);

    JsonDocument doc;
    parseBody(server, doc);

    TEST_ASSERT_TRUE(doc["success"].as<bool>());
    TEST_ASSERT_EQUAL_UINT32(1, doc["count"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["published"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(2, doc["size"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(900, doc["samples"][0]["tsMs"].as<uint32_t>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 35.5f, doc["samples"][0]["speedMph"].as<float>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 40.1f, doc["samples"][0]["latitude"].as<float>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -73.2f, doc["samples"][0]["longitude"].as<float>());
}

void test_handle_api_config_get_marks_ui_activity_and_returns_real_config() {
    WebServer server(80);
    UiActivityCtx uiCtx;

    settingsManager.settings.gpsEnabled = false;
    settingsManager.settings.gpsLockoutMode = LOCKOUT_RUNTIME_ENFORCE;
    settingsManager.settings.gpsLockoutPreQuiet = true;
    settingsManager.settings.gpsLockoutLearnerPromotionHits = 4;
    settingsManager.settings.gpsLockoutLearnerRadiusE5 = 200;
    settingsManager.settings.gpsLockoutLearnerFreqToleranceMHz = 12;
    settingsManager.settings.gpsLockoutKLearningEnabled = false;

    GpsApiService::handleApiConfigGet(
        server,
        settingsManager,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);

    JsonDocument doc;
    parseBody(server, doc);

    TEST_ASSERT_TRUE(doc["success"].as<bool>());
    TEST_ASSERT_FALSE(doc["enabled"].as<bool>());
    TEST_ASSERT_FALSE(doc["gpsEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("ENFORCE", doc["lockout"]["mode"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(LOCKOUT_RUNTIME_ENFORCE), doc["gpsLockoutMode"].as<int>());
    TEST_ASSERT_EQUAL_UINT32(4, doc["lockout"]["learnerPromotionHits"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(200, doc["lockout"]["learnerRadiusE5"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(12, doc["lockout"]["learnerFreqToleranceMHz"].as<uint32_t>());
    TEST_ASSERT_FALSE(doc["lockout"]["kLearningEnabled"].as<bool>());
    TEST_ASSERT_TRUE(doc["lockout"]["preQuiet"].as<bool>());
}

void test_handle_api_config_rate_limited_short_circuits() {
    WebServer server(80);
    RateLimitCtx rlCtx{ .allow = false };
    UiActivityCtx uiCtx;

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        doRateLimit, &rlCtx,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, rlCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(0, server.lastStatusCode);
}

void test_handle_api_config_rejects_invalid_json() {
    WebServer server(80);
    UiActivityCtx uiCtx;

    server.setArg("plain", "{bad json");

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [](void* /*ctx*/) { return true; }, nullptr,
        doUiActivity, &uiCtx);

    TEST_ASSERT_EQUAL_INT(1, uiCtx.calls);
    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("{\"success\":false,\"message\":\"Invalid JSON\"}", server.lastBody.c_str());
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);
}

void test_handle_api_config_requires_enabled_or_lockout_update() {
    WebServer server(80);

    server.setArg("plain", "{}");

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [](void* /*ctx*/) { return true; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING(
        "{\"success\":false,\"message\":\"Missing enabled or lockout settings\"}",
        server.lastBody.c_str());
}

void test_handle_api_config_rejects_out_of_range_speed() {
    WebServer server(80);

    server.setArg("plain", "{\"enabled\":true,\"speedMph\":251}");

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [](void* /*ctx*/) { return true; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING("{\"success\":false,\"message\":\"speedMph out of range\"}", server.lastBody.c_str());
    TEST_ASSERT_EQUAL_INT(0, gpsRuntime.setScaffoldSampleCalls);
}

void test_handle_api_config_rejects_partial_coordinates() {
    WebServer server(80);

    server.setArg("plain", "{\"enabled\":true,\"speedMph\":35,\"latitude\":42.1}");

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [](void* /*ctx*/) { return true; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING(
        "{\"success\":false,\"message\":\"latitude and longitude must be provided together\"}",
        server.lastBody.c_str());
}

void test_handle_api_config_rejects_out_of_range_coordinates() {
    WebServer server(80);

    server.setArg("plain", "{\"enabled\":true,\"speedMph\":35,\"latitude\":91.0,\"longitude\":-71.0}");

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [](void* /*ctx*/) { return true; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(400, server.lastStatusCode);
    TEST_ASSERT_EQUAL_STRING(
        "{\"success\":false,\"message\":\"latitude/longitude out of range\"}",
        server.lastBody.c_str());
}

void test_handle_api_config_lockout_only_update_mutates_real_settings_path() {
    WebServer server(80);

    server.setArg(
        "plain",
        "{\"gpsLockoutMode\":\"enforce\",\"gpsLockoutLearnerPromotionHits\":4,"
        "\"gpsLockoutLearnerRadiusE5\":200,\"gpsLockoutLearnerFreqToleranceMHz\":12,"
        "\"gpsLockoutLearnerLearnIntervalHours\":4,\"gpsLockoutMaxHdopX10\":80,"
        "\"gpsLockoutMinLearnerSpeedMph\":7,\"gpsLockoutKaLearningEnabled\":true,"
        "\"gpsLockoutKLearningEnabled\":false,\"gpsLockoutXLearningEnabled\":false,"
        "\"gpsLockoutPreQuiet\":true}");

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [](void* /*ctx*/) { return true; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_EQUAL(LOCKOUT_RUNTIME_ENFORCE, settingsManager.settings.gpsLockoutMode);
    TEST_ASSERT_EQUAL_UINT8(4, settingsManager.settings.gpsLockoutLearnerPromotionHits);
    TEST_ASSERT_EQUAL_UINT16(200, settingsManager.settings.gpsLockoutLearnerRadiusE5);
    TEST_ASSERT_EQUAL_UINT16(12, settingsManager.settings.gpsLockoutLearnerFreqToleranceMHz);
    TEST_ASSERT_EQUAL_UINT8(4, settingsManager.settings.gpsLockoutLearnerLearnIntervalHours);
    TEST_ASSERT_TRUE(settingsManager.settings.gpsLockoutKaLearningEnabled);
    TEST_ASSERT_FALSE(settingsManager.settings.gpsLockoutKLearningEnabled);
    TEST_ASSERT_FALSE(settingsManager.settings.gpsLockoutXLearningEnabled);
    TEST_ASSERT_TRUE(settingsManager.settings.gpsLockoutPreQuiet);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.requestDeferredPersistCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.setGpsEnabledCalls);
    TEST_ASSERT_EQUAL_INT(0, gpsRuntime.setEnabledCalls);
    TEST_ASSERT_EQUAL_INT(0, speedSelector.syncEnabledInputsCalls);
    TEST_ASSERT_EQUAL_INT(1, lockoutLearner.setTuningCalls);
    TEST_ASSERT_EQUAL_UINT16(80, lockoutLearner.lastMaxHdopX10);
    TEST_ASSERT_EQUAL_UINT8(7, lockoutLearner.lastMinLearnerSpeedMph);
    TEST_ASSERT_EQUAL_INT(1, lockoutSetKaLearningEnabledCalls);
    TEST_ASSERT_EQUAL_INT(1, lockoutSetKLearningEnabledCalls);
    TEST_ASSERT_EQUAL_INT(1, lockoutSetXLearningEnabledCalls);

    JsonDocument doc;
    parseBody(server, doc);
    TEST_ASSERT_TRUE(doc["success"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("ENFORCE", doc["lockout"]["mode"].as<const char*>());
    TEST_ASSERT_TRUE(doc["lockout"]["kaLearningEnabled"].as<bool>());
    TEST_ASSERT_FALSE(doc["lockout"]["kLearningEnabled"].as<bool>());
    TEST_ASSERT_FALSE(doc["lockout"]["xLearningEnabled"].as<bool>());
    TEST_ASSERT_TRUE(doc["lockout"]["preQuiet"].as<bool>());
}

void test_handle_api_config_enabled_scaffold_sample_updates_runtime_and_selector() {
    WebServer server(80);

    speedSelector.snapshotStatus.selectedSource = SpeedSource::GPS;
    speedSelector.snapshotStatus.selectedSpeedMph = 55.5f;
    speedSelector.snapshotStatus.selectedAgeMs = 0;
    gpsRuntime.snapshotStatus.enabled = false;

    server.setArg(
        "plain",
        "{\"enabled\":true,\"speedMph\":55.5,\"hasFix\":true,\"satellites\":6,"
        "\"hdop\":1.5,\"latitude\":42.1,\"longitude\":-71.2}");

    GpsApiService::handleApiConfig(
        server,
        settingsManager,
        gpsRuntime,
        speedSelector,
        lockoutLearner,
        gpsLog,
        perfCounters,
        eventBus,
        [](void* /*ctx*/) { return true; }, nullptr,
        nullptr, nullptr);

    TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
    TEST_ASSERT_TRUE(settingsManager.settings.gpsEnabled);
    TEST_ASSERT_EQUAL_INT(1, settingsManager.setGpsEnabledCalls);
    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.setEnabledCalls);
    TEST_ASSERT_TRUE(gpsRuntime.lastEnabled);
    TEST_ASSERT_EQUAL_INT(1, gpsRuntime.setScaffoldSampleCalls);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 55.5f, gpsRuntime.lastScaffoldSpeedMph);
    TEST_ASSERT_TRUE(gpsRuntime.lastScaffoldHasFix);
    TEST_ASSERT_EQUAL_UINT8(6, gpsRuntime.lastScaffoldSatellites);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.5f, gpsRuntime.lastScaffoldHdop);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 42.1f, gpsRuntime.lastScaffoldLatitudeDeg);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -71.2f, gpsRuntime.lastScaffoldLongitudeDeg);
    TEST_ASSERT_EQUAL_INT(1, speedSelector.syncEnabledInputsCalls);
    TEST_ASSERT_TRUE(speedSelector.lastGpsEnabled);
    TEST_ASSERT_FALSE(speedSelector.lastObdEnabled);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.saveCalls);
    TEST_ASSERT_EQUAL_INT(0, settingsManager.requestDeferredPersistCalls);

    JsonDocument doc;
    parseBody(server, doc);
    TEST_ASSERT_TRUE(doc["success"].as<bool>());
    TEST_ASSERT_TRUE(doc["enabled"].as<bool>());
    TEST_ASSERT_TRUE(doc["runtimeEnabled"].as<bool>());
    TEST_ASSERT_TRUE(doc["sampleValid"].as<bool>());
    TEST_ASSERT_TRUE(doc["hasFix"].as<bool>());
    TEST_ASSERT_TRUE(doc["locationValid"].as<bool>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 42.1f, doc["latitude"].as<float>());
    TEST_ASSERT_FLOAT_WITHIN(0.001f, -71.2f, doc["longitude"].as<float>());
    TEST_ASSERT_EQUAL_STRING("gps", doc["speedSource"].as<const char*>());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_api_status_marks_ui_activity_and_returns_real_status_payload);
    RUN_TEST(test_handle_api_observations_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_observations_returns_real_recent_samples);
    RUN_TEST(test_handle_api_config_get_marks_ui_activity_and_returns_real_config);
    RUN_TEST(test_handle_api_config_rate_limited_short_circuits);
    RUN_TEST(test_handle_api_config_rejects_invalid_json);
    RUN_TEST(test_handle_api_config_requires_enabled_or_lockout_update);
    RUN_TEST(test_handle_api_config_rejects_out_of_range_speed);
    RUN_TEST(test_handle_api_config_rejects_partial_coordinates);
    RUN_TEST(test_handle_api_config_rejects_out_of_range_coordinates);
    RUN_TEST(test_handle_api_config_lockout_only_update_mutates_real_settings_path);
    RUN_TEST(test_handle_api_config_enabled_scaffold_sample_updates_runtime_and_selector);
    return UNITY_END();
}
