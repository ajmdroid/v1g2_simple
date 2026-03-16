/**
 * V1 Gen2 Simple Display - Main Application
 * Target: Waveshare ESP32-S3-Touch-LCD-3.49 with Valentine1 Gen2 BLE
 * 
 * Features:
 * - BLE client for V1 Gen2 radar detector
 * - BLE server proxy for companion app compatibility
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
#include "main_loop_phases.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "display_mode.h"
#include "wifi_manager.h"
#include "settings.h"
#include "touch_handler.h"
#include "v1_profiles.h"
#include "v1_devices.h"
#include "battery_manager.h"
#include "storage_manager.h"
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
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/lockout/signal_capture_module.h"
#include "modules/lockout/signal_observation_sd_logger.h"
#include "modules/lockout/lockout_enforcer.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/lockout_band_policy.h"
#include "modules/lockout/lockout_runtime_mute_controller.h"
#include "modules/lockout/lockout_pre_quiet_controller.h"
#include "modules/lockout/road_map_reader.h"
#include "modules/lockout/lockout_orchestration_module.h"
#include "modules/debug/debug_api_service.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/wifi/wifi_boot_policy.h"
#include "modules/wifi/wifi_auto_start_module.h"
#include "modules/wifi/wifi_priority_policy_module.h"
#include "modules/wifi/wifi_visual_sync_module.h"
#include "modules/wifi/wifi_process_cadence_module.h"
#include "modules/wifi/wifi_runtime_module.h"
#include "modules/perf/debug_macros.h"
#include "provider_callback_bindings.h"
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
unsigned long v1ConnectedAtMs = 0;
static bool wifiAutoStartDone = false;

// Display preview driver (color demos)
DisplayPreviewModule displayPreviewModule;
static ConnectionStateCadenceModule connectionStateCadenceModule;

// Lockout orchestration (enforcement + mute + pre-quiet pipeline)
LockoutOrchestrationModule lockoutOrchestrationModule;

void requestColorPreviewHold(uint32_t durationMs) {
    displayPreviewModule.requestHold(durationMs);
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
static void showInitialScanningScreen() {
    if (initialScanningScreenShown) {
        return;
    }
    display.showScanning();
    display.drawProfileIndicator(settingsManager.get().activeSlot);
    initialScanningScreenShown = true;
    connectionStateCadenceModule.onScanningScreenShown(millis());
}

DisplayMode displayMode = DisplayMode::IDLE;

// Voice alert tracking handled by VoiceModule

// Volume fade module - reduce V1 volume after X seconds of continuous alert
VolumeFadeModule volumeFadeModule;

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

static void configureLoopSettingsPrepModule() {
    LoopSettingsPrepModule::Providers loopSettingsPrepProviders;
    loopSettingsPrepProviders.runTapGesture =
        ProviderCallbackBindings::member<TapGestureModule, &TapGestureModule::process>;
    loopSettingsPrepProviders.tapGestureContext = &tapGestureModule;
    loopSettingsPrepProviders.readSettingsValues = [](void* ctx) -> LoopSettingsPrepValues {
        const V1Settings& settings = static_cast<SettingsManager*>(ctx)->get();
        LoopSettingsPrepValues values;
        values.enableWifi = settings.enableWifi;
        values.enableWifiAtBoot = settings.enableWifiAtBoot;
        values.enableSignalTraceLogging = settings.enableSignalTraceLogging;
        return values;
    };
    loopSettingsPrepProviders.settingsContext = &settingsManager;
    loopSettingsPrepModule.begin(loopSettingsPrepProviders);
}

static void configureLoopRuntimeSnapshotModule() {
    LoopRuntimeSnapshotModule::Providers loopRuntimeSnapshotProviders;
    loopRuntimeSnapshotProviders.readBleConnected =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isConnected>;
    loopRuntimeSnapshotProviders.bleConnectedContext = &bleClient;
    loopRuntimeSnapshotProviders.readCanStartDma = [](void* ctx) -> bool {
        return static_cast<WiFiManager*>(ctx)->canStartSetupMode(nullptr, nullptr);
    };
    loopRuntimeSnapshotProviders.canStartDmaContext = &wifiManager;
    loopRuntimeSnapshotProviders.readDisplayPreviewRunning =
        ProviderCallbackBindings::member<DisplayPreviewModule, &DisplayPreviewModule::isRunning>;
    loopRuntimeSnapshotProviders.displayPreviewContext = &displayPreviewModule;
    loopRuntimeSnapshotModule.begin(loopRuntimeSnapshotProviders);
}

static void configureLoopPostDisplayModule() {
    LoopPostDisplayModule::Providers loopPostDisplayProviders;
    loopPostDisplayProviders.runAutoPush =
        ProviderCallbackBindings::member<AutoPushModule, &AutoPushModule::process>;
    loopPostDisplayProviders.autoPushContext = &autoPushModule;
    loopPostDisplayProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopPostDisplayProviders.readDispatchNowMs = [](void*) -> uint32_t {
        return millis();
    };
    loopPostDisplayProviders.readBleConnectedNow =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isConnected>;
    loopPostDisplayProviders.bleConnectedContext = &bleClient;
    loopPostDisplayProviders.runConnectionStateDispatch =
        ProviderCallbackBindings::memberDiscardReturn<ConnectionStateDispatchModule,
                                                      &ConnectionStateDispatchModule::process>;
    loopPostDisplayProviders.connectionDispatchContext = &connectionStateDispatchModule;
    loopPostDisplayModule.begin(loopPostDisplayProviders);
}

static void configureWifiRuntimeModule() {
    WifiRuntimeModule::Providers wifiRuntimeProviders;
    wifiRuntimeProviders.runWifiAutoStartProcess =
        [](void* ctx,
           uint32_t nowMs,
           uint32_t v1ConnectedAtMs,
           bool enableWifi,
           bool enableWifiAtBoot,
           bool bleConnected,
           bool canStartDma,
           bool& wifiAutoStartDone) {
            static_cast<WifiAutoStartModule*>(ctx)->process(
                nowMs,
                v1ConnectedAtMs,
                enableWifi,
                enableWifiAtBoot,
                bleConnected,
                canStartDma,
                wifiAutoStartDone,
                [] { getWifiOrchestrator().startWifi(); },
                [] { wifiManager.markAutoStarted(); });
        };
    wifiRuntimeProviders.wifiAutoStartContext = &wifiAutoStartModule;
    wifiRuntimeProviders.shouldRunWifiProcessingPolicy =
        [](void* ctx, bool enableWifi, bool enableWifiAtBoot, bool wifiAutoStartDone) {
            return isWifiProcessingEnabledPolicy(
                *static_cast<WiFiManager*>(ctx), enableWifi, enableWifiAtBoot, wifiAutoStartDone);
        };
    wifiRuntimeProviders.wifiPolicyContext = &wifiManager;
    wifiRuntimeProviders.perfTimestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    wifiRuntimeProviders.runWifiCadence =
        ProviderCallbackBindings::member<WifiProcessCadenceModule, &WifiProcessCadenceModule::process>;
    wifiRuntimeProviders.wifiCadenceContext = &wifiProcessCadenceModule;
    wifiRuntimeProviders.recordWifiProcessUs = [](void*, uint32_t elapsedUs) {
        perfRecordWifiProcessUs(elapsedUs);
    };
    wifiRuntimeProviders.readWifiServiceActive =
        ProviderCallbackBindings::member<WiFiManager, &WiFiManager::isWifiServiceActive>;
    wifiRuntimeProviders.wifiServiceContext = &wifiManager;
    wifiRuntimeProviders.readWifiConnected =
        ProviderCallbackBindings::member<WiFiManager, &WiFiManager::isConnected>;
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
        ProviderCallbackBindings::member<ConnectionRuntimeModule, &ConnectionRuntimeModule::process>;
    loopConnectionEarlyProviders.connectionRuntimeContext = &connectionRuntimeModule;
    loopConnectionEarlyProviders.showInitialScanning = [](void*) {
        showInitialScanningScreen();
    };
    loopConnectionEarlyProviders.readProxyConnected =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isProxyClientConnected>;
    loopConnectionEarlyProviders.proxyConnectedContext = &bleClient;
    loopConnectionEarlyProviders.readConnectionRssi =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::getConnectionRssi>;
    loopConnectionEarlyProviders.connectionRssiContext = &bleClient;
    loopConnectionEarlyProviders.readProxyRssi =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::getProxyClientRssi>;
    loopConnectionEarlyProviders.proxyRssiContext = &bleClient;
    loopConnectionEarlyProviders.runDisplayEarly =
        ProviderCallbackBindings::member<DisplayOrchestrationModule,
                                         &DisplayOrchestrationModule::processEarly>;
    loopConnectionEarlyProviders.displayEarlyContext = &displayOrchestrationModule;
    loopConnectionEarlyModule.begin(loopConnectionEarlyProviders);
}

static void refreshStorageDmaHeapCache(void*) {
    StorageManager::updateDmaHeapCache();
}

static uint32_t readCurrentFreeHeap(void*) {
    return ESP.getFreeHeap();
}

static uint32_t readCurrentLargestHeapBlock(void*) {
    return static_cast<uint32_t>(heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

static uint32_t readCachedFreeDmaHeap(void*) {
    return StorageManager::getCachedFreeDma();
}

static uint32_t readCachedLargestDmaHeap(void*) {
    return StorageManager::getCachedLargestDma();
}

static void recordHeapStatsSample(void*,
                                  uint32_t freeHeap,
                                  uint32_t largestHeapBlock,
                                  uint32_t cachedFreeDma,
                                  uint32_t cachedLargestDma) {
    perfRecordHeapStats(freeHeap, largestHeapBlock, cachedFreeDma, cachedLargestDma);
}

static void configureLoopPowerTouchModule() {
    LoopPowerTouchModule::Providers loopPowerTouchProviders;
    loopPowerTouchProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopPowerTouchProviders.microsNow = [](void*) -> uint32_t {
        return micros();
    };
    loopPowerTouchProviders.runPowerProcess =
        ProviderCallbackBindings::member<PowerModule, &PowerModule::process>;
    loopPowerTouchProviders.powerContext = &powerModule;
    loopPowerTouchProviders.runTouchUiProcess =
        ProviderCallbackBindings::member<TouchUiModule, &TouchUiModule::process>;
    loopPowerTouchProviders.touchUiContext = &touchUiModule;
    loopPowerTouchProviders.recordTouchUs = [](void*, uint32_t elapsedUs) {
        perfRecordTouchUs(elapsedUs);
    };
    loopPowerTouchProviders.recordLoopJitterUs = [](void*, uint32_t jitterUs) {
        perfRecordLoopJitterUs(jitterUs);
    };
    loopPowerTouchProviders.refreshDmaCache = refreshStorageDmaHeapCache;
    loopPowerTouchProviders.readFreeHeap = readCurrentFreeHeap;
    loopPowerTouchProviders.readLargestHeapBlock = readCurrentLargestHeapBlock;
    loopPowerTouchProviders.readCachedFreeDma = readCachedFreeDmaHeap;
    loopPowerTouchProviders.readCachedLargestDma = readCachedLargestDmaHeap;
    loopPowerTouchProviders.recordHeapStats = recordHeapStatsSample;
    loopPowerTouchModule.begin(loopPowerTouchProviders);
}

static void configureLoopPreIngestModule() {
    LoopPreIngestModule::Providers loopPreIngestProviders;
    loopPreIngestProviders.openBootReadyGate = [](void*, uint32_t nowMs) {
        bleClient.setBootReady(true);
        SerialLog.printf("[Boot] Ready gate opened at %lu ms (timeout)\n", static_cast<unsigned long>(nowMs));
    };
    loopPreIngestProviders.runWifiPriorityApply =
        [](void* ctx, uint32_t nowMs) {
            static_cast<WifiPriorityPolicyModule*>(ctx)->apply(
                nowMs,
                bleClient,
                wifiManager);
        };
    loopPreIngestProviders.wifiPriorityContext = &wifiPriorityPolicyModule;
    loopPreIngestProviders.runDebugApiProcess = [](void*, uint32_t nowMs) {
        DebugApiService::process(nowMs);
    };
    loopPreIngestModule.begin(loopPreIngestProviders);
}

static void configureConnectionRuntimeModule() {
    ConnectionRuntimeModule::Providers connectionRuntimeProviders;
    connectionRuntimeProviders.isBleConnected =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::isConnected>;
    connectionRuntimeProviders.isBackpressured =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::isBackpressured>;
    connectionRuntimeProviders.getLastRxMillis =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::getLastRxMillis>;
    connectionRuntimeProviders.bleContext = &bleClient;
    connectionRuntimeProviders.queueContext = &bleQueueModule;
    connectionRuntimeModule.begin(connectionRuntimeProviders);
}

static void configureConnectionStateDispatchModule() {
    ConnectionStateDispatchModule::Providers connectionStateDispatchProviders;
    connectionStateDispatchProviders.runCadence =
        ProviderCallbackBindings::member<ConnectionStateCadenceModule,
                                         &ConnectionStateCadenceModule::process>;
    connectionStateDispatchProviders.cadenceContext = &connectionStateCadenceModule;
    connectionStateDispatchProviders.runConnectionStateProcess =
        ProviderCallbackBindings::memberDiscardReturn<ConnectionStateModule,
                                                      &ConnectionStateModule::process>;
    connectionStateDispatchProviders.connectionStateContext = &connectionStateModule;
    connectionStateDispatchProviders.recordDecision = [](void*, const ConnectionStateDispatchDecision& decision) {
        PERF_INC(connectionDispatchRuns);
        if (decision.cadence.displayUpdateDue) {
            PERF_INC(connectionCadenceDisplayDue);
        }
        if (decision.cadence.holdScanDwell) {
            PERF_INC(connectionCadenceHoldScanDwell);
        }
        if (decision.elapsedSinceLastProcessMs > 0) {
            PERF_MAX(connectionStateProcessGapMaxMs, decision.elapsedSinceLastProcessMs);
        }
        if (decision.watchdogForced) {
            PERF_INC(connectionStateWatchdogForces);
        }
        if (decision.ranConnectionStateProcess) {
            PERF_INC(connectionStateProcessRuns);
        }
    };
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
    periodicMaintenanceProviders.runTimeSave =
        ProviderCallbackBindings::member<TimeService, &TimeService::periodicSave>;
    periodicMaintenanceProviders.timeSaveContext = &timeService;
    periodicMaintenanceProviders.recordTimeSaveUs = [](void*, uint32_t elapsedUs) {
        perfRecordTimeSaveUs(elapsedUs);
    };
    periodicMaintenanceProviders.runDeferredSettingsBackup =
        ProviderCallbackBindings::member<SettingsManager, &SettingsManager::serviceDeferredBackup>;
    periodicMaintenanceProviders.deferredSettingsBackupContext = &settingsManager;
    periodicMaintenanceProviders.runDeferredBleBondBackup =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::serviceDeferredBondBackup>;
    periodicMaintenanceProviders.deferredBleBondBackupContext = &bleClient;
    periodicMaintenanceProviders.nowEpochMsOr0 =
        ProviderCallbackBindings::member<TimeService, &TimeService::nowEpochMsOr0>;
    periodicMaintenanceProviders.epochContext = &timeService;
    periodicMaintenanceProviders.runLockoutLearner = [](void* ctx, uint32_t nowMs, int64_t epochMs) {
        static_cast<LockoutLearner*>(ctx)->process(nowMs, epochMs, timeService.tzOffsetMinutes());
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
    loopTailProviders.runBleDrain =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::process>;
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
    loopTelemetryProviders.refreshDmaCache = refreshStorageDmaHeapCache;
    loopTelemetryProviders.readFreeHeap = readCurrentFreeHeap;
    loopTelemetryProviders.readLargestHeapBlock = readCurrentLargestHeapBlock;
    loopTelemetryProviders.readCachedFreeDma = readCachedFreeDmaHeap;
    loopTelemetryProviders.readCachedLargestDma = readCachedLargestDmaHeap;
    loopTelemetryProviders.recordHeapStats = recordHeapStatsSample;
    loopTelemetryModule.begin(loopTelemetryProviders);
}

static void configureLoopIngestModule() {
    LoopIngestModule::Providers loopIngestProviders;
    loopIngestProviders.timestampUs = [](void*) -> uint32_t {
        return PERF_TIMESTAMP_US();
    };
    loopIngestProviders.runBleProcess =
        ProviderCallbackBindings::member<V1BLEClient, &V1BLEClient::process>;
    loopIngestProviders.bleProcessContext = &bleClient;
    loopIngestProviders.recordBleProcessUs = [](void*, uint32_t elapsedUs) {
        perfRecordBleProcessUs(elapsedUs);
    };
    loopIngestProviders.runBleDrain =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::process>;
    loopIngestProviders.bleDrainContext = &bleQueueModule;
    loopIngestProviders.recordBleDrainUs = [](void*, uint32_t elapsedUs) {
        perfRecordBleDrainUs(elapsedUs);
    };
    loopIngestProviders.readBleBackpressure =
        ProviderCallbackBindings::member<BleQueueModule, &BleQueueModule::isBackpressured>;
    loopIngestProviders.bleBackpressureContext = &bleQueueModule;
    loopIngestProviders.runGpsRuntimeUpdate =
        ProviderCallbackBindings::member<GpsRuntimeModule, &GpsRuntimeModule::update>;
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
        ProviderCallbackBindings::member<DisplayOrchestrationModule,
                                         &DisplayOrchestrationModule::processParsedFrame>;
    loopDisplayProviders.parsedFrameContext = &displayOrchestrationModule;
    loopDisplayProviders.runLightweightRefresh =
        ProviderCallbackBindings::member<DisplayOrchestrationModule,
                                         &DisplayOrchestrationModule::processLightweightRefresh>;
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

void configureTouchUiModule() {
    TouchUiModule::Callbacks touchCbs{
        .isWifiSetupActive = [] { return wifiManager.isWifiServiceActive(); },
        .stopWifiSetup = [] { wifiManager.stopSetupMode(true); },
        .startWifi = [] { getWifiOrchestrator().startWifi(); },
        .drawWifiIndicator = [] { display.drawWiFiIndicator(); },
        .restoreDisplay = [] {
            if (bootSplashHoldActive) {
                return;
            }
            displayPipelineModule.restoreCurrentOwner(millis());
        }
    };
    touchUiModule.begin(&display, &touchHandler, &settingsManager, touchCbs);
}

static void configureAlertAudioDisplayPipeline() {
    // Initialize alert/audio/display pipeline dependencies before WiFi starts
    alertPersistenceModule.begin(&bleClient, &parser, &display, &settingsManager);
    voiceModule.begin(&settingsManager, &bleClient);
    volumeFadeModule.begin(&settingsManager);
    displayPipelineModule.begin(&displayMode,
                                &display,
                                &parser,
                                &settingsManager,
                                &bleClient,
                                &alertPersistenceModule,
                                &volumeFadeModule,
                                &voiceModule);
}

static void configureSystemLoopCoreModules() {
    systemEventBus.reset();
    bleQueueModule.begin(&bleClient, &parser, &v1ProfileManager, &displayPreviewModule, &powerModule, &systemEventBus);
    configureConnectionRuntimeModule();
    connectionStateModule.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule, &systemEventBus);
    configureConnectionStateDispatchModule();
    configurePeriodicMaintenanceModule();
    configureLoopTailModule();
    configureLoopTelemetryModule();
    configureLoopIngestModule();
    displayRestoreModule.begin(&display,
                               &parser,
                               &bleClient,
                               &displayPreviewModule,
                               &displayPipelineModule);
    displayOrchestrationModule.begin(&display,
                                     &bleClient,
                                     &bleQueueModule,
                                     &displayPreviewModule,
                                     &displayRestoreModule,
                                     &parser,
                                     &settingsManager,
                                     &gpsRuntimeModule,
                                     &lockoutOrchestrationModule);
}

static void configureSystemLoopPhaseModules() {
    configureLoopDisplayModule();
    configureLoopConnectionEarlyModule();
    configureLoopPowerTouchModule();
    configureLoopPreIngestModule();
    configureLoopSettingsPrepModule();
    configureLoopRuntimeSnapshotModule();
    configureLoopPostDisplayModule();
}

static void configureSystemLoopModules() {
    configureSystemLoopCoreModules();
    configureSystemLoopPhaseModules();
}

static void configureRuntimeSensorModules() {
    gpsRuntimeModule.begin(settingsManager.get().gpsEnabled);
    speedSourceSelector.begin(settingsManager.get().gpsEnabled,
                              settingsManager.get().obdEnabled);
    obdRuntimeModule.begin(
        settingsManager.get().obdEnabled,
        settingsManager.get().obdSavedAddress.c_str(),
        settingsManager.get().obdSavedAddrType,
        settingsManager.get().obdMinRssi,
        settingsManager.get().obdCachedVinPrefix11.c_str(),
        settingsManager.get().obdCachedEotProfileId);
}

static void syncObdSettings() {
    const char* runtimeAddress = obdRuntimeModule.getSavedAddress();
    const char* cachedVinPrefix11 = obdRuntimeModule.getCachedVinPrefix11();
    const uint8_t cachedEotProfileId = obdRuntimeModule.getCachedEotProfileId();
    V1Settings& settings = settingsManager.mutableSettings();
    bool changed = false;

    if (settings.obdSavedAddress != runtimeAddress) {
        settings.obdSavedAddress = runtimeAddress;
        changed = true;
    }
    const uint8_t runtimeAddrType = obdRuntimeModule.getSavedAddrType();
    if (settings.obdSavedAddrType != runtimeAddrType) {
        settings.obdSavedAddrType = runtimeAddrType;
        changed = true;
    }
    if (settings.obdCachedVinPrefix11 != cachedVinPrefix11) {
        settings.obdCachedVinPrefix11 = cachedVinPrefix11;
        changed = true;
    }
    if (settings.obdCachedEotProfileId != cachedEotProfileId) {
        settings.obdCachedEotProfileId = cachedEotProfileId;
        changed = true;
    }

    if (!changed) {
        return;
    }

    settingsManager.save();
}

static void configureRuntimeCoreModules() {
    configureRuntimeSensorModules();
}

static void configureLockoutPipelineModules() {
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
                                 settings.gpsLockoutLearnerLearnIntervalHours,
                                 settings.gpsLockoutMaxHdopX10,
                                 settings.gpsLockoutMinLearnerSpeedMph);
    }
}

static void configureRuntimeAndLockoutModules() {
    configureRuntimeCoreModules();
    configureLockoutPipelineModules();
}

template <typename StageLogger>
static void finalizeBootReadyAndBleScan(const unsigned long setupStartMs,
                                        const StageLogger& logBootStage) {
    restorePendingLearnerCandidates();
    bootReady = true;
    bleClient.setBootReady(true);
    SerialLog.printf("[Boot] Ready gate opened at %lu ms\n", millis());

#ifndef REPLAY_MODE
    // Absorb BLE scan-stop settle cost in setup rather than first loop iteration.
    // Keep this call after bootReady/setBootReady to avoid starving BLE state
    // transitions that gate connectionStateModule.process() and scanning UI flow.
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
    const V1Settings& wifiSettings = settingsManager.get();
    if (!wifiSettings.enableWifi) {
        SerialLog.println("[WiFi] Master disabled in settings — startup and loop processing skipped");
    } else if (wifiSettings.enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled — will defer until V1 settles or 30 s timeout");
    } else {
        SerialLog.println("Setup complete - BLE scanning, WiFi off until BOOT long-press");
    }
    logBootStage("wifi");
    SerialLog.printf("[Boot] setup total: %lu ms\n", millis() - setupStartMs);
}

template <typename CheckpointLogger>
static esp_reset_reason_t initializeResetReasonAndCadenceState(
    const CheckpointLogger& logBootCheckpoint) {
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
    return resetReason;
}

template <typename CheckpointLogger, typename StageLogger>
static void initializeBlePreInitAndScan(const CheckpointLogger& logBootCheckpoint,
                                        const StageLogger& logBootStage) {
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
#else
    (void)logBootCheckpoint;
    (void)logBootStage;
#endif
}

template <typename CheckpointLogger, typename StageLogger>
static void initializePreflightDisplayAndBootUi(esp_reset_reason_t resetReason,
                                                 const CheckpointLogger& logBootCheckpoint,
                                                 const StageLogger& logBootStage) {
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

    // Initialize battery manager EARLY - needs to latch power on if running on battery.
    // This must happen before any long-running init to prevent shutdown.
    batteryManager.begin();
    logBootStage("battery");

    // Initialize display.
    if (!display.begin()) {
        SerialLog.println("Display initialization failed!");
        fatalBootError("Display init failed", false);
    }
    bootReadyDeadlineMs = millis() + 5000;

    // Brief post-display settle before settings init.
    const unsigned long postDisplaySettleMs = (resetReason == ESP_RST_DEEPSLEEP) ? 2UL : 10UL;
    delay(postDisplaySettleMs);
    SerialLog.printf("[BootTiming] post_display_settle_ms=%lu\n", postDisplaySettleMs);
    logBootStage("display");

    // Initialize settings BEFORE showing any styled screens (need displayStyle setting).
    settingsManager.begin();
    timeService.begin();

    powerModule.begin(&batteryManager, &display, &settingsManager);
    powerModule.logStartupStatus();
    logBootStage("settings");

    // Show boot splash only on true power-on (not crash reboots or firmware uploads).
    if (resetReason == ESP_RST_POWERON) {
        // True cold boot: brief non-blocking splash for immediate visual confirmation.
        logBootCheckpoint("splash_begin");
        const unsigned long splashCallStartMs = millis();
        display.showBootSplash();
        SerialLog.printf("[BootTiming] splash_call_ms=%lu\n", millis() - splashCallStartMs);
        bootSplashHoldActive = true;
        bootSplashHoldUntilMs = millis() + BOOT_SPLASH_HOLD_MS;
    } else {
        logBootCheckpoint("wake_ui_scan_begin");
        const unsigned long wakeUiStartMs = millis();
        showInitialScanningScreen();
        SerialLog.printf("[BootTiming] wake_ui_scan_ms=%lu\n", millis() - wakeUiStartMs);
    }
    logBootStage("boot_ui");

    // Initialize display preview driver.
    displayPreviewModule.begin(&display);
}

template <typename CheckpointLogger, typename StageLogger>
static void initializeStorageToReadyFlow(esp_reset_reason_t resetReason,
                                         const unsigned long setupStartMs,
                                         const CheckpointLogger& logBootCheckpoint,
                                         const StageLogger& logBootStage) {
    // ── Storage / SD mount ────────────────────────────────────────────
    // If you want to show the demo, call display.showDemo() manually elsewhere (e.g., via a button or menu).
    initializeStorageAndProfiles();

    const uint32_t bootId = initializeBootPerformanceLoggers();

    applyLockoutPolicyAndLoadZonesFromStorage();
    roadMapReader.begin();
    logBootStage("storage");

    initializeBlePreInitAndScan(logBootCheckpoint, logBootStage);

    configureUiInteractionModules();
    logBootStage("ui_modules");

    logBootSummaryAndWifiStartup(bootId, resetReason);

    initializeTouchAndDisplayControls();
    logBootStage("touch");

    configureAlertAudioDisplayPipeline();
    configureSystemLoopModules();
    configureRuntimeAndLockoutModules();

    configureWifiRuntimeModule();
    finalizeBootReadyAndBleScan(setupStartMs, logBootStage);
}

void setup() {
    const unsigned long setupStartMs = millis();
    unsigned long setupStageStartMs = setupStartMs;

    initializeEarlyBootDiagnostics();

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
    
    esp_reset_reason_t resetReason = initializeResetReasonAndCadenceState(logBootCheckpoint);

    initializePreflightDisplayAndBootUi(resetReason, logBootCheckpoint, logBootStage);

    initializeStorageToReadyFlow(resetReason, setupStartMs, logBootCheckpoint, logBootStage);
}

void loop() {
    unsigned long loopStartUs = micros();
    // Process audio amp timeout (disables amp after 3s of inactivity)
    audio_process_amp_timeout();
    static unsigned long lastLoopUs = 0;
    unsigned long now = millis();
    const LoopConnectionEarlyPhaseValues loopConnectionEarlyValues = processLoopConnectionEarlyPhase(
        now,
        micros(),
        lastLoopUs,
        bootSplashHoldActive,
        bootSplashHoldUntilMs,
        initialScanningScreenShown);

    bootSplashHoldActive = loopConnectionEarlyValues.bootSplashHoldActive;
    initialScanningScreenShown = loopConnectionEarlyValues.initialScanningScreenShown;

    bool bleConnectedNow = loopConnectionEarlyValues.bleConnectedNow;
    bool bleBackpressure = loopConnectionEarlyValues.bleBackpressure;
    bool skipNonCoreThisLoop = loopConnectionEarlyValues.skipNonCoreThisLoop;
    bool overloadThisLoop = loopConnectionEarlyValues.overloadThisLoop;

    // Process battery/power and touch UI.
    if (shouldReturnEarlyFromLoopPowerTouchPhase(now, loopStartUs)) {
        return;  // Skip normal loop processing while in settings mode.
    }

    auto runBleProcess = []() { bleClient.process(); };
    auto runBleDrain = []() { bleQueueModule.process(); };
    const LoopIngestPhaseValues loopIngestValues = processLoopIngestPhase(
        now,
        bootReady,
        bootReadyDeadlineMs,
        skipNonCoreThisLoop,
        overloadThisLoop,
        runBleProcess,
        runBleDrain);
    const LoopSettingsPrepValues& loopSettingsPrepValues = loopIngestValues.loopSettingsPrepValues;
    bootReady = loopIngestValues.bootReady;
    bleBackpressure = loopIngestValues.bleBackpressure;
    const bool skipLateNonCoreThisLoop = loopIngestValues.skipLateNonCoreThisLoop;
    const bool overloadLateThisLoop = loopIngestValues.overloadLateThisLoop;

    // No overload guard: handleParsed's internal 25ms throttle gates expensive draws;
    // fade/debounce/gap-recovery remain microsecond-cheap and must run every frame.
    auto runDisplayPipeline = [](uint32_t nowMs, bool lockoutPrioritySuppressed) {
        displayPipelineModule.handleParsed(nowMs, lockoutPrioritySuppressed);
    };

    const LoopDisplayPreWifiPhaseValues loopDisplayPreWifiValues = processLoopDisplayPreWifiPhase(
        now,
        bootSplashHoldActive,
        overloadLateThisLoop,
        loopSettingsPrepValues.enableSignalTraceLogging,
        skipLateNonCoreThisLoop,
        runDisplayPipeline);
    const bool loopSignalPriorityActive = loopDisplayPreWifiValues.loopSignalPriorityActive;

    // OBD runtime update — between DisplayPreWifi and WiFi phases.
    {
        const uint32_t obdStartUs = micros();
        obdRuntimeModule.update(now, bootReady, bleConnectedNow, !bleClient.isScanning());
        perfRecordObdUs(micros() - obdStartUs);
    }
    speedSourceSelector.update(now);
    syncObdSettings();

    auto runWifiManagerProcess = []() { wifiManager.process(); };
    const LoopWifiPhaseValues loopWifiValues = processLoopWifiPhase(
        now,
        v1ConnectedAtMs,
        loopSettingsPrepValues.enableWifi,
        loopSettingsPrepValues.enableWifiAtBoot,
        wifiAutoStartDone,
        skipLateNonCoreThisLoop,
        bootSplashHoldActive,
        runWifiManagerProcess);
    const LoopRuntimeSnapshotValues& loopRuntimeSnapshotValues = loopWifiValues.loopRuntimeSnapshotValues;
    wifiAutoStartDone = loopWifiValues.wifiAutoStartDone;
    
    loopTelemetryModule.process(loopStartUs);

    const LoopFinalizePhaseValues loopFinalizeValues = processLoopFinalizePhase(
        now,
        loopSettingsPrepValues,
        bootSplashHoldActive,
        loopRuntimeSnapshotValues.displayPreviewRunning,
        bleBackpressure,
        activeScanScreenDwellMs,
        CONNECTION_STATE_PROCESS_MAX_GAP_MS,
        loopStartUs);
    now = loopFinalizeValues.dispatchNowMs;
    bleConnectedNow = loopFinalizeValues.bleConnectedNow;
    lastLoopUs = loopFinalizeValues.lastLoopUs;
}
