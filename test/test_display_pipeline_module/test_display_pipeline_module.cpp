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
#include "../../src/modules/camera_alert/camera_alert_module.h"
#include "../../src/modules/gps/gps_runtime_module.h"
#include "../../src/modules/voice/voice_module.h"
#include "../../src/modules/volume_fade/volume_fade_module.h"
#include "../../src/perf_metrics.h"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;
void perfRecordDisplayRenderUs(uint32_t /*us*/) {}
void perfRecordCameraDisplayUs(uint32_t /*us*/) {}
void perfRecordCameraDebugDisplayUs(uint32_t /*us*/) {}

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

VolumeFadeModule::VolumeFadeModule()
    : settings(nullptr),
      alertStartMs(0),
      originalVolume(0xFF),
      originalMuteVolume(0),
      fadeActive(false),
      commandSent(false),
      restoreLogEmitted(false),
      seenCount(0),
      pendingRestoreVolume(0xFF),
      pendingRestoreMuteVolume(0),
      pendingRestoreSetMs(0),
      lastRestoreAttemptMs(0) {}

void VolumeFadeModule::begin(SettingsManager* settingsRef) {
    settings = settingsRef;
}

VolumeFadeAction VolumeFadeModule::process(const VolumeFadeContext& /*ctx*/) {
    return VolumeFadeAction{};
}

void VolumeFadeModule::reset() {
    settings = nullptr;
    alertStartMs = 0;
    originalVolume = 0xFF;
    originalMuteVolume = 0;
    fadeActive = false;
    commandSent = false;
    restoreLogEmitted = false;
    seenCount = 0;
    pendingRestoreVolume = 0xFF;
    pendingRestoreMuteVolume = 0;
    pendingRestoreSetMs = 0;
    lastRestoreAttemptMs = 0;
    hintBaselineVolume = 0xFF;
    hintBaselineMuteVolume = 0;
    hintSetMs = 0;
}

void VolumeFadeModule::setBaselineHint(uint8_t mainVol, uint8_t muteVol, uint32_t nowMs) {
    hintBaselineVolume = mainVol;
    hintBaselineMuteVolume = muteVol;
    hintSetMs = nowMs;
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

void CameraAlertModule::begin(RoadMapReader* roadMap, SettingsManager* settings) {
    roadMap_ = roadMap;
    settings_ = settings;
}

static int cameraProcessCalls = 0;

void CameraAlertModule::process(uint32_t nowMs, const CameraAlertContext& /*ctx*/) {
    hasPolled_ = true;
    lastPollMs_ = nowMs;
    cameraProcessCalls++;
}

void CameraAlertModule::resetEncounter() {
    breadcrumbCount_ = 0;
    breadcrumbWriteIndex_ = 0;
    state_ = ApproachState::IDLE;
    encounterLatE5_ = 0;
    encounterLonE5_ = 0;
    encounterFlags_ = 0;
    lastDistanceCm_ = UINT32_MAX;
    lastSeenMs_ = 0;
    closingPollCount_ = 0;
    displayPayload_ = CameraAlertDisplayPayload{};
    lastPollMs_ = 0;
    hasPolled_ = false;
}

void GpsRuntimeModule::begin(bool enabled) {
    enabled_ = enabled;
}

void GpsRuntimeModule::setEnabled(bool enabled) {
    enabled_ = enabled;
}

void GpsRuntimeModule::setScaffoldSample(float speedMph,
                                         bool hasFix,
                                         uint8_t satellites,
                                         float hdop,
                                         uint32_t timestampMs,
                                         float latitudeDeg,
                                         float longitudeDeg,
                                         float courseDeg) {
    sampleValid_ = true;
    hasFix_ = hasFix;
    speedMph_ = speedMph;
    satellites_ = satellites;
    stableSatellites_ = satellites;
    hdop_ = hdop;
    sampleTsMs_ = timestampMs;
    lastFixTsMs_ = timestampMs;
    lastStableFixTsMs_ = timestampMs;
    locationValid_ = hasFix;
    latitudeDeg_ = latitudeDeg;
    longitudeDeg_ = longitudeDeg;
    courseValid_ = !isnan(courseDeg);
    courseDeg_ = courseDeg;
    courseSampleTsMs_ = courseValid_ ? timestampMs : 0;
    ++injectedSamples_;
}

bool GpsRuntimeModule::getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const {
    if (!sampleValid_ || (nowMs - sampleTsMs_) > SAMPLE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = speedMph_;
    tsMsOut = sampleTsMs_;
    return true;
}

GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t nowMs) const {
    GpsRuntimeStatus status;
    const bool stableHasFix = hasFix_ && lastStableFixTsMs_ != 0;
    status.enabled = enabled_;
    status.sampleValid = sampleValid_;
    status.hasFix = hasFix_;
    status.stableHasFix = stableHasFix;
    status.speedMph = speedMph_;
    status.satellites = satellites_;
    status.stableSatellites = stableSatellites_;
    status.hdop = hdop_;
    status.locationValid = locationValid_;
    status.latitudeDeg = latitudeDeg_;
    status.longitudeDeg = longitudeDeg_;
    status.courseValid = courseValid_;
    status.courseDeg = courseDeg_;
    status.courseSampleTsMs = courseSampleTsMs_;
    status.courseAgeMs = courseValid_ ? (nowMs - courseSampleTsMs_) : UINT32_MAX;
    status.sampleTsMs = sampleTsMs_;
    status.sampleAgeMs = sampleValid_ ? (nowMs - sampleTsMs_) : UINT32_MAX;
    status.fixAgeMs = hasFix_ ? (nowMs - lastFixTsMs_) : UINT32_MAX;
    status.stableFixAgeMs = stableHasFix ? (nowMs - lastStableFixTsMs_) : UINT32_MAX;
    status.injectedSamples = injectedSamples_;
    status.moduleDetected = moduleDetected_;
    status.detectionTimedOut = detectionTimedOut_;
    status.parserActive = parserActive_;
    status.hardwareSamples = hardwareSamples_;
    status.bytesRead = bytesRead_;
    status.sentencesSeen = sentencesSeen_;
    status.sentencesParsed = sentencesParsed_;
    status.parseFailures = parseFailures_;
    status.checksumFailures = checksumFailures_;
    status.bufferOverruns = bufferOverruns_;
    status.lastSentenceTsMs = lastSentenceTsMs_;
    return status;
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
static int cameraAlertVoiceCalls = 0;
static CameraType lastCameraAlertVoiceType = CameraType::INVALID;
static AlertDirection lastCameraAlertVoiceDirection = AlertDirection::AHEAD;
static CameraAlertVoiceResult nextCameraAlertVoiceResult = CameraAlertVoiceResult::STARTED;
CameraAlertVoiceResult play_camera_alert_voice(CameraType type, AlertDirection direction) {
    lastCameraAlertVoiceType = type;
    lastCameraAlertVoiceDirection = direction;
    if (nextCameraAlertVoiceResult == CameraAlertVoiceResult::STARTED) {
        cameraAlertVoiceCalls++;
    }
    return nextCameraAlertVoiceResult;
}
void audio_init_sd() {}
void audio_init_buffers() {}
void audio_process_amp_timeout() {}

#include "../../src/modules/display/display_pipeline_module.cpp"

static DisplayMode displayMode = DisplayMode::IDLE;
static V1Display display;
static PacketParser parser;
static V1BLEClient ble;
static AlertPersistenceModule alertPersistence;
static VolumeFadeModule volumeFade;
static VoiceModule voice;
static GpsRuntimeModule gpsRuntime;
static CameraAlertModule cameraModule;
static DisplayPipelineModule module;

static void beginModule() {
    module.begin(&displayMode,
                 &display,
                 &parser,
                 &settingsManager,
                 &ble,
                 &alertPersistence,
                 &volumeFade,
                 &voice,
                 &gpsRuntime,
                 &cameraModule);
}

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
    display.reset();
    parser.reset();
    ble.reset();
    alertPersistence = AlertPersistenceModule{};
    volumeFade = VolumeFadeModule{};
    voice = VoiceModule{};
    gpsRuntime = GpsRuntimeModule{};
    cameraModule = CameraAlertModule{};
    module = DisplayPipelineModule{};
    displayMode = DisplayMode::IDLE;
    settingsManager = SettingsManager{};
    perfCounters.reset();
    perfExtended.reset();
    cameraProcessCalls = 0;
    cameraAlertVoiceCalls = 0;
    lastCameraAlertVoiceType = CameraType::INVALID;
    lastCameraAlertVoiceDirection = AlertDirection::AHEAD;
    nextCameraAlertVoiceResult = CameraAlertVoiceResult::STARTED;
    beginModule();
}

void tearDown() {}

void test_debug_camera_override_throttles_duplicate_frames_until_state_changes() {
    CameraAlertDisplayPayload payload{};
    payload.active = true;
    payload.distanceCm = 16093;

    parser.state.bogeyCounterChar = '1';
    parser.state.bogeyCounterDot = false;

    TEST_ASSERT_TRUE(module.debugRenderCameraPayload(1000, payload, 5000));

    mockMillis = 1030;
    mockMicros = 1030 * 1000UL;
    module.handleParsed(1030, false);
    TEST_ASSERT_EQUAL(1, display.updateCameraAlertCalls);
    TEST_ASSERT_TRUE(module.isCameraAlertActive());

    mockMillis = 1100;
    mockMicros = 1100 * 1000UL;
    module.handleParsed(1100, false);
    TEST_ASSERT_EQUAL(1, display.updateCameraAlertCalls);

    parser.state.bogeyCounterChar = '2';
    mockMillis = 1130;
    mockMicros = 1130 * 1000UL;
    module.handleParsed(1130, false);
    TEST_ASSERT_EQUAL(2, display.updateCameraAlertCalls);
}

void test_restore_current_owner_returns_to_resting_view_after_debug_override_clears() {
    CameraAlertDisplayPayload payload{};
    payload.active = true;
    payload.distanceCm = 32000;

    TEST_ASSERT_TRUE(module.debugRenderCameraPayload(1000, payload, 5000));

    mockMillis = 1030;
    mockMicros = 1030 * 1000UL;
    module.handleParsed(1030, false);
    TEST_ASSERT_EQUAL(1, display.updateCameraAlertCalls);

    module.clearDebugCameraOverride();

    mockMillis = 1500;
    mockMicros = 1500 * 1000UL;
    module.restoreCurrentOwner(1500);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(1, display.updateCameraAlertCalls);
    TEST_ASSERT_FALSE(module.isCameraAlertActive());
}

void test_restore_current_owner_shows_scanning_when_ble_is_disconnected() {
    ble.setConnected(false);

    mockMillis = 2000;
    mockMicros = 2000 * 1000UL;
    module.restoreCurrentOwner(2000);

    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, display.showScanningCalls);
    TEST_ASSERT_FALSE(module.isCameraAlertActive());
}

// --- Camera tracking under live radar alerts ---

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

void test_camera_process_called_while_radar_active() {
    // Set up a live radar alert
    parser.setAlerts({makeKAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    cameraProcessCalls = 0;
    module.handleParsed(1000, false);

    // Camera process should still be called even with radar active
    TEST_ASSERT_GREATER_THAN(0, cameraProcessCalls);
    // But camera should NOT own the display
    TEST_ASSERT_FALSE(module.isCameraAlertActive());
    TEST_ASSERT_EQUAL(0, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(0, cameraAlertVoiceCalls);
}

void test_camera_no_display_while_radar_active() {
    // Make camera module think it has an active display payload
    cameraModule.setDisplayPayloadForTest(CameraAlertDisplayPayload{true, 5000});

    // Set up live radar
    parser.setAlerts({makeKAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);

    // Camera should not render even though it has an active payload
    TEST_ASSERT_FALSE(module.isCameraAlertActive());
    TEST_ASSERT_EQUAL(0, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(0, cameraAlertVoiceCalls);
}

void test_camera_resumes_after_radar_clears() {
    // Make camera module think it has an active display payload
    cameraModule.setDisplayPayloadForTest(CameraAlertDisplayPayload{true, 5000});

    // First: radar active — camera should not render
    parser.setAlerts({makeKAlert()});
    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);
    TEST_ASSERT_FALSE(module.isCameraAlertActive());
    TEST_ASSERT_EQUAL(0, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(0, cameraAlertVoiceCalls);

    // Now: radar clears — camera should render
    parser.setAlerts({});
    mockMillis = 1050;
    mockMicros = 1050 * 1000UL;
    module.handleParsed(1050, false);
    TEST_ASSERT_TRUE(module.isCameraAlertActive());
    TEST_ASSERT_EQUAL(1, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(DisplayMode::IDLE, displayMode);
    TEST_ASSERT_EQUAL(1, cameraAlertVoiceCalls);
    TEST_ASSERT_EQUAL(static_cast<int>(CameraType::ALPR), static_cast<int>(lastCameraAlertVoiceType));
    TEST_ASSERT_EQUAL(static_cast<int>(AlertDirection::AHEAD), static_cast<int>(lastCameraAlertVoiceDirection));
}

void test_radar_display_unchanged_with_camera_fix() {
    // Radar-only scenario should behave exactly as before
    parser.setAlerts({makeKAlert()});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);

    TEST_ASSERT_EQUAL(DisplayMode::LIVE, displayMode);
    TEST_ASSERT_EQUAL(1, display.updateCalls);
    TEST_ASSERT_EQUAL(0, display.updateCameraAlertCalls);
    TEST_ASSERT_FALSE(module.isCameraAlertActive());
    TEST_ASSERT_EQUAL(0, cameraAlertVoiceCalls);
}

void test_camera_voice_announced_once_per_active_payload() {
    cameraModule.setDisplayPayloadForTest(CameraAlertDisplayPayload{true, 5000});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);
    TEST_ASSERT_EQUAL(1, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(1, cameraAlertVoiceCalls);

    mockMillis = 1030;
    mockMicros = 1030 * 1000UL;
    module.handleParsed(1030, false);
    TEST_ASSERT_EQUAL(2, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(1, cameraAlertVoiceCalls);
}

void test_camera_voice_retries_after_busy_audio_path() {
    cameraModule.setDisplayPayloadForTest(CameraAlertDisplayPayload{true, 5000});
    nextCameraAlertVoiceResult = CameraAlertVoiceResult::BUSY;

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);
    TEST_ASSERT_EQUAL(1, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(0, cameraAlertVoiceCalls);

    nextCameraAlertVoiceResult = CameraAlertVoiceResult::STARTED;
    mockMillis = 1030;
    mockMicros = 1030 * 1000UL;
    module.handleParsed(1030, false);
    TEST_ASSERT_EQUAL(2, display.updateCameraAlertCalls);
    TEST_ASSERT_EQUAL(1, cameraAlertVoiceCalls);
}

void test_camera_voice_resets_after_payload_clears() {
    cameraModule.setDisplayPayloadForTest(CameraAlertDisplayPayload{true, 5000});

    mockMillis = 1000;
    mockMicros = 1000 * 1000UL;
    module.handleParsed(1000, false);
    TEST_ASSERT_EQUAL(1, cameraAlertVoiceCalls);

    cameraModule.setDisplayPayloadForTest(CameraAlertDisplayPayload{false, 0});
    mockMillis = 1030;
    mockMicros = 1030 * 1000UL;
    module.handleParsed(1030, false);
    TEST_ASSERT_EQUAL(1, cameraAlertVoiceCalls);

    cameraModule.setDisplayPayloadForTest(CameraAlertDisplayPayload{true, 4500});
    mockMillis = 1060;
    mockMicros = 1060 * 1000UL;
    module.handleParsed(1060, false);
    TEST_ASSERT_EQUAL(2, cameraAlertVoiceCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_debug_camera_override_throttles_duplicate_frames_until_state_changes);
    RUN_TEST(test_restore_current_owner_returns_to_resting_view_after_debug_override_clears);
    RUN_TEST(test_restore_current_owner_shows_scanning_when_ble_is_disconnected);
    RUN_TEST(test_camera_process_called_while_radar_active);
    RUN_TEST(test_camera_no_display_while_radar_active);
    RUN_TEST(test_camera_resumes_after_radar_clears);
    RUN_TEST(test_radar_display_unchanged_with_camera_fix);
    RUN_TEST(test_camera_voice_announced_once_per_active_payload);
    RUN_TEST(test_camera_voice_retries_after_busy_audio_path);
    RUN_TEST(test_camera_voice_resets_after_payload_clears);
    return UNITY_END();
}
