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
static VoiceContext g_lastVoiceContext;
static int g_voiceProcessCalls = 0;
void perfRecordDisplayRenderUs(uint32_t /*us*/) {}
void perfRecordDisplayScenarioRenderUs(uint32_t /*us*/) {}
void perfRecordDisplayVoiceUs(uint32_t /*us*/) {}
void perfSetDisplayRenderScenario(PerfDisplayRenderScenario /*scenario*/) {}
PerfDisplayRenderScenario perfGetDisplayRenderScenario() { return PerfDisplayRenderScenario::None; }
void perfClearDisplayRenderScenario() {}

AlertPersistenceModule::AlertPersistenceModule() = default;

void AlertPersistenceModule::begin(V1BLEClient* ble,
                                   PacketParser* parserRef,
                                   V1Display* displayRef,
                                   SettingsManager* settingsRef) {
    bleClient_ = ble;
    parser_ = parserRef;
    display_ = displayRef;
    settings_ = settingsRef;
}

void AlertPersistenceModule::setPersistedAlert(const AlertData& alert) {
    persistedAlert_ = alert;
}

void AlertPersistenceModule::startPersistence(unsigned long now) {
    if (!alertPersistenceActive_) {
        alertClearedTime_ = now;
        alertPersistenceActive_ = true;
    }
}

void AlertPersistenceModule::clearPersistence() {
    alertPersistenceActive_ = false;
}

bool AlertPersistenceModule::shouldShowPersisted(unsigned long now, unsigned long persistMs) const {
    return alertPersistenceActive_ &&
           persistedAlert_.isValid &&
           (now - alertClearedTime_) < persistMs;
}

VoiceModule::VoiceModule() = default;

void VoiceModule::begin(SettingsManager* settingsRef, V1BLEClient* ble) {
    settings_ = settingsRef;
    bleClient_ = ble;
}

VoiceAction VoiceModule::process(const VoiceContext& ctx) {
    g_lastVoiceContext = ctx;
    g_voiceProcessCalls++;
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
    if (cachedSpeedTimestamp_ == 0 || (now - cachedSpeedTimestamp_) > SPEED_CACHE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = cachedSpeedMph_;
    return true;
}

void VoiceModule::updateSpeedSample(float speedMph, unsigned long timestampMs) {
    cachedSpeedMph_ = speedMph;
    cachedSpeedTimestamp_ = timestampMs;
}

void VoiceModule::clearSpeedSample() {
    cachedSpeedMph_ = 0.0f;
    cachedSpeedTimestamp_ = 0;
}

bool VoiceModule::hasValidSpeedSource(unsigned long now) const {
    float ignored = 0.0f;
    return getCurrentSpeedSample(now, ignored);
}

void VoiceModule::clearAnnouncedAlerts() {
    announcedAlertCount_ = 0;
    for (uint32_t& id : announcedAlertIds_) {
        id = 0;
    }
}

void VoiceModule::clearAlertHistories() {
    alertHistoryCount_ = 0;
    for (AlertHistory& history : alertHistories_) {
        history = AlertHistory{};
    }
}

void VoiceModule::resetDirectionThrottle(unsigned long now) {
    directionChangeCount_ = 0;
    directionChangeWindowStart_ = now;
}

void VoiceModule::resetPriorityStability() {
    lastPriorityAnnouncementTime_ = 0;
    priorityStableSince_ = 0;
    lastPriorityAlertId_ = 0xFFFFFFFF;
}

void VoiceModule::resetLastAnnounced() {
    lastVoiceAlertBand_ = BAND_NONE;
    lastVoiceAlertDirection_ = DIR_NONE;
    lastVoiceAlertFrequency_ = 0xFFFF;
    lastVoiceAlertBogeyCount_ = 0;
    lastVoiceAlertTime_ = 0;
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

// ALP module stubs — pipeline calls alpGunAbbrev() and uses AlpRuntimeModule pointer
#include "../../src/modules/alp/alp_runtime_module.h"
const char* alpStateName(AlpState) { return "OFF"; }
const char* alpGunName(AlpGunType) { return "Unknown"; }
const char* alpGunAbbrev(AlpGunType) { return "LASER"; }
AlpGunType alpLookupGun(uint8_t, uint8_t) { return AlpGunType::UNKNOWN; }
AlpGunType alpLookupGunObserve(uint8_t, uint8_t) { return AlpGunType::UNKNOWN; }

#include "../../src/modules/speed_mute/speed_mute_module.cpp"
#include "../../src/modules/quiet/quiet_coordinator_module.cpp"
#include "../../src/modules/display/display_pipeline_module.cpp"

static DisplayMode displayMode = DisplayMode::IDLE;
static V1Display display;
static PacketParser parser;
static V1BLEClient ble;
static AlertPersistenceModule alertPersistence;
static VoiceModule voice;
static QuietCoordinatorModule quiet;
static SpeedMuteModule speedMute;
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

static AlertData makeKaAlert(uint16_t freq = 34520) {
    AlertData a{};
    a.isValid = true;
    a.band = BAND_KA;
    a.frequency = freq;
    a.direction = DIR_REAR;
    a.rearStrength = 4;
    return a;
}

static void enableSpeedMute(uint8_t targetVolume) {
    speedMute.begin(true, 25, 3, targetVolume);
    speedMute.update(10.0f, true, 2000);
    module.setSpeedMuteModule(&speedMute);
}

static void beginModule() {
    module.begin(&displayMode,
                 &display,
                 &parser,
                 &settingsManager,
                 &ble,
                 &alertPersistence,
                 &voice,
                 &quiet);
}

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    display.reset();
    parser.reset();
    ble.reset();
    alertPersistence = AlertPersistenceModule{};
    voice = VoiceModule{};
    speedMute = SpeedMuteModule{};
    module = DisplayPipelineModule{};
    displayMode = DisplayMode::IDLE;
    settingsManager = SettingsManager{};
    perfCounters.reset();
    perfExtended.reset();
    g_lastVoiceContext = VoiceContext{};
    g_voiceProcessCalls = 0;
    quiet.begin(&ble, &parser);
    beginModule();
}

void tearDown() {}

void test_handle_parsed_updates_live_display_when_alert_present() {
    parser.setAlerts({makeKAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.updatePersistedCalls);
    TEST_ASSERT_EQUAL(1, display.lastAlertUpdateCount);
}

void test_handle_parsed_updates_resting_display_when_idle() {
    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.updatePersistedCalls);
}

void test_handle_parsed_prefers_persisted_alert_when_configured() {
    settingsManager.slotAlertPersistSec[0] = 2;
    alertPersistence.setPersistedAlert(makeKAlert());

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(0, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.updatePersistedCalls);
}

void test_handle_parsed_defers_secondary_cards_while_connect_burst_settles() {
    ble.setConnectBurstSettling(true);
    parser.setAlerts({makeKAlert(), makeKaAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.lastAlertUpdateCount);
}

void test_handle_parsed_uses_quiet_coordinator_to_suppress_speedmuted_k_voice() {
    enableSpeedMute(0);
    parser.state.muted = true;
    parser.state.mainVolume = 0;
    parser.setAlerts({makeKAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(1, g_voiceProcessCalls);
    TEST_ASSERT_TRUE(g_lastVoiceContext.isSuppressed);
    TEST_ASSERT_TRUE(quiet.getPresentationState().voiceSuppressed);
}

void test_handle_parsed_uses_quiet_coordinator_for_ka_vol_zero_bypass() {
    enableSpeedMute(0);
    parser.state.muted = true;
    parser.state.mainVolume = 0;
    parser.setAlerts({makeKaAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000);

    TEST_ASSERT_EQUAL(1, g_voiceProcessCalls);
    TEST_ASSERT_FALSE(g_lastVoiceContext.isSuppressed);
    TEST_ASSERT_FALSE(g_lastVoiceContext.isMuted);
    TEST_ASSERT_EQUAL_UINT8(1, g_lastVoiceContext.mainVolume);
    TEST_ASSERT_TRUE(quiet.getPresentationState().voiceAllowVolZeroBypass);
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_handle_parsed_updates_live_display_when_alert_present);
    RUN_TEST(test_handle_parsed_updates_resting_display_when_idle);
    RUN_TEST(test_handle_parsed_prefers_persisted_alert_when_configured);
    RUN_TEST(test_handle_parsed_defers_secondary_cards_while_connect_burst_settles);
    RUN_TEST(test_handle_parsed_uses_quiet_coordinator_to_suppress_speedmuted_k_voice);
    RUN_TEST(test_handle_parsed_uses_quiet_coordinator_for_ka_vol_zero_bypass);
    RUN_TEST(test_restore_current_owner_shows_scanning_when_ble_is_disconnected);
    RUN_TEST(test_restore_current_owner_restores_live_display_when_alerts_present);
    return UNITY_END();
}
