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
#include "modules/display/display_pipeline_module.h"
#include "modules/display/display_orchestration_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/system/parsed_frame_event_module.h"
#include "esp_heap_caps.h"
#include "modules/voice/voice_module.h"
#include "modules/speed_volume/speed_volume_module.h"
#include "modules/speed_volume/speaker_quiet_sync_module.h"
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

static bool bootReady = false;
static unsigned long bootReadyDeadlineMs = 0;
static bool bootSplashHoldActive = false;
static unsigned long bootSplashHoldUntilMs = 0;
static bool initialScanningScreenShown = false;
static constexpr unsigned long BOOT_SPLASH_HOLD_MS = 400;
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_MS = 400;
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_WAKE_MS = 120;
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

// Auto-push profile state machine
AutoPushModule autoPushModule;
TouchUiModule touchUiModule;
TapGestureModule tapGestureModule;
PowerModule powerModule;
BleQueueModule bleQueueModule;
ConnectionStateModule connectionStateModule;
ConnectionRuntimeModule connectionRuntimeModule;
DisplayPipelineModule displayPipelineModule;
DisplayOrchestrationModule displayOrchestrationModule;
DisplayRestoreModule displayRestoreModule;
SystemEventBus systemEventBus;
WifiAutoStartModule wifiAutoStartModule;
WifiPriorityPolicyModule wifiPriorityPolicyModule;
WifiVisualSyncModule wifiVisualSyncModule;
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

    // Touch/UI module callbacks
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
    systemEventBus.reset();
    bleQueueModule.begin(&bleClient, &parser, &v1ProfileManager, &displayPreviewModule, &powerModule, &systemEventBus);
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
    connectionStateModule.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule, &systemEventBus);
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
    obdHandler.setLinkReadyCallback([]() { return bleClient.isConnected(); });
    obdHandler.setStartScanCallback([]() { bleClient.startOBDScan(); });
    obdHandler.setVwDataEnabled(settingsManager.get().obdVwDataEnabled);
    obdHandler.begin();
    gpsRuntimeModule.begin(settingsManager.get().gpsEnabled);
    speedSourceSelector.begin(settingsManager.get().gpsEnabled);
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
    // Restore pending learner candidates (Tier 7 best-effort, non-fatal).
    if (storageManager.isReady()) {
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

    const auto connectionSnapshot = connectionRuntimeModule.process(
        now,
        micros(),
        lastLoopUs,
        bootSplashHoldActive,
        bootSplashHoldUntilMs,
        initialScanningScreenShown);

    bootSplashHoldActive = connectionSnapshot.bootSplashHoldActive;
    initialScanningScreenShown = connectionSnapshot.initialScanningScreenShown;
    if (connectionSnapshot.requestShowInitialScanning) {
        showInitialScanningScreen();
    }

    bool bleConnectedNow = connectionSnapshot.connected;
    bool bleBackpressure = connectionSnapshot.backpressured;
    bool skipNonCoreThisLoop = connectionSnapshot.skipNonCore;
    bool overloadThisLoop = connectionSnapshot.overloaded;

    DisplayOrchestrationEarlyContext displayEarlyCtx;
    displayEarlyCtx.nowMs = now;
    displayEarlyCtx.bootSplashHoldActive = bootSplashHoldActive;
    displayEarlyCtx.overloadThisLoop = overloadThisLoop;
    displayEarlyCtx.bleContext = {
        bleConnectedNow,
        bleClient.isProxyClientConnected(),
        bleClient.getConnectionRssi(),
        bleClient.getProxyClientRssi()
    };
    displayEarlyCtx.bleReceiving = connectionSnapshot.receiving;
    displayOrchestrationModule.processEarly(displayEarlyCtx);

    // Process battery/power and touch UI
#if defined(DISPLAY_WAVESHARE_349)
    powerModule.process(now);
    {
        uint32_t touchStartUs = PERF_TIMESTAMP_US();
        bool inSettings = touchUiModule.process(now, (digitalRead(BOOT_BUTTON_GPIO) == LOW));
        perfRecordTouchUs(PERF_TIMESTAMP_US() - touchStartUs);
        if (inSettings) {
            perfRecordLoopJitterUs(micros() - loopStartUs);
            StorageManager::updateDmaHeapCache();  // Keep DMA cache fresh
            perfRecordHeapStats(ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                                StorageManager::getCachedFreeDma(), StorageManager::getCachedLargestDma());
            return;  // Skip normal loop processing while in settings mode
        }
    }
#endif

    tapGestureModule.process(now);
    
#ifndef REPLAY_MODE
    if (!bootReady && millis() >= bootReadyDeadlineMs) {
        bootReady = true;
        bleClient.setBootReady(true);
        SerialLog.printf("[Boot] Ready gate opened at %lu ms (timeout)\n", millis());
    }

    const V1Settings& loopSettings = settingsManager.get();
    const bool obdServiceEnabled = loopSettings.obdEnabled;
    wifiPriorityPolicyModule.apply(now, obdServiceEnabled, bleClient, wifiManager, obdHandler);
    
    // Process BLE events (includes blocking connect/discovery during reconnect)
    uint32_t bleProcessStartUs = PERF_TIMESTAMP_US();
    bleClient.process();
    perfRecordBleProcessUs(PERF_TIMESTAMP_US() - bleProcessStartUs);
#endif
    
    DebugApiService::process(now);

    // Process queued BLE data (safe for SPI - runs in main loop context)
    uint32_t bleDrainStartUs = PERF_TIMESTAMP_US();
    bleQueueModule.process();
    perfRecordBleDrainUs(PERF_TIMESTAMP_US() - bleDrainStartUs);
    bleBackpressure = bleQueueModule.isBackpressured();
    const bool skipLateNonCoreThisLoop = skipNonCoreThisLoop || bleBackpressure;
    const bool overloadLateThisLoop = overloadThisLoop || bleBackpressure;

    uint32_t obdStartUs = PERF_TIMESTAMP_US();
    obdRuntimeModule.process(now,
                             obdServiceEnabled,
                             obdAutoConnectPending,
                             obdAutoConnectAtMs,
                             obdHandler,
                             speedSourceSelector);
    if (obdServiceEnabled) {
        perfRecordObdUs(PERF_TIMESTAMP_US() - obdStartUs);
    }

    uint32_t gpsStartUs = PERF_TIMESTAMP_US();
    gpsRuntimeModule.update(now);
    perfRecordGpsUs(PERF_TIMESTAMP_US() - gpsStartUs);

    SpeedSelection speedSelection;
    if (speedSourceSelector.select(now, speedSelection)) {
        voiceModule.updateSpeedSample(speedSelection.speedMph, speedSelection.timestampMs);
    } else {
        voiceModule.clearSpeedSample();
    }
    
    const ParsedFrameSignal parsedSignal = ParsedFrameEventModule::collect(
        bleQueueModule.consumeParsedFlag(),
        bleQueueModule.getLastParsedTimestamp(),
        systemEventBus);
    const bool parsedReady = parsedSignal.parsedReady;
    const uint32_t parsedTsMs = parsedSignal.parsedTsMs;

    // Drive display orchestration separately from BLE drain for better timing attribution.
    const uint32_t displayNowMs = millis();
    DisplayOrchestrationParsedContext parsedDisplayCtx;
    parsedDisplayCtx.nowMs = displayNowMs;
    parsedDisplayCtx.parsedReady = parsedReady;
    parsedDisplayCtx.bootSplashHoldActive = bootSplashHoldActive;
    parsedDisplayCtx.enableSignalTraceLogging = loopSettings.enableSignalTraceLogging;
    const uint32_t lockoutStartUs = PERF_TIMESTAMP_US();
    const auto parsedDisplayResult = displayOrchestrationModule.processParsedFrame(parsedDisplayCtx);
    if (parsedDisplayResult.lockoutEvaluated) {
        perfRecordLockoutUs(PERF_TIMESTAMP_US() - lockoutStartUs);
    }

    bool pipelineRanThisLoop = false;
    if (parsedDisplayResult.runDisplayPipeline) {
        if (parsedTsMs != 0 && displayNowMs >= parsedTsMs) {
            perfRecordNotifyToDisplayMs(displayNowMs - parsedTsMs);
        }
        // No overload guard: handleParsed's internal 25ms throttle gates expensive draws;
        // fade/debounce/gap-recovery remain microsecond-cheap and must run every frame.
        uint32_t dispPipeStartUs = PERF_TIMESTAMP_US();
        displayPipelineModule.handleParsed(displayNowMs, parsedDisplayResult.lockoutPrioritySuppressed);
        perfRecordDispPipeUs(PERF_TIMESTAMP_US() - dispPipeStartUs);
        pipelineRanThisLoop = true;
    }

    DisplayOrchestrationRefreshContext refreshCtx;
    refreshCtx.nowMs = displayNowMs;
    refreshCtx.bootSplashHoldActive = bootSplashHoldActive;
    refreshCtx.overloadLateThisLoop = overloadLateThisLoop;
    refreshCtx.pipelineRanThisLoop = pipelineRanThisLoop;
    const auto refreshResult = displayOrchestrationModule.processLightweightRefresh(refreshCtx);
    const bool loopSignalPriorityActive = refreshResult.signalPriorityActive;

    // Drive auto-push state machine (non-blocking)
    autoPushModule.process();

    // Camera runtime is strictly low-priority and self-gated on overload/non-core.
    // Only a real priority V1 signal preempts camera lifecycle — weak/background
    // alerts (BSM, door openers, etc.) must not suppress camera matching.
    uint32_t cameraStartUs = PERF_TIMESTAMP_US();
    {
        cameraRuntimeModule.process(now, skipLateNonCoreThisLoop, overloadLateThisLoop, loopSignalPriorityActive);
    }
    perfRecordCameraUs(PERF_TIMESTAMP_US() - cameraStartUs);

    wifiAutoStartModule.process(
        now,
        v1ConnectedAtMs,
        loopSettings.enableWifiAtBoot,
        bleClient.isConnected(),
        wifiManager.canStartSetupMode(nullptr, nullptr),
        wifiAutoStartDone,
        [] { getWifiOrchestrator().startWifi(); },
        [] { wifiManager.markAutoStarted(); });

    // Process WiFi/web server only when WiFi is actually enabled.
    // Explicit gate keeps this path a near-no-op in wifi-off profiles.
    if (!skipLateNonCoreThisLoop &&
        isWifiProcessingEnabledPolicy(wifiManager, loopSettings.enableWifiAtBoot, wifiAutoStartDone)) {
        // Poll WiFi/web stack at a bounded cadence to reduce loop overhead while
        // preserving sub-frame UI responsiveness and connect-state progression.
        static uint32_t lastWifiProcessUs = 0;
        constexpr uint32_t WIFI_PROCESS_MIN_INTERVAL_US = 2000;  // 500 Hz max
        const uint32_t nowProcessUs = PERF_TIMESTAMP_US();
        if (lastWifiProcessUs == 0 ||
            static_cast<uint32_t>(nowProcessUs - lastWifiProcessUs) >= WIFI_PROCESS_MIN_INTERVAL_US) {
            uint32_t wifiStartUs = PERF_TIMESTAMP_US();
            wifiManager.process();
            perfRecordWifiProcessUs(PERF_TIMESTAMP_US() - wifiStartUs);
            lastWifiProcessUs = nowProcessUs;
        }
    }

    const bool wifiVisualActiveNow =
        wifiManager.isWifiServiceActive() || wifiManager.isConnected();
    const unsigned long wifiVisualNowMs = millis();
    wifiVisualSyncModule.process(
        wifiVisualNowMs,
        wifiVisualActiveNow,
        displayPreviewModule.isRunning(),
        bootSplashHoldActive,
        [] {
            display.drawWiFiIndicator();
            const int leftColWidth = 64;
            const int leftColHeight = 96;
            display.flushRegion(0, SCREEN_HEIGHT - leftColHeight, leftColWidth, leftColHeight);
        });
    
    perfRecordLoopJitterUs(micros() - loopStartUs);
    StorageManager::updateDmaHeapCache();  // Keep DMA cache fresh for SD gating
    perfRecordHeapStats(ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                        StorageManager::getCachedFreeDma(), StorageManager::getCachedLargestDma());
    
    // Speed-based volume: delegate to module (rate-limited internally)
    speedVolumeModule.process(now);
    
    speakerQuietSyncModule.process(
        speedVolumeModule.isQuietActive(),
        speedVolumeModule.getQuietVolume(),
        settingsManager.get().voiceVolume,
        [](uint8_t volume) { audio_set_volume(volume); });
    
    // Display cadence and scan-dwell gate for connection state transitions.
    now = millis();
    bleConnectedNow = bleClient.isConnected();
    ConnectionStateCadenceContext cadenceCtx;
    cadenceCtx.nowMs = now;
    cadenceCtx.displayUpdateIntervalMs = DISPLAY_UPDATE_MS;
    cadenceCtx.scanScreenDwellMs = activeScanScreenDwellMs;
    cadenceCtx.bleConnectedNow = bleConnectedNow;
    cadenceCtx.bootSplashHoldActive = bootSplashHoldActive;
    cadenceCtx.displayPreviewRunning = displayPreviewModule.isRunning();
    const ConnectionStateCadenceDecision cadenceDecision =
        connectionStateCadenceModule.process(cadenceCtx);

    if (cadenceDecision.shouldRunConnectionStateProcess) {
        // Handle connection state transitions (connect/disconnect, stale data re-request).
        connectionStateModule.process(now);
    }
    
    // Periodic perf metrics report (stability diagnostics)
    {
        uint32_t perfReportStartUs = PERF_TIMESTAMP_US();
        perfMetricsCheckReport();
        perfRecordPerfReportUs(PERF_TIMESTAMP_US() - perfReportStartUs);
    }

    // Periodic time persistence (every 5 min) — ensures NVS has a recent epoch
    // for restoration after deep sleep battery death or hard power loss.
    {
        uint32_t timeSaveStartUs = PERF_TIMESTAMP_US();
        timeService.periodicSave(now);
        perfRecordTimeSaveUs(PERF_TIMESTAMP_US() - timeSaveStartUs);
    }

    // Lockout learner: ingest observations, manage candidates, promote (Tier 7)
    lockoutLearner.process(now, timeService.nowEpochMsOr0());

    // Lockout store: periodic save when dirty (Tier 7 — best-effort, never block)
    processLockoutStoreSave(now);
    // Learner pending candidates: periodic best-effort save (Tier 7).
    processLearnerPendingSave(now);

    // If BLE ingest was backpressured this loop, do one late opportunistic drain
    // so queued notifications don't sit through the sleep + next-loop startup.
    if (bleBackpressure) {
        uint32_t bleDrainLateStartUs = PERF_TIMESTAMP_US();
        bleQueueModule.process();
        perfRecordBleDrainUs(PERF_TIMESTAMP_US() - bleDrainLateStartUs);
    }

    // Short FreeRTOS delay to yield CPU without capping loop at ~200 Hz
    vTaskDelay(pdMS_TO_TICKS(1));
    lastLoopUs = micros() - loopStartUs;
}
