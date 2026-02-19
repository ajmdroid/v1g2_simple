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
#include "modules/display/display_pipeline_module.h"
#include "modules/system/system_event_bus.h"
#include "esp_heap_caps.h"
#include "modules/voice/voice_module.h"
#include "modules/speed_volume/speed_volume_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/gps/gps_lockout_safety.h"
#include "modules/camera/camera_runtime_module.h"
#include "modules/lockout/signal_capture_module.h"
#include "modules/lockout/signal_observation_sd_logger.h"
#include "modules/lockout/lockout_enforcer.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/lockout_band_policy.h"
#include "modules/lockout/lockout_runtime_mute_controller.h"
#include "modules/lockout/lockout_pre_quiet_controller.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/wifi/wifi_boot_policy.h"
#include "modules/perf/debug_macros.h"
#include "time_service.h"
#include <driver/gpio.h>
#include <esp_sleep.h>
#include "../include/display_driver.h"
#include <FS.h>
#include <driver/gpio.h>
#include <algorithm>
#include <cmath>

// Global objects
V1BLEClient bleClient;
PacketParser parser;
V1Display display;
TouchHandler touchHandler;

// Alert persistence module
AlertPersistenceModule alertPersistenceModule;

// Voice Module - handles voice announcement decisions
VoiceModule voiceModule;

unsigned long lastDisplayUpdate = 0;
static bool bootReady = false;
static unsigned long bootReadyDeadlineMs = 0;
static bool bootSplashHoldActive = false;
static unsigned long bootSplashHoldUntilMs = 0;
static bool initialScanningScreenShown = false;
static constexpr unsigned long BOOT_SPLASH_HOLD_MS = 400;
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_MS = 400;
static constexpr unsigned long MIN_SCAN_SCREEN_DWELL_WAKE_MS = 120;
static unsigned long activeScanScreenDwellMs = MIN_SCAN_SCREEN_DWELL_MS;
static unsigned long scanScreenEnteredMs = 0;
static bool scanScreenDwellActive = false;
static bool lastBleConnectedForScanDwell = false;
static bool obdAutoConnectPending = false;
static unsigned long obdAutoConnectAtMs = 0;
static unsigned long v1ConnectedAtMs = 0;
static bool wifiAutoStartDone = false;

// Display preview driver (color + camera demos)
DisplayPreviewModule displayPreviewModule;

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
    scanScreenEnteredMs = millis();
    scanScreenDwellActive = true;
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

// Auto-push profile state machine
AutoPushModule autoPushModule;
TouchUiModule touchUiModule;
TapGestureModule tapGestureModule;
PowerModule powerModule;
BleQueueModule bleQueueModule;
ConnectionStateModule connectionStateModule;
DisplayPipelineModule displayPipelineModule;
DisplayRestoreModule displayRestoreModule;
SystemEventBus systemEventBus;

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

static bool isWifiProcessingEnabled(const V1Settings& settings) {
    return wifiManager.isSetupModeActive() ||
           wifiManager.isConnected() ||
           (settings.enableWifiAtBoot && !wifiAutoStartDone);
}

static void updateBleWifiPriority(unsigned long now, bool obdServiceEnabled) {
    // WiFi priority mode: web UI can deprioritize BLE background work, but never
    // at the expense of establishing/maintaining V1 connectivity.
    // Hysteresis: different timeouts for enable vs disable prevent oscillation.
    // Min-hold guard: once a transition fires, suppress further transitions for
    // a minimum hold period to avoid sub-second flapping caused by HTTP request
    // arrival racing with this check within the same loop iteration.
    constexpr unsigned long WIFI_PRIORITY_ENABLE_TIMEOUT_MS = 3500;
    constexpr unsigned long WIFI_PRIORITY_DISABLE_TIMEOUT_MS = 8000;
    constexpr unsigned long WIFI_PRIORITY_MIN_HOLD_MS = 5000;
    static unsigned long wifiPriorityLastTransitionMs = 0;
    const bool wifiPriorityAllowed = bleClient.isConnected();
    const bool wifiPriorityCurrent = bleClient.isWifiPriority();
    const unsigned long uiTimeoutMs = wifiPriorityCurrent ? WIFI_PRIORITY_DISABLE_TIMEOUT_MS
                                                          : WIFI_PRIORITY_ENABLE_TIMEOUT_MS;
    const bool uiActive = wifiManager.isUiActive(uiTimeoutMs);
    // OBD BLE-critical suppression only matters when WiFi AP is actually on.
    // When WiFi is off, the OBD handler already stops the V1 scan before
    // connecting (connectToDevice) — piggybacking on WiFi priority mode is
    // redundant and causes harmful flapping (stop/restart proxy advertising
    // on every OBD retry cycle, confusing log spam about "WiFi priority"
    // when WiFi is completely off).
    const bool wifiApOn = wifiManager.isSetupModeActive();
    const OBDState obdState = obdHandler.getState();
    const bool obdBleCritical =
        wifiApOn &&
        obdServiceEnabled &&
        (obdHandler.isScanActive() ||
         obdState == OBDState::CONNECTING ||
         obdState == OBDState::INITIALIZING);
    // Keep BLE background suppression active through OBD scan/connect/init so
    // proxy advertising or scan resumes do not interrupt OBD pairing flow
    // (only relevant when WiFi AP is on and could cause radio contention).
    const bool wifiPriority = wifiPriorityAllowed && (uiActive || obdBleCritical);
    const bool holdActive = (now - wifiPriorityLastTransitionMs) < WIFI_PRIORITY_MIN_HOLD_MS;
    if (wifiPriority != wifiPriorityCurrent && !holdActive) {
        bleClient.setWifiPriority(wifiPriority);
        wifiPriorityLastTransitionMs = now;
    }
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
    const AutoPushSlot& slot = settingsManager.getSlot(activeSlotIndex);
    SerialLog.printf("[AutoPush] onV1Connected autoPush=%s slot=%d profile='%s' mode=%d\n",
                     s.autoPushEnabled ? "on" : "off",
                     activeSlotIndex,
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

    // Use global activeSlot
    AUTO_PUSH_LOGF("[AutoPush] Using global activeSlot: %d\n", activeSlotIndex);

    autoPushModule.start(activeSlotIndex);
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

#if defined(PIN_POWER_ON) && PIN_POWER_ON >= 0
    // Cut panel power until we intentionally bring it up
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, LOW);
#endif

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
    
    // DEBUG: Simulate battery for testing UI (uncomment to test)
    // batteryManager.simulateBattery(3800);  // 60% battery
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
        audio_init_sd();  // Initialize SD-based frequency voice audio

        // Retry settings restore now that SD is mounted
        // (settings.begin() runs before storage, so restore may have failed)
        if (settingsManager.checkAndRestoreFromSD()) {
            // Settings were restored from SD - update display with restored brightness
            display.setBrightness(settingsManager.get().brightness);
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
        .isWifiSetupActive = [] { return wifiManager.isSetupModeActive(); },
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
                    AlertData priority = parser.getPriorityAlert();
                    const auto& alerts = parser.getAllAlerts();
                    display.update(priority, alerts.data(), parser.getAlertCount(), state);
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
    connectionStateModule.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule, &systemEventBus);
    displayRestoreModule.begin(&display, &parser, &bleClient, &displayPreviewModule);
    obdHandler.setLinkReadyCallback([]() { return bleClient.isConnected(); });
    obdHandler.setStartScanCallback([]() { bleClient.startOBDScan(); });
    obdHandler.setVwDataEnabled(settingsManager.get().obdVwDataEnabled);
    obdHandler.begin();
    gpsRuntimeModule.begin(settingsManager.get().gpsEnabled);
    speedSourceSelector.begin(settingsManager.get().gpsEnabled);
    cameraRuntimeModule.begin(settingsManager.get().cameraEnabled);
    // Wire lockout store only if not already done during zone-load above.
    // Calling begin() again would reset the dirty flag set by legacy migration.
    if (!lockoutStore.isInitialized()) {
        lockoutStore.begin(&lockoutIndex);
    }
    lockoutEnforcer.begin(&settingsManager, &lockoutIndex, &lockoutStore);
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
    static constexpr unsigned long AUDIO_TICK_MAX_US = 25000;
    static constexpr unsigned long OVERLOAD_LOOP_US = 25000;
    static constexpr unsigned long FREQ_UI_MAX_MS = 75;
    static constexpr unsigned long FREQ_UI_PREVIEW_MAX_MS = 250;
    static constexpr unsigned long CARD_UI_MAX_MS = 100;
    static unsigned long lastAudioTickUs = 0;
    static unsigned long lastFreqUiMs = 0;
    static unsigned long lastCardUiMs = 0;
    static unsigned long lastLoopUs = 0;
    unsigned long now = millis();

    if (bootSplashHoldActive && now >= bootSplashHoldUntilMs) {
        bootSplashHoldActive = false;
        if (!bleClient.isConnected()) {
            showInitialScanningScreen();
        } else {
            initialScanningScreenShown = true;
        }
    }

    bool bleConnectedNow = bleClient.isConnected();

    unsigned long audioTickUs = micros();
    unsigned long sinceAudioUs = audioTickUs - lastAudioTickUs;
    lastAudioTickUs = audioTickUs;
    bool skipNonCoreThisLoop = sinceAudioUs > AUDIO_TICK_MAX_US;
    bool overloadThisLoop = (lastLoopUs >= OVERLOAD_LOOP_US) || skipNonCoreThisLoop;

    // RUN_START marker: fires once when boot phase is complete
    // Helps analyzers distinguish flash/boot noise from runtime behavior
    static bool runStartLogged = false;
    if (!runStartLogged) {
        // Fire when: BLE connected OR 30 seconds elapsed (whichever first)
        bool bleReady = bleClient.isConnected();
        bool timeReady = (now >= 30000);
        if (bleReady || timeReady) {
            runStartLogged = true;
            const char* trigger = bleReady ? "ble_connected" : "timeout_30s";
            SerialLog.printf("RUN_START trigger=%s millis=%lu\n", trigger, now);
        }
    }

    if (!overloadThisLoop) {
        // Update BLE indicator: show when V1 is connected; color reflects JBV1 connection
        // Third param is "receiving" - true if we got V1 packets in last 2s (heartbeat visual)
        if (!bootSplashHoldActive) {
            unsigned long lastRx = bleQueueModule.getLastRxMillis();
            bool bleReceiving = (now - lastRx) < 2000;
            display.setBLEProxyStatus(bleClient.isConnected(), bleClient.isProxyClientConnected(), bleReceiving);
        }
    }
    
    // Drive color preview (band cycle) first; skip other updates if active
    if (!overloadThisLoop && !bootSplashHoldActive) {
        if (displayPreviewModule.isRunning()) {
            displayPreviewModule.update();
        } else {
            // Check if preview/test ended and restore display if needed
            displayRestoreModule.process();
        }
    }

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
    updateBleWifiPriority(now, obdServiceEnabled);
    
    // Process BLE events (includes blocking connect/discovery during reconnect)
    uint32_t bleProcessStartUs = PERF_TIMESTAMP_US();
    bleClient.process();
    perfRecordBleProcessUs(PERF_TIMESTAMP_US() - bleProcessStartUs);
#endif
    
    // Process queued BLE data (safe for SPI - runs in main loop context)
    uint32_t bleDrainStartUs = PERF_TIMESTAMP_US();
    bleQueueModule.process();
    perfRecordBleDrainUs(PERF_TIMESTAMP_US() - bleDrainStartUs);

    static bool obdRuntimeDisabledLatched = false;
    if (!obdServiceEnabled) {
        obdAutoConnectPending = false;
        if (!obdRuntimeDisabledLatched) {
            obdHandler.stopScan();
            obdHandler.disconnect();
            obdRuntimeDisabledLatched = true;
        }
    } else {
        obdRuntimeDisabledLatched = false;
        if (obdAutoConnectPending && now >= obdAutoConnectAtMs) {
            obdAutoConnectPending = false;
            obdHandler.tryAutoConnect();
        }

        uint32_t obdStartUs = PERF_TIMESTAMP_US();
        if (obdHandler.update()) {
            OBDData obdData = obdHandler.getData();
            speedSourceSelector.updateObdSample(obdData.speed_mph, obdData.timestamp_ms, obdData.valid);
        }
        perfRecordObdUs(PERF_TIMESTAMP_US() - obdStartUs);
    }
    speedSourceSelector.setObdConnected(obdServiceEnabled && obdHandler.isConnected());

    uint32_t gpsStartUs = PERF_TIMESTAMP_US();
    gpsRuntimeModule.update(now);
    perfRecordGpsUs(PERF_TIMESTAMP_US() - gpsStartUs);

    SpeedSelection speedSelection;
    if (speedSourceSelector.select(now, speedSelection)) {
        voiceModule.updateSpeedSample(speedSelection.speedMph, speedSelection.timestampMs);
    } else {
        voiceModule.clearSpeedSample();
    }
    
    // Drain only parsed-frame events; keep non-frame events available for
    // dedicated consumers (OBD/GPS/connection modules) as they are added.
    // Fallback parsed flag preserves behavior if the bus drops under load.
    bool parsedReady = bleQueueModule.consumeParsedFlag();
    uint32_t parsedTsMs = bleQueueModule.getLastParsedTimestamp();
    SystemEvent event;
    while (systemEventBus.consumeByType(SystemEventType::BLE_FRAME_PARSED, event)) {
        parsedReady = true;
        if (event.tsMs != 0) {
            parsedTsMs = event.tsMs;
        }
    }

    // Drive display pipeline separately from BLE drain (decoupled for accurate timing)
    // This is intentionally outside the bleDrain timing to isolate display latency
    if (parsedReady && !bootSplashHoldActive) {
        const uint32_t nowMs = millis();
        const GpsRuntimeStatus gpsStatus = gpsRuntimeModule.snapshot(nowMs);
        const bool proxyClientConnected = bleClient.isProxyClientConnected();
        bool lockoutPrioritySuppressed = false;
        static LockoutRuntimeMuteState lockoutMuteState;
        static PreQuietState preQuietState;

        uint32_t lockoutStartUs = PERF_TIMESTAMP_US();
        signalCaptureModule.capturePriorityObservation(
            nowMs,
            parser,
            gpsStatus,
            loopSettings.enableSignalTraceLogging);
        if (!proxyClientConnected) {
            lockoutEnforcer.process(nowMs, timeService.nowEpochMsOr0(), parser, gpsStatus);

            // Feed lockout decision into display indicator before rendering.
            const auto& lockRes = lockoutEnforcer.lastResult();
            display.setLockoutIndicator(lockRes.evaluated && lockRes.shouldMute);

            // ENFORCE mute execution: send mute to V1 when lockout decides to suppress.
            // Rate-limited: only send once per lockout-match cycle (not every frame).
            const V1Settings& lockoutSettings = settingsManager.get();
            const GpsLockoutCoreGuardStatus lockoutGuard = gpsLockoutEvaluateCoreGuard(
                lockoutSettings.gpsLockoutCoreGuardEnabled,
                lockoutSettings.gpsLockoutMaxQueueDrops,
                lockoutSettings.gpsLockoutMaxPerfDrops,
                lockoutSettings.gpsLockoutMaxEventBusDrops,
                perfCounters.queueDrops.load(),
                perfCounters.perfDrop.load(),
                systemEventBus.getDropCount());

            const bool enforceMode =
                lockRes.mode == static_cast<uint8_t>(LOCKOUT_RUNTIME_ENFORCE);
            const bool enforceAllowed = enforceMode && !lockoutGuard.tripped;

            // Suppress local priority announcements in ENFORCE lockout matches.
            lockoutPrioritySuppressed =
                lockRes.evaluated && lockRes.shouldMute && enforceAllowed;

            const LockoutRuntimeMuteDecision muteDecision =
                evaluateLockoutRuntimeMute(lockRes, lockoutGuard, bleClient.isConnected(), lockoutMuteState);

            if (muteDecision.sendMute) {
                bleClient.setMute(true);
                Serial.println("[Lockout] ENFORCE: mute sent to V1");
            }
            if (muteDecision.logGuardBlocked) {
                Serial.printf("[Lockout] ENFORCE blocked by core guard (%s)\n", lockoutGuard.reason);
            }

            // Pre-quiet: proactively drop volume when GPS is in a lockout zone.
            // findNearby is position-only, O(N) ~50 μs — same scan the enforcer uses.
            {
                const DisplayState& pqState = parser.getDisplayState();
                size_t nearbyCount = 0;
                if (gpsStatus.locationValid &&
                    std::isfinite(gpsStatus.latitudeDeg) &&
                    std::isfinite(gpsStatus.longitudeDeg)) {
                    const int32_t latE5 = static_cast<int32_t>(lroundf(gpsStatus.latitudeDeg * 100000.0f));
                    const int32_t lonE5 = static_cast<int32_t>(lroundf(gpsStatus.longitudeDeg * 100000.0f));
                    int16_t nearbyBuf[16];
                    nearbyCount = lockoutIndex.findNearby(latE5, lonE5, nearbyBuf, 16);
                }
                const PreQuietDecision pqDecision = evaluatePreQuiet(
                    lockoutSettings.gpsLockoutPreQuiet,
                    enforceMode,
                    bleClient.isConnected(),
                    gpsStatus.locationValid,
                    parser.hasAlerts(),
                    lockRes.evaluated,
                    lockRes.shouldMute,
                    nearbyCount,
                    pqState.mainVolume,
                    pqState.muteVolume,
                    nowMs,
                    preQuietState);
                if (pqDecision.action == PreQuietDecision::DROP_VOLUME) {
                    bleClient.setVolume(pqDecision.volume, pqDecision.muteVolume);
                    Serial.println("[Lockout] PRE-QUIET: volume dropped in lockout zone");
                } else if (pqDecision.action == PreQuietDecision::RESTORE_VOLUME) {
                    bleClient.setVolume(pqDecision.volume, pqDecision.muteVolume);
                    // Tell VolumeFade the real baseline so it doesn't capture stale echo.
                    volumeFadeModule.setBaselineHint(pqDecision.volume, pqDecision.muteVolume, nowMs);
                    Serial.println("[Lockout] PRE-QUIET: volume restored");
                }
                display.setPreQuietActive(preQuietState.phase == PreQuietPhase::DROPPED);
            }
        } else {
            // Proxy-connected sessions are display-first:
            // keep learner capture active, but disable runtime lockout enforcement.
            display.setLockoutIndicator(false);
            display.setPreQuietActive(false);
            lockoutMuteState = LockoutRuntimeMuteState{};
            preQuietState = PreQuietState{};
        }
        perfRecordLockoutUs(PERF_TIMESTAMP_US() - lockoutStartUs);

        // Feed GPS satellite count into display indicator (throttled to ~90 s).
        {
            static uint32_t lastGpsSatUpdateMs = 0;
            constexpr uint32_t GPS_SAT_UPDATE_INTERVAL_MS = 90000;  // 90 seconds
            if (lastGpsSatUpdateMs == 0 || (nowMs - lastGpsSatUpdateMs >= GPS_SAT_UPDATE_INTERVAL_MS)) {
                display.setGpsSatellites(gpsStatus.enabled, gpsStatus.hasFix, gpsStatus.satellites);
                lastGpsSatUpdateMs = nowMs;
            }
        }

        // Feed OBD connection state into display indicator.
        {
            const bool obdEnabled = settingsManager.get().obdEnabled;
            display.setObdConnected(obdEnabled, obdHandler.isConnected(), obdHandler.hasValidData());
        }

        // Skip display pipeline if preview is running (don't overwrite demo)
        if (!displayPreviewModule.isRunning()) {
            if (parsedTsMs != 0 && nowMs >= parsedTsMs) {
                perfRecordNotifyToDisplayMs(nowMs - parsedTsMs);
            }
            // No overload guard: handleParsed's internal 25ms throttle gates
            // expensive draws; fade/debounce/gap-recovery are microsecond-cheap
            // and must run every frame (BLE volume restore is tier-2 priority).
            uint32_t dispPipeStartUs = PERF_TIMESTAMP_US();
            displayPipelineModule.handleParsed(nowMs, lockoutPrioritySuppressed);
            perfRecordDispPipeUs(PERF_TIMESTAMP_US() - dispPipeStartUs);
            lastFreqUiMs = nowMs;
        }
    } else if (!bootSplashHoldActive) {
        // Clear stale lockout badge if parser traffic has stopped or link is down.
        // Avoids showing a latched "L" when we no longer have fresh lockout decisions.
        static constexpr uint32_t LOCKOUT_INDICATOR_STALE_MS = 2000;
        const uint32_t nowMs = millis();
        const uint32_t lastParsedMs = bleQueueModule.getLastParsedTimestamp();
        if (!bleClient.isConnected() ||
            lastParsedMs == 0 ||
            static_cast<uint32_t>(nowMs - lastParsedMs) > LOCKOUT_INDICATOR_STALE_MS) {
            display.setLockoutIndicator(false);
        }
    }

    bool previewRunning = displayPreviewModule.isRunning();
    unsigned long freqUiMaxMs = previewRunning ? FREQ_UI_PREVIEW_MAX_MS : FREQ_UI_MAX_MS;
    if (!bootSplashHoldActive && bleClient.isConnected() && (now - lastFreqUiMs) >= freqUiMaxMs) {
        const DisplayState& state = parser.getDisplayState();
        const AlertData priority = parser.getPriorityAlert();
        bool hasPriority = parser.hasAlerts() && priority.isValid && priority.band != BAND_NONE;
        bool isPhotoRadar = (state.bogeyCounterChar == 'P');
        if (hasPriority) {
            display.refreshFrequencyOnly(priority.frequency, priority.band, state.muted, isPhotoRadar);
        } else {
            display.refreshFrequencyOnly(0, BAND_NONE, false, false);
        }
        lastFreqUiMs = now;
    }

    if (!bootSplashHoldActive &&
        bleClient.isConnected() &&
        !displayPreviewModule.isRunning() &&
        overloadThisLoop &&
        (now - lastCardUiMs) >= CARD_UI_MAX_MS) {
        const auto& allAlerts = parser.getAllAlerts();
        int alertCount = static_cast<int>(parser.getAlertCount());
        const AlertData priority = parser.getPriorityAlert();
        const DisplayState& state = parser.getDisplayState();
        display.refreshSecondaryAlertCards(allAlerts.data(), alertCount, priority, state.muted);
        lastCardUiMs = now;
    }

    // Drive auto-push state machine (non-blocking)
    autoPushModule.process();

    // Camera runtime is strictly low-priority and self-gated on overload/non-core.
    // Only a real priority V1 signal preempts camera lifecycle — weak/background
    // alerts (BSM, door openers, etc.) must not suppress camera matching.
    uint32_t cameraStartUs = PERF_TIMESTAMP_US();
    {
        const AlertData camPriority = parser.getPriorityAlert();
        const bool signalPriorityActive = parser.hasAlerts() &&
                                          camPriority.isValid &&
                                          camPriority.band != BAND_NONE;
        cameraRuntimeModule.process(now, skipNonCoreThisLoop, overloadThisLoop, signalPriorityActive);
    }
    perfRecordCameraUs(PERF_TIMESTAMP_US() - cameraStartUs);

    // ── WiFi deferred auto-start gate ────────────────────────────────
    // Replaces the old setup()-time startWifi() call.  WiFi waits for
    // BLE to connect + settle, or a 30 s boot timeout, whichever first.
    if (!wifiAutoStartDone && loopSettings.enableWifiAtBoot) {
        constexpr uint32_t WIFI_SETTLE_MS  = 3000;
        constexpr uint32_t WIFI_BOOT_TIMEOUT_MS = 30000;
        const uint32_t msSinceV1 = (v1ConnectedAtMs > 0) ? (now - v1ConnectedAtMs) : 0;
        const bool canDma = wifiManager.canStartSetupMode(nullptr, nullptr);
        if (WifiBootPolicy::shouldAutoStartWifi(
                true, false, bleClient.isConnected(),
                msSinceV1, WIFI_SETTLE_MS,
                now, WIFI_BOOT_TIMEOUT_MS, canDma)) {
            SerialLog.printf("[WiFi] Deferred auto-start at %lu ms (v1Connect=%lu ms ago)\n",
                             now, msSinceV1);
            getWifiOrchestrator().startWifi();
            wifiAutoStartDone = true;
        }
    }

    // Process WiFi/web server only when WiFi is actually enabled.
    // Explicit gate keeps this path a near-no-op in wifi-off profiles.
    if (!skipNonCoreThisLoop && isWifiProcessingEnabled(loopSettings)) {
        uint32_t wifiStartUs = PERF_TIMESTAMP_US();
        wifiManager.process();
        perfRecordWifiProcessUs(PERF_TIMESTAMP_US() - wifiStartUs);
    }
    
    perfRecordLoopJitterUs(micros() - loopStartUs);
    StorageManager::updateDmaHeapCache();  // Keep DMA cache fresh for SD gating
    perfRecordHeapStats(ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                        StorageManager::getCachedFreeDma(), StorageManager::getCachedLargestDma());
    
    // Speed-based volume: delegate to module (rate-limited internally)
    speedVolumeModule.process(now);
    
    // Low-speed speaker volume scaling: when quiet is active, reduce speaker
    // proportionally; restore when quiet ends.
    {
        static bool speakerQuietActive = false;
        static uint8_t speakerOriginalVolume = 0;
        bool quietNow = speedVolumeModule.isQuietActive();
        if (quietNow && !speakerQuietActive) {
            // Entering low-speed quiet — scale speaker volume
            speakerOriginalVolume = settingsManager.get().voiceVolume;
            uint8_t qv = speedVolumeModule.getQuietVolume();
            uint8_t scaled = (qv == 0) ? 0
                : static_cast<uint8_t>((uint16_t)speakerOriginalVolume * qv / 9);
            audio_set_volume(scaled);
            speakerQuietActive = true;
        } else if (!quietNow && speakerQuietActive) {
            // Exiting low-speed quiet — restore speaker volume
            audio_set_volume(settingsManager.get().voiceVolume);
            speakerQuietActive = false;
        }
    }
    
    // Update display periodically
    now = millis();
    bleConnectedNow = bleClient.isConnected();
    if (lastBleConnectedForScanDwell && !bleConnectedNow && !bootSplashHoldActive) {
        scanScreenEnteredMs = now;
        scanScreenDwellActive = true;
    }
    lastBleConnectedForScanDwell = bleConnectedNow;
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
        lastDisplayUpdate = now;
        if (!displayPreviewModule.isRunning()) {
            // Handle connection state transitions (connect/disconnect, stale data re-request)
            if (!bootSplashHoldActive) {
                bool holdScanDwell = false;
                if (scanScreenDwellActive && bleConnectedNow) {
                    unsigned long scanDwellMs = now - scanScreenEnteredMs;
                    holdScanDwell = scanDwellMs < activeScanScreenDwellMs;
                }

                if (!holdScanDwell) {
                    connectionStateModule.process(now);
                    if (bleConnectedNow) {
                        scanScreenDwellActive = false;
                    }
                }
            }
        }
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

    // Short FreeRTOS delay to yield CPU without capping loop at ~200 Hz
    vTaskDelay(pdMS_TO_TICKS(1));
    lastLoopUs = micros() - loopStartUs;
}
