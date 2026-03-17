#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/ble_client.h"
#include "../mocks/display.h"
#include "../mocks/packet_parser.h"
#include "../mocks/settings.h"

#ifndef ARDUINO
SerialClass Serial;
SettingsManager settingsManager;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/audio_beep.h"
#include "../../src/modules/alert_persistence/alert_persistence_module.h"
#include "../../src/modules/voice/voice_module.h"
#include "../../src/perf_metrics.h"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordDisplayRenderUs(uint32_t /*us*/) {}

AlertPersistenceModule::AlertPersistenceModule() = default;

void AlertPersistenceModule::begin(V1BLEClient* ble,
                                   PacketParser* parserRef,
                                   V1Display* displayRef,
                                   SettingsManager* settingsRef) {
    bleClient = ble;
    parser = parserRef;
    display = displayRef;
    settings = settingsRef;
    initialized = true;
}

void AlertPersistenceModule::update() {}

void AlertPersistenceModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert = alert;
}

void AlertPersistenceModule::startPersistence(unsigned long now) {
    if (!alertPersistenceActive) {
        alertClearedTime = now;
        alertPersistenceActive = true;
    }
}

void AlertPersistenceModule::clearPersistence() {
    alertPersistenceActive = false;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive &&
           persistedAlert.isValid &&
           (now - alertClearedTime) < persistMs;
}

VoiceModule::VoiceModule() = default;

void VoiceModule::begin(SettingsManager* settingsRef, V1BLEClient* ble) {
    settings = settingsRef;
    bleClient = ble;
}

VoiceAction VoiceModule::process(const VoiceContext& /*ctx*/) {
    return VoiceAction{};
}

void VoiceModule::clearAllState() {
    clearAnnouncedAlerts();
    clearAlertHistories();
    resetDirectionThrottle(0);
    resetPriorityStability();
    resetLastAnnounced();
    clearSpeedSample();
}

float VoiceModule::getCurrentSpeedMph(unsigned long now) {
    float speedMph = 0.0f;
    return getCurrentSpeedSample(now, speedMph) ? speedMph : 0.0f;
}

bool VoiceModule::getCurrentSpeedSample(unsigned long now, float& speedMphOut) const {
    if (cachedSpeedTimestamp == 0 || (now - cachedSpeedTimestamp) > SPEED_CACHE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = cachedSpeedMph;
    return true;
}

void VoiceModule::updateSpeedSample(float speedMph, unsigned long timestampMs) {
    cachedSpeedMph = speedMph;
    cachedSpeedTimestamp = timestampMs;
}

void VoiceModule::clearSpeedSample() {
    cachedSpeedMph = 0.0f;
    cachedSpeedTimestamp = 0;
}

bool VoiceModule::hasValidSpeedSource(unsigned long now) const {
    float ignored = 0.0f;
    return getCurrentSpeedSample(now, ignored);
}

void VoiceModule::clearAnnouncedAlerts() {
    announcedAlertCount = 0;
    for (uint32_t& id : announcedAlertIds) {
        id = 0;
    }
}

void VoiceModule::clearAlertHistories() {
    alertHistoryCount = 0;
    for (AlertHistory& history : alertHistories) {
        history = AlertHistory{};
    }
}

void VoiceModule::resetDirectionThrottle(unsigned long now) {
    directionChangeCount = 0;
    directionChangeWindowStart = now;
}

void VoiceModule::resetPriorityStability() {
    lastPriorityAnnouncementTime = 0;
    priorityStableSince = 0;
    lastPriorityAlertId = 0xFFFFFFFF;
}

void VoiceModule::resetLastAnnounced() {
    lastVoiceAlertBand = BAND_NONE;
    lastVoiceAlertDirection = DIR_NONE;
    lastVoiceAlertFrequency = 0xFFFF;
    lastVoiceAlertBogeyCount = 0;
    lastVoiceAlertTime = 0;
}

void audio_set_volume(uint8_t /*volumePercent*/) {}
void play_test_voice() {}
void play_vol0_beep() {}
void play_alert_voice(AlertBand /*band*/, AlertDirection /*direction*/) {}
void play_frequency_voice(AlertBand /*band*/,
                          uint16_t /*freqMHz*/,
                          AlertDirection /*direction*/,
                          VoiceAlertMode /*mode*/,
                          bool /*includeDirection*/,
                          uint8_t /*bogeyCount*/) {}
void play_direction_only(AlertDirection /*direction*/, uint8_t /*bogeyCount*/) {}
void play_bogey_breakdown(uint8_t /*total*/, uint8_t /*ahead*/, uint8_t /*behind*/, uint8_t /*side*/) {}
void play_threat_escalation(AlertBand /*band*/,
                            uint16_t /*freqMHz*/,
                            AlertDirection /*direction*/,
                            uint8_t /*total*/,
                            uint8_t /*ahead*/,
                            uint8_t /*behind*/,
                            uint8_t /*side*/) {}
void play_band_only(AlertBand /*band*/) {}
void audio_init_sd() {}
void audio_init_buffers() {}
void audio_process_amp_timeout() {}

#include "../../src/modules/display/display_pipeline_module.cpp"

static DisplayMode displayMode = DisplayMode::IDLE;
static V1Display display;
static PacketParser parser;
static V1BLEClient ble;
static AlertPersistenceModule alertPersistence;
static VoiceModule voice;
static DisplayPipelineModule module;

static AlertData makeKAlert(uint16_t freq = 24148) {
    AlertData a{};
    a.isValid = true;
    a.isPriority = true;
    a.band = BAND_K;
    a.frequency = freq;
    a.direction = DIR_FRONT;
    a.frontStrength = 5;
    return a;
}

static void beginModule() {
    module.begin(&displayMode,
                 &display,
                 &parser,
                 &settingsManager,
                 &ble,
                 &alertPersistence,
                 &voice);
}

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    display.reset();
    parser.reset();
    ble.reset();
    alertPersistence = AlertPersistenceModule{};
    voice = VoiceModule{};
    module = DisplayPipelineModule{};
    displayMode = DisplayMode::IDLE;
    settingsManager = SettingsManager{};
    perfCounters.reset();
    perfExtended.reset();
    beginModule();
}

void tearDown() {}

void test_handle_parsed_updates_live_display_when_alert_present() {
    parser.setAlerts({makeKAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.updatePersistedCalls);
}

void test_handle_parsed_updates_resting_display_when_idle() {
    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.updatePersistedCalls);
}

void test_handle_parsed_prefers_persisted_alert_when_configured() {
    settingsManager.slotAlertPersistSec[0] = 2;
    alertPersistence.setPersistedAlert(makeKAlert());

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(0, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.updatePersistedCalls);
}

void test_restore_current_owner_shows_scanning_when_ble_is_disconnected() {
    ble.setConnected(false);

    mockMillis = 2000;
    mockMicros = 2000 * 1000UL;
    module.restoreCurrentOwner(2000);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.showScanningCalls);
}

void test_restore_current_owner_restores_live_display_when_alerts_present() {
    parser.setAlerts({makeKAlert(24210)});

    mockMillis = 3000;
    mockMicros = 3000 * 1000UL;
    module.restoreCurrentOwner(3000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.forceNextRedrawCalls);
}

void test_alert_gap_recovery_throttle_is_instance_owned() {
    PacketParser parserA;
    PacketParser parserB;
    V1BLEClient bleA;
    V1BLEClient bleB;
    V1Display displayA;
    V1Display displayB;
    AlertPersistenceModule persistenceA;
    AlertPersistenceModule persistenceB;
    VoiceModule voiceA;
    VoiceModule voiceB;
    DisplayMode modeA = DisplayMode::IDLE;
    DisplayMode modeB = DisplayMode::IDLE;
    DisplayPipelineModule moduleA;
    DisplayPipelineModule moduleB;

    parserA.setActiveBands(BAND_K);
    parserB.setActiveBands(BAND_K);

    moduleA.begin(&modeA, &displayA, &parserA, &settingsManager, &bleA, &persistenceA, &voiceA);
    moduleB.begin(&modeB, &displayB, &parserB, &settingsManager, &bleB, &persistenceB, &voiceB);

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    moduleA.handleParsed(1000, false);
    moduleB.handleParsed(1000, false);

    TEST_ASSERT_EQUAL(1, bleA.requestAlertDataCalls);
    TEST_ASSERT_EQUAL(1, bleB.requestAlertDataCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_parsed_updates_live_display_when_alert_present);
    RUN_TEST(test_handle_parsed_updates_resting_display_when_idle);
    RUN_TEST(test_handle_parsed_prefers_persisted_alert_when_configured);
    RUN_TEST(test_restore_current_owner_shows_scanning_when_ble_is_disconnected);
    RUN_TEST(test_restore_current_owner_restores_live_display_when_alerts_present);
    RUN_TEST(test_alert_gap_recovery_throttle_is_instance_owned);
    return UNITY_END();
}
