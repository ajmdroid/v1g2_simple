/**
 * V1 Gen2 Simple Display - Main Application
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 with Valentine1 Gen2 BLE
 * 
 * Features:
 * - BLE client for V1 Gen2 radar detector
 * - BLE server proxy for JBV1 app compatibility
 * - 3.49" AMOLED display with touch support
 * - WiFi web interface for configuration
 * - 3-slot auto-push profile system
 * - Tap-to-mute functionality
 * - Multiple color themes
 * 
 * Architecture:
 * - FreeRTOS queue for BLE data handling
 * - Non-blocking display updates
 * - Persistent settings via Preferences
 * 
 * Author: Based on Valentine Research ESP protocol
 * License: MIT
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include "main_internals.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "display_mode.h"
#include "wifi_manager.h"
#include "settings.h"
#include "touch_handler.h"
#include "v1_profiles.h"
#include "v1_devices.h"
#include "obd_handler.h"
#include "battery_manager.h"
#include "storage_manager.h"
#include "debug_logger.h"
#include "audio_beep.h"
#include "perf_metrics.h"
#include "perf_sd_logger.h"
#include "../include/config.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/touch/touch_ui_module.h"
#include "modules/touch/tap_gesture_module.h"
#include "modules/wifi/wifi_orchestrator_module.h"
#include "modules/power/power_module.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/ble/connection_state_module.h"
#include "modules/ble/connection_runtime_module.h"
#include "modules/ble/connection_state_cadence_module.h"
#include "modules/ble/connection_state_dispatch_module.h"
#include "modules/display/display_pipeline_module.h"
#include "modules/display/display_orchestration_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/system/parsed_frame_event_module.h"
#include "modules/system/periodic_maintenance_module.h"
#include "modules/system/loop_tail_module.h"
#include "modules/system/loop_telemetry_module.h"
#include "modules/system/loop_ingest_module.h"
#include "modules/system/loop_display_module.h"
#include "modules/system/loop_power_touch_module.h"
#include "modules/system/loop_pre_ingest_module.h"
#include "modules/system/loop_runtime_snapshot_module.h"
#include "modules/system/loop_settings_prep_module.h"
#include "modules/system/loop_connection_early_module.h"
#include "modules/system/loop_post_display_module.h"
#include "esp_heap_caps.h"
#include "modules/voice/voice_module.h"
#include "modules/voice/voice_speed_sync_module.h"
#include "modules/speed_volume/speed_volume_module.h"
#include "modules/speed_volume/speaker_quiet_sync_module.h"
#include "modules/speed_volume/speed_volume_runtime_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/camera/camera_runtime_module.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/lockout/signal_capture_module.h"
#include "modules/lockout/signal_observation_sd_logger.h"
#include "modules/lockout/lockout_enforcer.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/lockout_band_policy.h"
#include "modules/lockout/lockout_runtime_mute_controller.h"
#include "modules/lockout/lockout_pre_quiet_controller.h"
#include "modules/lockout/lockout_orchestration_module.h"
#include "modules/debug/debug_api_service.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/wifi/wifi_boot_policy.h"
#include "modules/wifi/wifi_auto_start_module.h"
#include "modules/wifi/wifi_priority_policy_module.h"
#include "modules/wifi/wifi_visual_sync_module.h"
#include "modules/wifi/wifi_process_cadence_module.h"
#include "modules/wifi/wifi_runtime_module.h"
#include "modules/perf/debug_macros.h"
#include "time_service.h"
#include <driver/gpio.h>
#include "../include/display_driver.h"
#include <FS.h>
#include <algorithm>

// Global objects
V1BLEClient bleClient;
PacketParser parser;
V1Display display;
TouchHandler touchHandler;

// Alert persistence module
AlertPersistenceModule alertPersistenceModule;

// Voice Module - handles voice announcement decisions
VoiceModule voiceModule;
VoiceSpeedSyncModule voiceSpeedSyncModule;

static bool bootReady = false;
static unsigned long bootReadyDeadlineMs = 0;
static bool bootSplashHoldActive = false;
static unsigned long bootSplashHoldUntilMs = 0;
static bool initialScanningScreenShown = false;
static constexpr unsigned long BOOT_SPLASH_HOLD_MS = 400;
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_MS = 400;
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_WAKE_MS = 120;
static constexpr unsigned long CONNECTION_STATE_PROCESS_MAX_GAP_MS = 1000;
static unsigned long activeScanScreenDwellMs = MIN_SCAN_SCREEN_DWELL_MS;
static bool obdAutoConnectPending = false;
static unsigned long obdAutoConnectAtMs = 0;
static unsigned long v1ConnectedAtMs = 0;
static bool wifiAutoStartDone = false;

// Display preview driver (color + camera demos)
DisplayPreviewModule displayPreviewModule;
static ConnectionStateCadenceModule connectionStateCadenceModule;

// Lockout orchestration (enforcement + mute + pre-quiet pipeline)
LockoutOrchestrationModule lockoutOrchestrationModule;

void requestColorPreviewHold(uint32_t durationMs) {
    displayPreviewModule.requestHold(durationMs);
}

void requestCameraPreviewCycleHold(uint32_t durationMs) {
    displayPreviewModule.requestCameraCycle(durationMs);
}

void requestCameraPreviewSingleHold(uint8_t cameraType, uint32_t durationMs, bool muted) {
    displayPreviewModule.requestCameraSingle(cameraType, durationMs, muted);
}

bool isDisplayPreviewRunning() {
    return displayPreviewModule.isRunning();
}

bool isColorPreviewRunning() {
    return isDisplayPreviewRunning();
}

void cancelDisplayPreview() {
    displayPreviewModule.cancel();
}

void cancelColorPreview() {
    cancelDisplayPreview();
}

// resetReasonToString() — moved to main_boot.cpp

// normalizeLegacyLockoutRadiusScale() — moved to main_boot.cpp

static void showInitialScanningScreen() {
    if (initialScanningScreenShown) {
        return;
    }
    display.showScanning();
    display.drawProfileIndicator(settingsManager.get().activeSlot);
    initialScanningScreenShown = true;
    connectionStateCadenceModule.onScanningScreenShown(millis());
}

// logPanicBreadcrumbs() — moved to main_boot.cpp

// nvsHealthCheck() — moved to main_boot.cpp

// nextBootId() — moved to main_boot.cpp

static DisplayMode displayMode = DisplayMode::IDLE;

// Voice alert tracking handled by VoiceModule

// Volume fade module - reduce V1 volume after X seconds of continuous alert
VolumeFadeModule volumeFadeModule;

// Speed volume module - boost volume at highway speeds
SpeedVolumeModule speedVolumeModule;
SpeakerQuietSyncModule speakerQuietSyncModule;
SpeedVolumeRuntimeModule speedVolumeRuntimeModule;

// Auto-push profile state machine
AutoPushModule autoPushModule;
TouchUiModule touchUiModule;
TapGestureModule tapGestureModule;
PowerModule powerModule;
BleQueueModule bleQueueModule;
ConnectionStateModule connectionStateModule;
ConnectionRuntimeModule connectionRuntimeModule;
ConnectionStateDispatchModule connectionStateDispatchModule;
DisplayPipelineModule displayPipelineModule;
DisplayOrchestrationModule displayOrchestrationModule;
DisplayRestoreModule displayRestoreModule;
SystemEventBus systemEventBus;
PeriodicMaintenanceModule periodicMaintenanceModule;
LoopTailModule loopTailModule;
LoopTelemetryModule loopTelemetryModule;
LoopIngestModule loopIngestModule;
LoopDisplayModule loopDisplayModule;
LoopPowerTouchModule loopPowerTouchModule;
LoopPreIngestModule loopPreIngestModule;
LoopRuntimeSnapshotModule loopRuntimeSnapshotModule;
LoopSettingsPrepModule loopSettingsPrepModule;
LoopConnectionEarlyModule loopConnectionEarlyModule;
LoopPostDisplayModule loopPostDisplayModule;
WifiAutoStartModule wifiAutoStartModule;
WifiPriorityPolicyModule wifiPriorityPolicyModule;
WifiVisualSyncModule wifiVisualSyncModule;
WifiProcessCadenceModule wifiProcessCadenceModule;
WifiRuntimeModule wifiRuntimeModule;
ObdRuntimeModule obdRuntimeModule;

// Callback for BLE data reception - just queues data, doesn't process
// This runs in BLE task context, so we avoid SPI operations here
void onV1Data(const uint8_t* data, size_t length, uint16_t charUUID) {
    bleQueueModule.onNotify(data, length, charUUID);
}

// WiFi orchestration helper encapsulates WiFi start + callback wiring
static WifiOrchestrator& getWifiOrchestrator() {
    static WifiOrchestrator orchestrator(
        wifiManager,
        bleClient,
        parser,
        settingsManager,
        storageManager,
        autoPushModule,
        [](int slotIndex) { autoPushModule.start(slotIndex); });
    return orchestrator;
}

// Callback when V1 connection is fully established
// Handles auto-push of default profile and mode
void onV1Connected() {
    v1ConnectedAtMs = millis();

    // Start a new perf CSV session so scoring tools can isolate
    // V1-connected data from idle boot noise.
    perfSdLogger.startNewSession();

    const V1Settings& s = settingsManager.get();
    int activeSlotIndex = std::max(0, std::min(2, s.activeSlot));

    String connectedAddress;
    NimBLEAddress connected = bleClient.getConnectedAddress();
    if (!connected.isNull()) {
        connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
    }
    if (connectedAddress.length() == 0) {
        connectedAddress = normalizeV1DeviceAddress(s.lastV1Address);
    }

    if (connectedAddress.length() > 0 && v1DeviceStore.isReady()) {
        v1DeviceStore.upsertDevice(connectedAddress);
    }

    uint8_t deviceDefaultProfile = 0;
    int selectedSlotIndex = activeSlotIndex;
    if (connectedAddress.length() > 0 && v1DeviceStore.isReady()) {
        deviceDefaultProfile = v1DeviceStore.getDeviceDefaultProfile(connectedAddress);
        if (deviceDefaultProfile >= 1 && deviceDefaultProfile <= 3) {
            selectedSlotIndex = static_cast<int>(deviceDefaultProfile) - 1;
        }
    }

    const AutoPushSlot& slot = settingsManager.getSlot(selectedSlotIndex);
    SerialLog.printf("[AutoPush] onV1Connected autoPush=%s activeSlot=%d selectedSlot=%d defaultProfile=%u addr='%s' profile='%s' mode=%d\n",
                     s.autoPushEnabled ? "on" : "off",
                     activeSlotIndex,
                     selectedSlotIndex,
                     static_cast<unsigned>(deviceDefaultProfile),
                     connectedAddress.c_str(),
                     slot.profileName.c_str(),
                     static_cast<int>(slot.mode));
    if (activeSlotIndex != s.activeSlot) {
        AUTO_PUSH_LOGF("[AutoPush] WARNING: activeSlot out of range (%d). Using slot %d instead.\n",
                        s.activeSlot, activeSlotIndex);
    }

    // Attempt OBD auto-connect shortly after V1 stabilizes.
    // This runs regardless of autoPush when OBD service is enabled.
    if (s.obdEnabled) {
        obdAutoConnectPending = true;
        obdAutoConnectAtMs = millis() + 1500;
    } else {
        obdAutoConnectPending = false;
    }
    
    if (!s.autoPushEnabled) {
        AUTO_PUSH_LOGLN("[AutoPush] Disabled, skipping");
        return;
    }

    if (deviceDefaultProfile >= 1 && deviceDefaultProfile <= 3) {
        AUTO_PUSH_LOGF("[AutoPush] Using per-device default profile %u -> slot %d\n",
                       static_cast<unsigned>(deviceDefaultProfile),
                       selectedSlotIndex);
    } else {
        AUTO_PUSH_LOGF("[AutoPush] Using global activeSlot: %d\n", selectedSlotIndex);
    }

    autoPushModule.start(selectedSlotIndex);
}

// fatalBootError() — moved to main_boot.cpp

static void configureLoopSettingsPrepModule() {
    LoopSettingsPrepModule::Providers loopSettingsPrepProviders;
    loopSettingsPrepProviders.runTapGesture = [](void* ctx, uint32_t nowMs) {
        static_cast<TapGestureModule*>(ctx)->process(nowMs);
    };
    loopSettingsPrepProviders.tapGestureContext = &tapGestureModule;
    loopSettingsPrepProviders.readSettingsValues = [](void* ctx) -> LoopSettingsPrepValues {
        const V1Settings& settings = static_cast<SettingsManager*>(ctx)->get();
        LoopSettingsPrepValues values;
        values.obdServiceEnabled = settings.obdEnabled;
        values.enableWifiAtBoot = settings.enableWifiAtBoot;
        values.enableSignalTraceLogging = settings.enableSignalTraceLogging;
        values.configuredVoiceVolume = settings.voiceVolume;
        return values;
    };
    loopSettingsPrepProviders.settingsContext = &settingsManager;
    loopSettingsPrepModule.begin(loopSettingsPrepProviders);
}

static void configureLoopRuntimeSnapshotModule() {
    LoopRuntimeSnapshotModule::Providers loopRuntimeSnapshotProviders;
    loopRuntimeSnapshotProviders.readBleConnected = [](void* ctx) -> bool {
        return static_cast<V1BLEClient*>(ctx)->isConnected();
    };
    loopRuntimeSnapshotProviders.bleConnectedContext = &bleClient;
    loopRuntimeSnapshotProviders.readCanStartDma = [](void* ctx) -> bool {
        return static_cast<WiFiManager*>(ctx)->canStartSetupMode(nullptr, nullptr);
    };
    loopRuntimeSnapshotProviders.canStartDmaContext = &wifiManager;
    loopRuntimeSnapshotProviders.readDisplayPreviewRunning = [](void* ctx) -> bool {
        return static_cast<DisplayPreviewModule*>(ctx)->isRunning();
    };
    loopRuntimeSnapshotProviders.displayPreviewContext = &displayPreviewModule;
    loopRuntimeSnapshotModule.begin(loopRuntimeSnapshotProviders);
}

static void configureLoopPostDisplayModule() {
    LoopPostDisplayModule::Providers loopPostDisplayProviders;
    loopPostDisplayProviders.runAutoPush = [](void* ctx) {
        static_cast<AutoPushModule*>(ctx)->process();
    };
    loopPostDisplayProviders.autoPushContext = &autoPushModule;
    loopPostDisplayProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopPostDisplayProviders.runCameraRuntime =
        [](void*,
           uint32_t nowMs,
           bool skipLateNonCoreThisLoop,
           bool overloadLateThisLoop,
           bool loopSignalPriorityActive) {
            cameraRuntimeModule.process(
                nowMs,
                skipLateNonCoreThisLoop,
                overloadLateThisLoop,
                loopSignalPriorityActive);
        };
    loopPostDisplayProviders.recordCameraUs = [](void*, uint32_t elapsedUs) {
        perfRecordCameraUs(elapsedUs);
    };
    loopPostDisplayProviders.runSpeedVolumeRuntime =
        [](void* ctx, const SpeedVolumeRuntimeContext& speedVolumeCtx) {
            static_cast<SpeedVolumeRuntimeModule*>(ctx)->process(speedVolumeCtx);
        };
    loopPostDisplayProviders.speedVolumeRuntimeContext = &speedVolumeRuntimeModule;
    loopPostDisplayProviders.readDispatchNowMs = [](void*) -> uint32_t {
        return millis();
    };
    loopPostDisplayProviders.readBleConnectedNow = [](void* ctx) -> bool {
        return static_cast<V1BLEClient*>(ctx)->isConnected();
    };
    loopPostDisplayProviders.bleConnectedContext = &bleClient;
    loopPostDisplayProviders.runConnectionStateDispatch =
        [](void* ctx, const ConnectionStateDispatchContext& dispatchCtx) {
            static_cast<ConnectionStateDispatchModule*>(ctx)->process(dispatchCtx);
        };
    loopPostDisplayProviders.connectionDispatchContext = &connectionStateDispatchModule;
    loopPostDisplayModule.begin(loopPostDisplayProviders);
}

static void configureVoiceSpeedSyncModule() {
    VoiceSpeedSyncModule::Providers voiceSpeedSyncProviders;
    voiceSpeedSyncProviders.selectSpeedSample = [](void* ctx, uint32_t nowMs, SpeedSelection& selection) {
        return static_cast<SpeedSourceSelector*>(ctx)->select(nowMs, selection);
    };
    voiceSpeedSyncProviders.speedSelectorContext = &speedSourceSelector;
    voiceSpeedSyncProviders.updateVoiceSpeedSample = [](void* ctx, float speedMph, uint32_t timestampMs) {
        static_cast<VoiceModule*>(ctx)->updateSpeedSample(speedMph, timestampMs);
    };
    voiceSpeedSyncProviders.clearVoiceSpeedSample = [](void* ctx) {
        static_cast<VoiceModule*>(ctx)->clearSpeedSample();
    };
    voiceSpeedSyncProviders.voiceContext = &voiceModule;
    voiceSpeedSyncModule.begin(voiceSpeedSyncProviders);
}

static void configureSpeedVolumeRuntimeModule() {
    SpeedVolumeRuntimeModule::Providers speedVolumeRuntimeProviders;
    speedVolumeRuntimeProviders.runSpeedVolumeProcess = [](void* ctx, uint32_t nowMs) {
        static_cast<SpeedVolumeModule*>(ctx)->process(nowMs);
    };
    speedVolumeRuntimeProviders.speedVolumeContext = &speedVolumeModule;
    speedVolumeRuntimeProviders.readSpeedQuietActive = [](void* ctx) -> bool {
        return static_cast<SpeedVolumeModule*>(ctx)->isQuietActive();
    };
    speedVolumeRuntimeProviders.speedQuietActiveContext = &speedVolumeModule;
    speedVolumeRuntimeProviders.readSpeedQuietVolume = [](void* ctx) -> uint8_t {
        return static_cast<SpeedVolumeModule*>(ctx)->getQuietVolume();
    };
    speedVolumeRuntimeProviders.speedQuietVolumeContext = &speedVolumeModule;
    speedVolumeRuntimeProviders.runSpeakerQuietSync =
        [](void* ctx, bool quietNow, uint8_t quietVolume, uint8_t configuredVoiceVolume) {
            static_cast<SpeakerQuietSyncModule*>(ctx)->process(
                quietNow,
                quietVolume,
                configuredVoiceVolume,
                [](uint8_t volume) { audio_set_volume(volume); });
        };
    speedVolumeRuntimeProviders.speakerQuietContext = &speakerQuietSyncModule;
    speedVolumeRuntimeModule.begin(speedVolumeRuntimeProviders);
}

static void configureWifiRuntimeModule() {
    WifiRuntimeModule::Providers wifiRuntimeProviders;
    wifiRuntimeProviders.runWifiAutoStartProcess =
        [](void* ctx,
           uint32_t nowMs,
           uint32_t v1ConnectedAtMs,
           bool enableWifiAtBoot,
           bool bleConnected,
           bool canStartDma,
           bool& wifiAutoStartDone) {
            static_cast<WifiAutoStartModule*>(ctx)->process(
                nowMs,
                v1ConnectedAtMs,
                enableWifiAtBoot,
                bleConnected,
                canStartDma,
                wifiAutoStartDone,
                [] { getWifiOrchestrator().startWifi(); },
                [] { wifiManager.markAutoStarted(); });
        };
    wifiRuntimeProviders.wifiAutoStartContext = &wifiAutoStartModule;
    wifiRuntimeProviders.shouldRunWifiProcessingPolicy =
        [](void* ctx, bool enableWifiAtBoot, bool wifiAutoStartDone) {
            return isWifiProcessingEnabledPolicy(
                *static_cast<WiFiManager*>(ctx), enableWifiAtBoot, wifiAutoStartDone);
        };
    wifiRuntimeProviders.wifiPolicyContext = &wifiManager;
    wifiRuntimeProviders.perfTimestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    wifiRuntimeProviders.runWifiCadence = [](void* ctx, const WifiProcessCadenceContext& cadenceCtx) {
        return static_cast<WifiProcessCadenceModule*>(ctx)->process(cadenceCtx);
    };
    wifiRuntimeProviders.wifiCadenceContext = &wifiProcessCadenceModule;
    wifiRuntimeProviders.recordWifiProcessUs = [](void*, uint32_t elapsedUs) {
        perfRecordWifiProcessUs(elapsedUs);
    };
    wifiRuntimeProviders.readWifiServiceActive = [](void* ctx) -> bool {
        return static_cast<WiFiManager*>(ctx)->isWifiServiceActive();
    };
    wifiRuntimeProviders.wifiServiceContext = &wifiManager;
    wifiRuntimeProviders.readWifiConnected = [](void* ctx) -> bool {
        return static_cast<WiFiManager*>(ctx)->isConnected();
    };
    wifiRuntimeProviders.wifiConnectedContext = &wifiManager;
    wifiRuntimeProviders.readVisualNowMs = [](void*) -> uint32_t {
        return millis();
    };
    wifiRuntimeProviders.runWifiVisualSync =
        [](void* ctx,
           uint32_t nowMs,
           bool wifiVisualActiveNow,
           bool displayPreviewRunning,
           bool bootSplashHoldActive) {
            static_cast<WifiVisualSyncModule*>(ctx)->process(
                nowMs,
                wifiVisualActiveNow,
                displayPreviewRunning,
                bootSplashHoldActive,
                [] {
                    display.drawWiFiIndicator();
                    const int leftColWidth = 64;
                    const int leftColHeight = 96;
                    display.flushRegion(0, SCREEN_HEIGHT - leftColHeight, leftColWidth, leftColHeight);
                });
        };
    wifiRuntimeProviders.wifiVisualSyncContext = &wifiVisualSyncModule;
    wifiRuntimeModule.begin(wifiRuntimeProviders);
}

static void configureLoopConnectionEarlyModule() {
    LoopConnectionEarlyModule::Providers loopConnectionEarlyProviders;
    loopConnectionEarlyProviders.runConnectionRuntime =
        [](void* ctx,
           uint32_t nowMs,
           uint32_t nowUs,
           uint32_t lastLoopUs,
           bool bootSplashHoldActive,
           uint32_t bootSplashHoldUntilMs,
           bool initialScanningScreenShown) {
            return static_cast<ConnectionRuntimeModule*>(ctx)->process(
                nowMs,
                nowUs,
                lastLoopUs,
                bootSplashHoldActive,
                bootSplashHoldUntilMs,
                initialScanningScreenShown);
        };
    loopConnectionEarlyProviders.connectionRuntimeContext = &connectionRuntimeModule;
    loopConnectionEarlyProviders.showInitialScanning = [](void*) {
        showInitialScanningScreen();
    };
    loopConnectionEarlyProviders.readProxyConnected = [](void* ctx) -> bool {
        return static_cast<V1BLEClient*>(ctx)->isProxyClientConnected();
    };
    loopConnectionEarlyProviders.proxyConnectedContext = &bleClient;
    loopConnectionEarlyProviders.readConnectionRssi = [](void* ctx) -> int {
        return static_cast<V1BLEClient*>(ctx)->getConnectionRssi();
    };
    loopConnectionEarlyProviders.connectionRssiContext = &bleClient;
    loopConnectionEarlyProviders.readProxyRssi = [](void* ctx) -> int {
        return static_cast<V1BLEClient*>(ctx)->getProxyClientRssi();
    };
    loopConnectionEarlyProviders.proxyRssiContext = &bleClient;
    loopConnectionEarlyProviders.runDisplayEarly =
        [](void* ctx, const DisplayOrchestrationEarlyContext& displayEarlyCtx) {
            static_cast<DisplayOrchestrationModule*>(ctx)->processEarly(displayEarlyCtx);
        };
    loopConnectionEarlyProviders.displayEarlyContext = &displayOrchestrationModule;
    loopConnectionEarlyModule.begin(loopConnectionEarlyProviders);
}

static void configureLoopPowerTouchModule() {
    LoopPowerTouchModule::Providers loopPowerTouchProviders;
    loopPowerTouchProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopPowerTouchProviders.microsNow = [](void*) -> uint32_t {
        return micros();
    };
    loopPowerTouchProviders.runPowerProcess = [](void* ctx, uint32_t nowMs) {
        static_cast<PowerModule*>(ctx)->process(nowMs);
    };
    loopPowerTouchProviders.powerContext = &powerModule;
    loopPowerTouchProviders.runTouchUiProcess =
        [](void* ctx, uint32_t nowMs, bool bootButtonPressed) -> bool {
            return static_cast<TouchUiModule*>(ctx)->process(nowMs, bootButtonPressed);
        };
    loopPowerTouchProviders.touchUiContext = &touchUiModule;
    loopPowerTouchProviders.recordTouchUs = [](void*, uint32_t elapsedUs) {
        perfRecordTouchUs(elapsedUs);
    };
    loopPowerTouchProviders.recordLoopJitterUs = [](void*, uint32_t jitterUs) {
        perfRecordLoopJitterUs(jitterUs);
    };
    loopPowerTouchProviders.refreshDmaCache = [](void*) {
        StorageManager::updateDmaHeapCache();
    };
    loopPowerTouchProviders.readFreeHeap = [](void*) -> uint32_t {
        return ESP.getFreeHeap();
    };
    loopPowerTouchProviders.readLargestHeapBlock = [](void*) -> uint32_t {
        return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    };
    loopPowerTouchProviders.readCachedFreeDma = [](void*) -> uint32_t {
        return StorageManager::getCachedFreeDma();
    };
    loopPowerTouchProviders.readCachedLargestDma = [](void*) -> uint32_t {
        return StorageManager::getCachedLargestDma();
    };
    loopPowerTouchProviders.recordHeapStats =
        [](void*, uint32_t freeHeap, uint32_t largestHeapBlock, uint32_t cachedFreeDma, uint32_t cachedLargestDma) {
            perfRecordHeapStats(freeHeap, largestHeapBlock, cachedFreeDma, cachedLargestDma);
        };
    loopPowerTouchModule.begin(loopPowerTouchProviders);
}

static void configureLoopPreIngestModule() {
    LoopPreIngestModule::Providers loopPreIngestProviders;
    loopPreIngestProviders.openBootReadyGate = [](void*, uint32_t nowMs) {
        bleClient.setBootReady(true);
        SerialLog.printf("[Boot] Ready gate opened at %lu ms (timeout)\n", static_cast<unsigned long>(nowMs));
    };
    loopPreIngestProviders.runWifiPriorityApply =
        [](void* ctx, uint32_t nowMs, bool obdServiceEnabled) {
            static_cast<WifiPriorityPolicyModule*>(ctx)->apply(
                nowMs,
                obdServiceEnabled,
                bleClient,
                wifiManager,
                obdHandler);
        };
    loopPreIngestProviders.wifiPriorityContext = &wifiPriorityPolicyModule;
    loopPreIngestProviders.runDebugApiProcess = [](void*, uint32_t nowMs) {
        DebugApiService::process(nowMs);
    };
    loopPreIngestModule.begin(loopPreIngestProviders);
}

static void configureConnectionRuntimeModule() {
    ConnectionRuntimeModule::Providers connectionRuntimeProviders;
    connectionRuntimeProviders.isBleConnected = [](void* ctx) -> bool {
        return static_cast<V1BLEClient*>(ctx)->isConnected();
    };
    connectionRuntimeProviders.isBackpressured = [](void* ctx) -> bool {
        return static_cast<BleQueueModule*>(ctx)->isBackpressured();
    };
    connectionRuntimeProviders.getLastRxMillis = [](void* ctx) -> unsigned long {
        return static_cast<BleQueueModule*>(ctx)->getLastRxMillis();
    };
    connectionRuntimeProviders.bleContext = &bleClient;
    connectionRuntimeProviders.queueContext = &bleQueueModule;
    connectionRuntimeModule.begin(connectionRuntimeProviders);
}

static void configureConnectionStateDispatchModule() {
    ConnectionStateDispatchModule::Providers connectionStateDispatchProviders;
    connectionStateDispatchProviders.runCadence = [](void* ctx, const ConnectionStateCadenceContext& cadenceCtx) {
        return static_cast<ConnectionStateCadenceModule*>(ctx)->process(cadenceCtx);
    };
    connectionStateDispatchProviders.cadenceContext = &connectionStateCadenceModule;
    connectionStateDispatchProviders.runConnectionStateProcess = [](void* ctx, uint32_t nowMs) {
        static_cast<ConnectionStateModule*>(ctx)->process(nowMs);
    };
    connectionStateDispatchProviders.connectionStateContext = &connectionStateModule;
    connectionStateDispatchModule.begin(connectionStateDispatchProviders);
}

static void configurePeriodicMaintenanceModule() {
    PeriodicMaintenanceModule::Providers periodicMaintenanceProviders;
    periodicMaintenanceProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    periodicMaintenanceProviders.runPerfReport = [](void*) { perfMetricsCheckReport(); };
    periodicMaintenanceProviders.recordPerfReportUs = [](void*, uint32_t elapsedUs) {
        perfRecordPerfReportUs(elapsedUs);
    };
    periodicMaintenanceProviders.runTimeSave = [](void* ctx, uint32_t nowMs) {
        static_cast<TimeService*>(ctx)->periodicSave(nowMs);
    };
    periodicMaintenanceProviders.timeSaveContext = &timeService;
    periodicMaintenanceProviders.recordTimeSaveUs = [](void*, uint32_t elapsedUs) {
        perfRecordTimeSaveUs(elapsedUs);
    };
    periodicMaintenanceProviders.nowEpochMsOr0 = [](void* ctx) -> int64_t {
        return static_cast<TimeService*>(ctx)->nowEpochMsOr0();
    };
    periodicMaintenanceProviders.epochContext = &timeService;
    periodicMaintenanceProviders.runLockoutLearner = [](void* ctx, uint32_t nowMs, int64_t epochMs) {
        static_cast<LockoutLearner*>(ctx)->process(nowMs, epochMs);
    };
    periodicMaintenanceProviders.lockoutLearnerContext = &lockoutLearner;
    periodicMaintenanceProviders.runLockoutStoreSave = [](void*, uint32_t nowMs) {
        processLockoutStoreSave(nowMs);
    };
    periodicMaintenanceProviders.runLearnerPendingSave = [](void*, uint32_t nowMs) {
        processLearnerPendingSave(nowMs);
    };
    periodicMaintenanceModule.begin(periodicMaintenanceProviders);
}

static void configureLoopTailModule() {
    LoopTailModule::Providers loopTailProviders;
    loopTailProviders.perfTimestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopTailProviders.loopMicrosUs = [](void*) -> uint32_t {
        return micros();
    };
    loopTailProviders.runBleDrain = [](void* ctx) {
        static_cast<BleQueueModule*>(ctx)->process();
    };
    loopTailProviders.bleDrainContext = &bleQueueModule;
    loopTailProviders.recordBleDrainUs = [](void*, uint32_t elapsedUs) {
        perfRecordBleDrainUs(elapsedUs);
    };
    loopTailProviders.yieldOneTick = [](void*) {
        vTaskDelay(pdMS_TO_TICKS(1));
    };
    loopTailModule.begin(loopTailProviders);
}

static void configureLoopTelemetryModule() {
    LoopTelemetryModule::Providers loopTelemetryProviders;
    loopTelemetryProviders.microsNow = [](void*) -> uint32_t {
        return micros();
    };
    loopTelemetryProviders.recordLoopJitterUs = [](void*, uint32_t jitterUs) {
        perfRecordLoopJitterUs(jitterUs);
    };
    loopTelemetryProviders.refreshDmaCache = [](void*) {
        StorageManager::updateDmaHeapCache();
    };
    loopTelemetryProviders.readFreeHeap = [](void*) -> uint32_t {
        return ESP.getFreeHeap();
    };
    loopTelemetryProviders.readLargestHeapBlock = [](void*) -> uint32_t {
        return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    };
    loopTelemetryProviders.readCachedFreeDma = [](void*) -> uint32_t {
        return StorageManager::getCachedFreeDma();
    };
    loopTelemetryProviders.readCachedLargestDma = [](void*) -> uint32_t {
        return StorageManager::getCachedLargestDma();
    };
    loopTelemetryProviders.recordHeapStats =
        [](void*, uint32_t freeHeap, uint32_t largestHeapBlock, uint32_t cachedFreeDma, uint32_t cachedLargestDma) {
            perfRecordHeapStats(freeHeap, largestHeapBlock, cachedFreeDma, cachedLargestDma);
        };
    loopTelemetryModule.begin(loopTelemetryProviders);
}

static void configureLoopIngestModule() {
    LoopIngestModule::Providers loopIngestProviders;
    loopIngestProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopIngestProviders.runBleProcess = [](void* ctx) {
        static_cast<V1BLEClient*>(ctx)->process();
    };
    loopIngestProviders.bleProcessContext = &bleClient;
    loopIngestProviders.recordBleProcessUs = [](void*, uint32_t elapsedUs) {
        perfRecordBleProcessUs(elapsedUs);
    };
    loopIngestProviders.runBleDrain = [](void* ctx) {
        static_cast<BleQueueModule*>(ctx)->process();
    };
    loopIngestProviders.bleDrainContext = &bleQueueModule;
    loopIngestProviders.recordBleDrainUs = [](void*, uint32_t elapsedUs) {
        perfRecordBleDrainUs(elapsedUs);
    };
    loopIngestProviders.readBleBackpressure = [](void* ctx) -> bool {
        return static_cast<BleQueueModule*>(ctx)->isBackpressured();
    };
    loopIngestProviders.bleBackpressureContext = &bleQueueModule;
    loopIngestProviders.runObdRuntime = [](void*,
                                           uint32_t nowMs,
                                           bool obdServiceEnabled) {
        obdRuntimeModule.process(nowMs,
                                 obdServiceEnabled,
                                 obdAutoConnectPending,
                                 obdAutoConnectAtMs,
                                 obdHandler,
                                 speedSourceSelector);
    };
    loopIngestProviders.recordObdUs = [](void*, uint32_t elapsedUs) {
        perfRecordObdUs(elapsedUs);
    };
    loopIngestProviders.runGpsRuntimeUpdate = [](void* ctx, uint32_t nowMs) {
        static_cast<GpsRuntimeModule*>(ctx)->update(nowMs);
    };
    loopIngestProviders.gpsRuntimeContext = &gpsRuntimeModule;
    loopIngestProviders.recordGpsUs = [](void*, uint32_t elapsedUs) {
        perfRecordGpsUs(elapsedUs);
    };
    loopIngestModule.begin(loopIngestProviders);
}

static void configureLoopDisplayModule() {
    LoopDisplayModule::Providers loopDisplayProviders;
    loopDisplayProviders.readDisplayNowMs = [](void*) -> uint32_t {
        return millis();
    };
    loopDisplayProviders.collectParsedSignal = [](void* ctx) -> ParsedFrameSignal {
        BleQueueModule* queue = static_cast<BleQueueModule*>(ctx);
        return ParsedFrameEventModule::collect(
            queue->consumeParsedFlag(),
            queue->getLastParsedTimestamp(),
            systemEventBus);
    };
    loopDisplayProviders.parsedSignalContext = &bleQueueModule;
    loopDisplayProviders.runParsedFrame =
        [](void* ctx, const DisplayOrchestrationParsedContext& parsedCtx) {
            return static_cast<DisplayOrchestrationModule*>(ctx)->processParsedFrame(parsedCtx);
        };
    loopDisplayProviders.parsedFrameContext = &displayOrchestrationModule;
    loopDisplayProviders.runLightweightRefresh =
        [](void* ctx, const DisplayOrchestrationRefreshContext& refreshCtx) {
            return static_cast<DisplayOrchestrationModule*>(ctx)->processLightweightRefresh(refreshCtx);
        };
    loopDisplayProviders.lightweightRefreshContext = &displayOrchestrationModule;
    loopDisplayProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopDisplayProviders.recordLockoutUs = [](void*, uint32_t elapsedUs) {
        perfRecordLockoutUs(elapsedUs);
    };
    loopDisplayProviders.recordDispPipeUs = [](void*, uint32_t elapsedUs) {
        perfRecordDispPipeUs(elapsedUs);
    };
    loopDisplayProviders.recordNotifyToDisplayMs = [](void*, uint32_t elapsedMs) {
        perfRecordNotifyToDisplayMs(elapsedMs);
    };
    loopDisplayModule.begin(loopDisplayProviders);
}

static void configureTouchUiModule() {
    TouchUiModule::Callbacks touchCbs{
        .isWifiSetupActive = [] { return wifiManager.isWifiServiceActive(); },
        .stopWifiSetup = [] { wifiManager.stopSetupMode(true); },
        .startWifi = [] { getWifiOrchestrator().startWifi(); },
        .drawWifiIndicator = [] { display.drawWiFiIndicator(); },
        .restoreDisplay = [] {
            if (bootSplashHoldActive) {
                return;
            }
            if (bleClient.isConnected()) {
                display.forceNextRedraw();
                DisplayState state = parser.getDisplayState();
                if (parser.hasAlerts()) {
                    AlertData priority;
                    if (parser.getRenderablePriorityAlert(priority)) {
                        const auto& alerts = parser.getAllAlerts();
                        display.update(priority, alerts.data(), parser.getAlertCount(), state);
                    } else {
                        display.update(state);
                    }
                } else {
                    display.update(state);
                }
            } else {
                display.forceNextRedraw();
                display.showScanning();
            }
        }
    };
    touchUiModule.begin(&display, &touchHandler, &settingsManager, touchCbs);
}

static void configureAlertAudioDisplayPipeline() {
    // Initialize alert/audio/display pipeline dependencies before BLE starts
    alertPersistenceModule.begin(&bleClient, &parser, &display, &settingsManager);
    voiceModule.begin(&settingsManager, &bleClient);
    speedVolumeModule.begin(&settingsManager, &bleClient, &parser, &voiceModule, &volumeFadeModule);
    volumeFadeModule.begin(&settingsManager);
    displayPipelineModule.begin(&displayMode,
                                &display,
                                &parser,
                                &settingsManager,
                                &bleClient,
                                &alertPersistenceModule,
                                &volumeFadeModule,
                                &voiceModule,
                                &speedVolumeModule,
                                &debugLogger);
}

static void configureSystemLoopModules() {
    systemEventBus.reset();
    bleQueueModule.begin(&bleClient, &parser, &v1ProfileManager, &displayPreviewModule, &powerModule, &systemEventBus);
    configureConnectionRuntimeModule();
    connectionStateModule.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule, &systemEventBus);
    configureConnectionStateDispatchModule();
    configurePeriodicMaintenanceModule();
    configureLoopTailModule();
    configureLoopTelemetryModule();
    configureLoopIngestModule();
    displayRestoreModule.begin(&display, &parser, &bleClient, &displayPreviewModule);
    displayOrchestrationModule.begin(&display,
                                     &bleClient,
                                     &bleQueueModule,
                                     &displayPreviewModule,
                                     &displayRestoreModule,
                                     &parser,
                                     &settingsManager,
                                     &gpsRuntimeModule,
                                     &obdHandler,
                                     &lockoutOrchestrationModule);
    configureLoopDisplayModule();
    configureLoopConnectionEarlyModule();
    configureLoopPowerTouchModule();
    configureLoopPreIngestModule();
    configureLoopSettingsPrepModule();
    configureLoopRuntimeSnapshotModule();
    configureLoopPostDisplayModule();
}

static void configureRuntimeAndLockoutModules() {
    obdHandler.setLinkReadyCallback([]() { return bleClient.isConnected(); });
    obdHandler.setStartScanCallback([]() { bleClient.startOBDScan(); });
    obdHandler.setVwDataEnabled(settingsManager.get().obdVwDataEnabled);
    obdHandler.begin();
    gpsRuntimeModule.begin(settingsManager.get().gpsEnabled);
    speedSourceSelector.begin(settingsManager.get().gpsEnabled);
    configureVoiceSpeedSyncModule();
    cameraRuntimeModule.begin(settingsManager.get().cameraEnabled);
    cameraRuntimeModule.setAlertTuning(settingsManager.get().cameraAlertDistanceFt,
                                       settingsManager.get().cameraAlertPersistSec);
    // Wire lockout store only if not already done during zone-load above.
    // Calling begin() again would reset the dirty flag set by legacy migration.
    if (!lockoutStore.isInitialized()) {
        lockoutStore.begin(&lockoutIndex);
    }
    lockoutEnforcer.begin(&settingsManager, &lockoutIndex, &lockoutStore);
    lockoutOrchestrationModule.begin(&bleClient, &parser, &settingsManager,
                                     &display, &lockoutEnforcer, &lockoutIndex,
                                     &signalCaptureModule, &volumeFadeModule,
                                     &systemEventBus, &perfCounters, &timeService);
    lockoutLearner.begin(&lockoutIndex, &signalObservationLog);
    {
        const V1Settings& settings = settingsManager.get();
        lockoutLearner.setTuning(settings.gpsLockoutLearnerPromotionHits,
                                 settings.gpsLockoutLearnerRadiusE5,
                                 settings.gpsLockoutLearnerFreqToleranceMHz,
                                 settings.gpsLockoutLearnerLearnIntervalHours);
    }
}

static void restorePendingLearnerCandidates() {
    // Restore pending learner candidates (Tier 7 best-effort, non-fatal).
    if (!storageManager.isReady()) {
        return;
    }

    static constexpr const char* LOCKOUT_PENDING_PATH = "/v1simple_lockout_pending.json";
    fs::FS* fs = storageManager.getFilesystem();
    if (fs && fs->exists(LOCKOUT_PENDING_PATH)) {
        File f = fs->open(LOCKOUT_PENDING_PATH, "r");
        if (f && f.size() > 0 && f.size() < 32768) {
            JsonDocument doc;
            const DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err) {
                if (lockoutLearner.fromJson(doc, timeService.nowEpochMsOr0())) {
                    SerialLog.printf("[Learner] Restored %u pending candidates from %s\n",
                                     static_cast<unsigned>(lockoutLearner.activeCandidateCount()),
                                     LOCKOUT_PENDING_PATH);
                } else {
                    SerialLog.printf("[Learner] Ignoring invalid pending file format: %s\n",
                                     LOCKOUT_PENDING_PATH);
                }
            } else {
                SerialLog.printf("[Learner] Pending JSON parse error: %s\n", err.c_str());
            }
        } else if (f) {
            f.close();
        }
    } else {
        SerialLog.println("[Learner] No saved pending candidate file found");
    }
}


void setup() {
    const unsigned long setupStartMs = millis();
    unsigned long setupStageStartMs = setupStartMs;

    // Wait for USB to stabilize after upload
    delay(50);

    // Release GPIO hold from deep sleep (backlight was held off during sleep).
    // Must happen before display init re-configures the pin.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(LCD_BL));

// Backlight is handled in display.begin() (inverted PWM for Waveshare)

    Serial.begin(115200);
    delay(30);   // Conservative USB CDC settle
    
    // PANIC BREADCRUMBS: Log crash info FIRST (before any other init)
    logPanicBreadcrumbs();
    
    // Check NVS health early - before other subsystems start using it
    nvsHealthCheck();

    auto logBootStage = [&](const char* stageName) {
        const unsigned long now = millis();
        SerialLog.printf("[Boot] stage=%s delta=%lu total=%lu\n",
                         stageName,
                         now - setupStageStartMs,
                         now - setupStartMs);
        setupStageStartMs = now;
    };
    auto logBootCheckpoint = [&](const char* label) {
        const unsigned long now = millis();
        SerialLog.printf("[BootTiming] checkpoint=%s total=%lu\n",
                         label,
                         now - setupStartMs);
    };
    
    SerialLog.println("\n===================================");
    SerialLog.println("V1 Gen2 Simple Display");
    SerialLog.println("Firmware: " FIRMWARE_VERSION);
    SerialLog.println("[Build] core-only");
    SerialLog.print("Board: ");
    SerialLog.println(DISPLAY_NAME);
    
    // Check reset reason - if firmware flash, clear BLE bonds
    esp_reset_reason_t resetReason = esp_reset_reason();
    SerialLog.printf("Reset reason: %d ", resetReason);
    if (resetReason == ESP_RST_SW || resetReason == ESP_RST_UNKNOWN) {
        SerialLog.println("(SW/Upload - will clear BLE bonds for clean reconnect)");
    } else if (resetReason == ESP_RST_POWERON) {
        SerialLog.println("(Power-on)");
    } else if (resetReason == ESP_RST_DEEPSLEEP) {
        SerialLog.println("(Wake from deep sleep - RTC clock preserved)");
    } else {
        SerialLog.printf("(Other: %d)\n", resetReason);
    }
    SerialLog.println("===================================\n");
    SerialLog.printf("[BootTiming] reset=%s (%d)\n",
                     resetReasonToString(resetReason),
                     static_cast<int>(resetReason));
    if (resetReason == ESP_RST_DEEPSLEEP) {
        logBootCheckpoint("wake_deepsleep");
    }
    activeScanScreenDwellMs =
        (resetReason == ESP_RST_DEEPSLEEP) ? MIN_SCAN_SCREEN_DWELL_WAKE_MS : MIN_SCAN_SCREEN_DWELL_MS;
    SerialLog.printf("[BootTiming] scan_dwell_target_ms=%lu\n", activeScanScreenDwellMs);
    connectionStateCadenceModule.reset();
    wifiProcessCadenceModule.reset();

    // Runtime PSRAM visibility: board metadata can differ from actual hardware.
    bool psramOk = psramFound();
    uint32_t psramTotal = static_cast<uint32_t>(ESP.getPsramSize());
    uint32_t psramFree = static_cast<uint32_t>(ESP.getFreePsram());
    uint32_t psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    SerialLog.printf("[Memory] PSRAM: found=%s total=%lu free=%lu largest=%lu\n",
                     psramOk ? "yes" : "no",
                     static_cast<unsigned long>(psramTotal),
                     static_cast<unsigned long>(psramFree),
                     static_cast<unsigned long>(psramLargest));
    logBootStage("preflight");
    
    // Initialize battery manager EARLY - needs to latch power on if running on battery
    // This must happen before any long-running init to prevent shutdown
#if defined(DISPLAY_WAVESHARE_349)
    batteryManager.begin();
#endif
    logBootStage("battery");
    
    // Initialize display
    if (!display.begin()) {
        SerialLog.println("Display initialization failed!");
        fatalBootError("Display init failed", false);
    }
    bootReadyDeadlineMs = millis() + 5000;
    
    // Brief post-display settle before settings init
    const unsigned long postDisplaySettleMs = (resetReason == ESP_RST_DEEPSLEEP) ? 2UL : 10UL;
    delay(postDisplaySettleMs);
    SerialLog.printf("[BootTiming] post_display_settle_ms=%lu\n", postDisplaySettleMs);
    logBootStage("display");

    // Initialize settings BEFORE showing any styled screens (need displayStyle setting)
    settingsManager.begin();
    timeService.begin();

#if defined(DISPLAY_WAVESHARE_349)
    powerModule.begin(&batteryManager, &display, &settingsManager);
    powerModule.logStartupStatus();
#endif
    logBootStage("settings");

    // Show boot splash only on true power-on (not crash reboots or firmware uploads)
    if (resetReason == ESP_RST_POWERON) {
        // True cold boot: brief non-blocking splash for immediate visual confirmation
        logBootCheckpoint("splash_begin");
        const unsigned long splashCallStartMs = millis();
        display.showBootSplash();
        SerialLog.printf("[BootTiming] splash_call_ms=%lu\n",
                         millis() - splashCallStartMs);
        bootSplashHoldActive = true;
        bootSplashHoldUntilMs = millis() + BOOT_SPLASH_HOLD_MS;
    } else {
        logBootCheckpoint("wake_ui_scan_begin");
        const unsigned long wakeUiStartMs = millis();
        showInitialScanningScreen();
        SerialLog.printf("[BootTiming] wake_ui_scan_ms=%lu\n",
                         millis() - wakeUiStartMs);
    }
    logBootStage("boot_ui");

    // Initialize display preview driver
    displayPreviewModule.begin(&display);

    // ── Storage / SD mount ────────────────────────────────────────────
    // If you want to show the demo, call display.showDemo() manually elsewhere (e.g., via a button or menu)

    // Mount storage (SD if available, else LittleFS) for profiles and settings
    SerialLog.println("[Setup] Mounting storage...");
    if (storageManager.begin()) {
        SerialLog.printf("[Setup] Storage ready: %s\n", storageManager.statusText().c_str());
        v1ProfileManager.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        v1DeviceStore.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        audio_init_sd();  // Initialize SD-based frequency voice audio

        // Retry settings restore now that SD is mounted
        // (settings.begin() runs before storage, so restore may have failed)
        if (settingsManager.checkAndRestoreFromSD()) {
            // Settings were restored from SD - update display with restored brightness
            display.setBrightness(settingsManager.get().brightness);
        }

        const String restoredLastKnownV1 = normalizeV1DeviceAddress(settingsManager.get().lastV1Address);
        if (restoredLastKnownV1.length() > 0 && v1DeviceStore.isReady()) {
            v1DeviceStore.upsertDevice(restoredLastKnownV1);
        }

        // Validate profile references in auto-push slots
        // Clear references to profiles that don't exist
        settingsManager.validateProfileReferences(v1ProfileManager);
    } else {
        SerialLog.println("[Setup] Storage unavailable - profiles will be disabled");
    }

    uint32_t bootId = nextBootId();
    perfSdLogger.setBootId(bootId);
    signalObservationSdLogger.setBootId(bootId);

    // Standalone perf CSV logger (SD only).
    perfSdLogger.begin(storageManager.isReady() && storageManager.isSDCard());
    if (perfSdLogger.isEnabled()) {
        SerialLog.printf("[PERF] SD logger enabled (%s)\n", perfSdLogger.csvPath());
    } else {
        SerialLog.println("[PERF] SD logger disabled (no SD)");
    }
    signalObservationSdLogger.begin(storageManager.isReady() && storageManager.isSDCard());
    if (signalObservationSdLogger.isEnabled()) {
        SerialLog.printf("[LockoutSD] Candidate logger enabled (%s)\n", signalObservationSdLogger.csvPath());
    } else {
        SerialLog.println("[LockoutSD] Candidate logger disabled (no SD)");
    }

    // Apply persisted Ka lockout policy before loading/sanitizing lockout zones.
    lockoutSetKaLearningEnabled(settingsManager.get().gpsLockoutKaLearningEnabled);

    // Load lockout zones from SD/LittleFS (Tier 7 — best-effort).
    if (storageManager.isReady()) {
        static constexpr const char* LOCKOUT_ZONES_PATH = "/v1simple_lockout_zones.json";
        fs::FS* fs = storageManager.getFilesystem();
        if (fs && fs->exists(LOCKOUT_ZONES_PATH)) {
            File f = fs->open(LOCKOUT_ZONES_PATH, "r");
            if (f && f.size() > 0 && f.size() < 65536) {
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, f);
                f.close();
                if (!err) {
                    const uint32_t legacyRadiusMigrations = normalizeLegacyLockoutRadiusScale(doc);
                    lockoutStore.begin(&lockoutIndex);
                    if (lockoutStore.fromJson(doc)) {
                        SerialLog.printf("[Lockout] Loaded %lu zones from %s\n",
                                         static_cast<unsigned long>(lockoutStore.stats().entriesLoaded),
                                         LOCKOUT_ZONES_PATH);
                        if (legacyRadiusMigrations > 0) {
                            SerialLog.printf("[Lockout] Normalized %lu legacy zone radius values (x10->x1 scale)\n",
                                             static_cast<unsigned long>(legacyRadiusMigrations));
                            // Persist normalized values on the next best-effort save cycle.
                            lockoutStore.markDirty();
                        }
                    }
                } else {
                    SerialLog.printf("[Lockout] JSON parse error: %s\n", err.c_str());
                }
            } else if (f) {
                f.close();
            }
        } else {
            SerialLog.println("[Lockout] No saved zones file found");
        }
    }
    logBootStage("storage");

#ifndef REPLAY_MODE
    // ── BLE init + scan start ────────────────────────────────────────
    // Run AFTER SD restore/validation so BLE proxy settings reflect the
    // restored configuration during the first scan/connection attempt.
    {
        const V1Settings& blePreInitSettings = settingsManager.get();
        logBootCheckpoint("ble_preinit_begin");
        const unsigned long blePreInitStartMs = millis();
        if (!bleClient.initBLE(blePreInitSettings.proxyBLE, blePreInitSettings.proxyName.c_str())) {
            SerialLog.println("BLE pre-initialization failed!");
            fatalBootError("BLE pre-init failed", true);
        }
        SerialLog.printf("[BootTiming] ble_preinit_ms=%lu\n", millis() - blePreInitStartMs);
        logBootStage("ble_preinit");

        // Scan starts in setup; connection state-machine work still waits for
        // the boot-ready gate later in setup().
        bleClient.onDataReceived(onV1Data);
        bleClient.onV1Connected(onV1Connected);
        logBootCheckpoint("ble_callbacks_registered");
        const V1Settings& bleScanSettings = settingsManager.get();
        SerialLog.printf("Starting BLE scan for V1 (proxy: %s, name: %s)\n",
                         bleScanSettings.proxyBLE ? "enabled" : "disabled",
                         bleScanSettings.proxyName.c_str());
        logBootCheckpoint("ble_scan_begin");
        const unsigned long bleScanStartMs = millis();
        if (!bleClient.begin(bleScanSettings.proxyBLE, bleScanSettings.proxyName.c_str())) {
            SerialLog.println("BLE scan failed to start!");
            fatalBootError("BLE scan failed", true);
        }
        SerialLog.printf("[BootTiming] ble_scan_start_ms=%lu\n", millis() - bleScanStartMs);
    }
#endif

    // Initialize auto-push module after settings/profiles are ready
    autoPushModule.begin(&settingsManager, &v1ProfileManager, &bleClient, &display);

    configureTouchUiModule();

    tapGestureModule.begin(&touchHandler,
                           &settingsManager,
                           &display,
                           &bleClient,
                           &parser,
                           &autoPushModule,
                           &alertPersistenceModule,
                           &displayMode);
    logBootStage("ui_modules");

    const V1Settings& bootSettings = settingsManager.get();
    const char* scenario = "default";
#ifdef GIT_SHA
    const char* gitSha = GIT_SHA;
#else
    const char* gitSha = "unknown";
#endif
    const char* resetStr = resetReasonToString(resetReason);
    SerialLog.printf("BOOT bootId=%lu reset=%s git=%s scenario=%s wifi=%s\n",
                    (unsigned long)bootId,
                    resetStr,
                    gitSha,
                    scenario,
                    bootSettings.enableWifi ? "on" : "off");

    // WiFi startup behavior - either auto-start or wait for BOOT button
    if (settingsManager.get().enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled (dev setting)");
    } else {
        SerialLog.println("[WiFi] Off by default - start with BOOT long-press");
    }
    
    // Initialize touch handler early - before BLE to avoid interleaved logs
    SerialLog.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        SerialLog.println("Touch handler initialized successfully");
    } else {
        SerialLog.println("WARNING: Touch handler failed to initialize - continuing anyway");
    }
    logBootStage("touch");
    
    // Initialize BOOT button (GPIO 0) for brightness adjustment
#if defined(DISPLAY_WAVESHARE_349)
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
    const V1Settings& displaySettings = settingsManager.get();
    display.setBrightness(displaySettings.brightness);  // Apply saved brightness
    audio_set_volume(displaySettings.voiceVolume);      // Apply saved voice volume
    SerialLog.printf("[Settings] Applied saved brightness: %d, voice volume: %d\n", 
                    displaySettings.brightness, displaySettings.voiceVolume);
#endif

    configureAlertAudioDisplayPipeline();
    configureSystemLoopModules();
    configureRuntimeAndLockoutModules();

    configureSpeedVolumeRuntimeModule();
    configureWifiRuntimeModule();
    restorePendingLearnerCandidates();
    bootReady = true;
    bleClient.setBootReady(true);
    SerialLog.printf("[Boot] Ready gate opened at %lu ms\n", millis());

#ifndef REPLAY_MODE
    // Absorb BLE scan-stop settle cost in setup rather than first loop iteration.
    // If V1 was found during setup scanning, this processes the SCAN_STOPPING settle
    // (~200 ms on cold boot). If V1 wasn't found yet, this is a ~microsecond no-op.
    {
        const unsigned long absorbStartMs = millis();
        bleClient.process();
        SerialLog.printf("[BootTiming] ble_absorb_ms=%lu\n", millis() - absorbStartMs);
    }
    SerialLog.println("BLE scan active from setup path");
#else
    SerialLog.println("[REPLAY_MODE] BLE disabled - using packet replay for UI testing");
#endif
    logBootStage("core_pipeline");
    
    // WiFi auto-start is deferred to loop() with a V1 settle gate.
    // See WifiBootPolicy::shouldAutoStartWifi() for the gating logic.
    if (settingsManager.get().enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled — will defer until V1 settles or 30 s timeout");
    } else {
        SerialLog.println("Setup complete - BLE scanning, WiFi off until BOOT long-press");
    }
    logBootStage("wifi");
    SerialLog.printf("[Boot] setup total: %lu ms\n", millis() - setupStartMs);
}

void loop() {
    unsigned long loopStartUs = micros();
    // Process audio amp timeout (disables amp after 3s of inactivity)
    audio_process_amp_timeout();
    static unsigned long lastLoopUs = 0;
    unsigned long now = millis();

    LoopConnectionEarlyContext loopConnectionEarlyCtx;
    loopConnectionEarlyCtx.nowMs = now;
    loopConnectionEarlyCtx.nowUs = micros();
    loopConnectionEarlyCtx.lastLoopUs = lastLoopUs;
    loopConnectionEarlyCtx.bootSplashHoldActive = bootSplashHoldActive;
    loopConnectionEarlyCtx.bootSplashHoldUntilMs = bootSplashHoldUntilMs;
    loopConnectionEarlyCtx.initialScanningScreenShown = initialScanningScreenShown;
    const LoopConnectionEarlyResult loopConnectionEarlyResult =
        loopConnectionEarlyModule.process(loopConnectionEarlyCtx);

    bootSplashHoldActive = loopConnectionEarlyResult.bootSplashHoldActive;
    initialScanningScreenShown = loopConnectionEarlyResult.initialScanningScreenShown;

    bool bleConnectedNow = loopConnectionEarlyResult.bleConnectedNow;
    bool bleBackpressure = loopConnectionEarlyResult.bleBackpressure;
    bool skipNonCoreThisLoop = loopConnectionEarlyResult.skipNonCoreThisLoop;
    bool overloadThisLoop = loopConnectionEarlyResult.overloadThisLoop;

    // Process battery/power and touch UI
#if defined(DISPLAY_WAVESHARE_349)
    LoopPowerTouchContext loopPowerTouchCtx;
    loopPowerTouchCtx.nowMs = now;
    loopPowerTouchCtx.loopStartUs = loopStartUs;
    loopPowerTouchCtx.bootButtonPressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);
    const LoopPowerTouchResult loopPowerTouchResult = loopPowerTouchModule.process(loopPowerTouchCtx);
    if (loopPowerTouchResult.shouldReturnEarly) {
        return;  // Skip normal loop processing while in settings mode
    }
#endif

    LoopSettingsPrepContext loopSettingsPrepCtx;
    loopSettingsPrepCtx.nowMs = now;
    const LoopSettingsPrepValues loopSettingsPrepValues = loopSettingsPrepModule.process(loopSettingsPrepCtx);
    const bool obdServiceEnabled = loopSettingsPrepValues.obdServiceEnabled;
    LoopPreIngestContext loopPreIngestCtx;
    loopPreIngestCtx.nowMs = now;
    loopPreIngestCtx.bootReady = bootReady;
    loopPreIngestCtx.bootReadyDeadlineMs = bootReadyDeadlineMs;
    loopPreIngestCtx.obdServiceEnabled = obdServiceEnabled;
#ifdef REPLAY_MODE
    loopPreIngestCtx.replayMode = true;
#endif
    const LoopPreIngestResult loopPreIngestResult = loopPreIngestModule.process(loopPreIngestCtx);
    bootReady = loopPreIngestResult.bootReady;
    const bool runBleProcessThisLoop = loopPreIngestResult.runBleProcessThisLoop;
    auto runBleProcess = []() { bleClient.process(); };
    auto runBleDrain = []() { bleQueueModule.process(); };
    LoopIngestContext loopIngestCtx;
    loopIngestCtx.nowMs = now;
    loopIngestCtx.bleProcessEnabled = runBleProcessThisLoop;
    loopIngestCtx.runBleProcess = runBleProcess;
    loopIngestCtx.runBleDrain = runBleDrain;
    loopIngestCtx.skipNonCoreThisLoop = skipNonCoreThisLoop;
    loopIngestCtx.overloadThisLoop = overloadThisLoop;
    loopIngestCtx.obdServiceEnabled = obdServiceEnabled;
    const LoopIngestResult loopIngestResult = loopIngestModule.process(loopIngestCtx);
    bleBackpressure = loopIngestResult.bleBackpressure;
    const bool skipLateNonCoreThisLoop = loopIngestResult.skipLateNonCoreThisLoop;
    const bool overloadLateThisLoop = loopIngestResult.overloadLateThisLoop;

    VoiceSpeedSyncContext voiceSpeedSyncCtx;
    voiceSpeedSyncCtx.nowMs = now;
    voiceSpeedSyncModule.process(voiceSpeedSyncCtx);

    // No overload guard: handleParsed's internal 25ms throttle gates expensive draws;
    // fade/debounce/gap-recovery remain microsecond-cheap and must run every frame.
    auto runDisplayPipeline = [](uint32_t nowMs, bool lockoutPrioritySuppressed) {
        displayPipelineModule.handleParsed(nowMs, lockoutPrioritySuppressed);
    };
    LoopDisplayContext loopDisplayCtx;
    loopDisplayCtx.nowMs = now;
    loopDisplayCtx.bootSplashHoldActive = bootSplashHoldActive;
    loopDisplayCtx.overloadLateThisLoop = overloadLateThisLoop;
    loopDisplayCtx.enableSignalTraceLogging = loopSettingsPrepValues.enableSignalTraceLogging;
    loopDisplayCtx.runDisplayPipeline = runDisplayPipeline;
    const LoopDisplayResult loopDisplayResult = loopDisplayModule.process(loopDisplayCtx);
    const bool loopSignalPriorityActive = loopDisplayResult.signalPriorityActive;

    // Camera runtime is strictly low-priority and self-gated on overload/non-core.
    // Only a real priority V1 signal preempts camera lifecycle — weak/background
    // alerts (BSM, door openers, etc.) must not suppress camera matching.
    auto runCameraRuntime = [](uint32_t nowMs,
                               bool skipLateNonCoreThisLoop,
                               bool overloadLateThisLoop,
                               bool loopSignalPriorityActive) {
        cameraRuntimeModule.process(
            nowMs,
            skipLateNonCoreThisLoop,
            overloadLateThisLoop,
            loopSignalPriorityActive);
    };
    LoopPostDisplayContext loopPostDisplayPreWifiCtx;
    loopPostDisplayPreWifiCtx.runAutoPushAndCamera = true;
    loopPostDisplayPreWifiCtx.runSpeedAndDispatch = false;
    loopPostDisplayPreWifiCtx.nowMs = now;
    loopPostDisplayPreWifiCtx.skipLateNonCoreThisLoop = skipLateNonCoreThisLoop;
    loopPostDisplayPreWifiCtx.overloadLateThisLoop = overloadLateThisLoop;
    loopPostDisplayPreWifiCtx.loopSignalPriorityActive = loopSignalPriorityActive;
    loopPostDisplayPreWifiCtx.runCameraRuntime = runCameraRuntime;
    loopPostDisplayModule.process(loopPostDisplayPreWifiCtx);

    LoopRuntimeSnapshotContext loopRuntimeSnapshotCtx;
    const LoopRuntimeSnapshotValues loopRuntimeSnapshotValues =
        loopRuntimeSnapshotModule.process(loopRuntimeSnapshotCtx);
    auto runWifiManagerProcess = []() { wifiManager.process(); };
    WifiRuntimeContext wifiRuntimeCtx;
    wifiRuntimeCtx.nowMs = now;
    wifiRuntimeCtx.v1ConnectedAtMs = v1ConnectedAtMs;
    wifiRuntimeCtx.enableWifiAtBoot = loopSettingsPrepValues.enableWifiAtBoot;
    wifiRuntimeCtx.bleConnected = loopRuntimeSnapshotValues.bleConnected;
    wifiRuntimeCtx.canStartDma = loopRuntimeSnapshotValues.canStartDma;
    wifiRuntimeCtx.wifiAutoStartDone = wifiAutoStartDone;
    wifiRuntimeCtx.skipLateNonCoreThisLoop = skipLateNonCoreThisLoop;
    wifiRuntimeCtx.displayPreviewRunning = loopRuntimeSnapshotValues.displayPreviewRunning;
    wifiRuntimeCtx.bootSplashHoldActive = bootSplashHoldActive;
    wifiRuntimeCtx.runWifiManagerProcess = runWifiManagerProcess;
    const WifiRuntimeResult wifiRuntimeResult = wifiRuntimeModule.process(wifiRuntimeCtx);
    wifiAutoStartDone = wifiRuntimeResult.wifiAutoStartDone;
    
    loopTelemetryModule.process(loopStartUs);

    LoopPostDisplayContext loopPostDisplayPostWifiCtx;
    loopPostDisplayPostWifiCtx.runAutoPushAndCamera = false;
    loopPostDisplayPostWifiCtx.runSpeedAndDispatch = true;
    loopPostDisplayPostWifiCtx.nowMs = now;
    loopPostDisplayPostWifiCtx.configuredVoiceVolume = loopSettingsPrepValues.configuredVoiceVolume;
    loopPostDisplayPostWifiCtx.displayUpdateIntervalMs = DISPLAY_UPDATE_MS;
    loopPostDisplayPostWifiCtx.scanScreenDwellMs = activeScanScreenDwellMs;
    loopPostDisplayPostWifiCtx.bootSplashHoldActive = bootSplashHoldActive;
    loopPostDisplayPostWifiCtx.displayPreviewRunning = loopRuntimeSnapshotValues.displayPreviewRunning;
    loopPostDisplayPostWifiCtx.maxProcessGapMs = CONNECTION_STATE_PROCESS_MAX_GAP_MS;
    const LoopPostDisplayResult loopPostDisplayResult = loopPostDisplayModule.process(loopPostDisplayPostWifiCtx);
    now = loopPostDisplayResult.dispatchNowMs;
    bleConnectedNow = loopPostDisplayResult.bleConnectedNow;
    
    // Periodic perf/time/lockout maintenance bundle.
    periodicMaintenanceModule.process(now);

    // End-of-loop tail: opportunistic BLE drain + yield + loop duration capture.
    lastLoopUs = loopTailModule.process(bleBackpressure, loopStartUs);
}
