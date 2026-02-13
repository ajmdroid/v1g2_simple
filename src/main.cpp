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
 * - Alert logging and replay
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
#include "esp_core_dump.h"
#include "modules/voice/voice_module.h"
#include "modules/speed_volume/speed_volume_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/perf/debug_macros.h"
#include "time_service.h"
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <vector>
#include <algorithm>
#include <cstring>

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
static unsigned long scanScreenEnteredMs = 0;
static bool scanScreenDwellActive = false;
static bool lastBleConnectedForScanDwell = false;
static bool obdAutoConnectPending = false;
static unsigned long obdAutoConnectAtMs = 0;

// Color preview driver (demo band cycle)
DisplayPreviewModule displayPreviewModule;

void requestColorPreviewHold(uint32_t durationMs) {
    displayPreviewModule.requestHold(durationMs);
}

bool isColorPreviewRunning() {
    return displayPreviewModule.isRunning();
}

void cancelColorPreview() {
    displayPreviewModule.cancel();
}

static const char* resetReasonToString(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "POWERON";
        case ESP_RST_SW: return "SW";
        case ESP_RST_PANIC: return "PANIC";
        case ESP_RST_INT_WDT: return "WDT_INT";
        case ESP_RST_TASK_WDT: return "WDT_TASK";
        case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_SDIO: return "SDIO";
        default: return "UNKNOWN";
    }
}

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

static uint32_t buildLogCategoryBitmap(const DebugLogConfig& cfg) {
    uint32_t mask = 0;
    if (cfg.alerts) mask |= (1u << 0);
    if (cfg.wifi) mask |= (1u << 1);
    if (cfg.ble) mask |= (1u << 2);
    if (cfg.system) mask |= (1u << 3);
    if (cfg.display) mask |= (1u << 4);
    if (cfg.perfMetrics) mask |= (1u << 5);
    if (cfg.audio) mask |= (1u << 6);
    if (cfg.touch) mask |= (1u << 7);
    return mask;
}

// ============================================================================
// PANIC BREADCRUMBS: Log heap stats + coredump info on crash recovery
// ============================================================================
static void logPanicBreadcrumbs() {
    esp_reset_reason_t reason = esp_reset_reason();
    bool isCrash = (reason == ESP_RST_PANIC || 
                    reason == ESP_RST_INT_WDT || 
                    reason == ESP_RST_TASK_WDT || 
                    reason == ESP_RST_WDT);
    
    if (!isCrash) return;
    
    Serial.println("\n!!! CRASH RECOVERY DETECTED !!!");
    Serial.printf("Reset reason: %s\n", resetReasonToString(reason));
    
    // Log current heap stats (post-crash, but helpful for baseline)
    uint32_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    uint32_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    uint32_t minFreeHeap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    Serial.printf("Heap now: free=%lu, largest=%lu, minEver=%lu\n",
                  (unsigned long)freeHeap, (unsigned long)largestBlock, (unsigned long)minFreeHeap);
    
    // Check for coredump
    esp_core_dump_summary_t summary;
    esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err == ESP_OK) {
        Serial.println("Coredump found:");
        Serial.printf("  Crashed task: %s\n", summary.exc_task);
        Serial.printf("  Exception cause: %lu\n", (unsigned long)summary.ex_info.exc_cause);
        Serial.printf("  Exception PC: 0x%08lx\n", (unsigned long)summary.exc_pc);
        
        // Print backtrace if available
        if (summary.exc_bt_info.depth > 0) {
            Serial.print("  Backtrace: ");
            for (int i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
                Serial.printf("0x%08lx ", (unsigned long)summary.exc_bt_info.bt[i]);
            }
            Serial.println();
        }
    } else {
        Serial.printf("No coredump available (err=%d) - check serial log for backtrace\n", err);
    }
    
    Serial.println("!!! END CRASH RECOVERY INFO !!!\n");
    
    // Best-effort: Try to write panic.txt to LittleFS (SD not mounted yet)
    // This runs BEFORE storage init, so we use LittleFS directly
    if (LittleFS.begin(true)) {  // true = format if mount fails
        File f = LittleFS.open("/panic.txt", FILE_WRITE);
        if (f) {
            f.printf("CRASH at boot (millis=%lu)\n", millis());
            f.printf("Reset reason: %s\n", resetReasonToString(reason));
            f.printf("Heap: free=%lu, largest=%lu, minEver=%lu\n",
                     (unsigned long)freeHeap, (unsigned long)largestBlock, (unsigned long)minFreeHeap);
            
            if (err == ESP_OK) {
                f.printf("Task: %s\n", summary.exc_task);
                f.printf("PC: 0x%08lx\n", (unsigned long)summary.exc_pc);
                if (summary.exc_bt_info.depth > 0) {
                    f.print("BT: ");
                    for (int i = 0; i < summary.exc_bt_info.depth && i < 16; i++) {
                        f.printf("0x%08lx ", (unsigned long)summary.exc_bt_info.bt[i]);
                    }
                    f.println();
                }
            }
            f.close();
            Serial.println("[PANIC] Wrote /panic.txt to LittleFS");
        }
        // Don't end LittleFS - let storage manager handle it
    }
}

// Log NVS statistics and perform cleanup if needed
static void nvsHealthCheck() {
    nvs_stats_t stats;
    if (nvs_get_stats(NULL, &stats) == ESP_OK) {
        uint32_t usedPct = (stats.used_entries * 100) / stats.total_entries;
        Serial.printf("[NVS] Entries: %lu/%lu used (%lu%%), namespaces: %lu, free: %lu\n",
                      (unsigned long)stats.used_entries, 
                      (unsigned long)stats.total_entries,
                      (unsigned long)usedPct,
                      (unsigned long)stats.namespace_count,
                      (unsigned long)stats.free_entries);
        
        // If NVS is >80% full, attempt recovery
        if (usedPct > 80) {
            Serial.println("[NVS] WARNING: NVS >80% full, clearing stale data...");
            
            // Clear legacy settings namespace if it exists
            Preferences legacy;
            if (legacy.begin("v1settings", false)) {
                legacy.clear();
                legacy.end();
                Serial.println("[NVS] Cleared legacy v1settings namespace");
            }
            
            // Clear inactive settings namespace
            Preferences meta;
            String activeNs = "";
            if (meta.begin("v1settingsMeta", true)) {
                activeNs = meta.getString("active", "");
                meta.end();
            }
            
            const char* inactiveNs = nullptr;
            if (activeNs == "v1settingsA") {
                inactiveNs = "v1settingsB";
            } else if (activeNs == "v1settingsB") {
                inactiveNs = "v1settingsA";
            }
            
            if (inactiveNs) {
                Preferences inactive;
                if (inactive.begin(inactiveNs, false)) {
                    inactive.clear();
                    inactive.end();
                    Serial.printf("[NVS] Cleared inactive namespace %s\n", inactiveNs);
                }
            }
            
            // Log stats after cleanup
            if (nvs_get_stats(NULL, &stats) == ESP_OK) {
                usedPct = (stats.used_entries * 100) / stats.total_entries;
                Serial.printf("[NVS] After cleanup: %lu/%lu used (%lu%%)\n",
                              (unsigned long)stats.used_entries, 
                              (unsigned long)stats.total_entries,
                              (unsigned long)usedPct);
            }
        }
    } else {
        Serial.println("[NVS] WARNING: Could not get NVS stats");
    }
}

static uint32_t nextBootId() {
    Preferences prefs;
    if (!prefs.begin("v1boot", false)) {
        return 0;
    }
    uint32_t bootId = prefs.getUInt("bootId", 0) + 1;
    prefs.putUInt("bootId", bootId);
    prefs.end();
    return bootId;
}

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

// Callback when V1 connection is fully established
// Handles auto-push of default profile and mode
void onV1Connected() {
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
    // This runs regardless of autoPush — V1 connecting is the "car on" signal.
    obdAutoConnectPending = true;
    obdAutoConnectAtMs = millis() + 1500;
    
    if (!s.autoPushEnabled) {
        AUTO_PUSH_LOGLN("[AutoPush] Disabled, skipping");
        return;
    }

    // Use global activeSlot
    AUTO_PUSH_LOGF("[AutoPush] Using global activeSlot: %d\n", activeSlotIndex);

    autoPushModule.start(activeSlotIndex);
}

// Helper for fatal boot errors - shows message, waits, then restarts
// displayAvailable: true if display.begin() succeeded and we can show on-screen error
static void fatalBootError(const char* message, bool displayAvailable) {
    SerialLog.printf("FATAL: %s\n", message);
    
    if (displayAvailable) {
        // Show error on screen with countdown
        display.showDisconnected();  // Clear screen with base frame
        // Draw error message (red text, centered)
        // Note: Using drawStatusText-like approach
        SerialLog.println("Showing error on display, will restart in 10 seconds...");
        
        // Simple countdown - show message and wait
        for (int i = 10; i > 0; i--) {
            SerialLog.printf("Restarting in %d...\n", i);
            delay(1000);
        }
    } else {
        // No display - just wait and restart
        SerialLog.println("Display unavailable. Restarting in 10 seconds...");
        delay(10000);
    }
    
    SerialLog.println("Restarting...");
    ESP.restart();
}


void setup() {
    const unsigned long setupStartMs = millis();
    unsigned long setupStageStartMs = setupStartMs;

    // Wait for USB to stabilize after upload
    delay(50);
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
    } else {
        SerialLog.printf("(Other: %d)\n", resetReason);
    }
    SerialLog.println("===================================\n");

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
    delay(10);
    logBootStage("display");

    // Initialize settings BEFORE showing any styled screens (need displayStyle setting)
    settingsManager.begin();
    timeService.begin();

#if defined(DISPLAY_WAVESHARE_349)
    powerModule.begin(&batteryManager, &display, &settingsManager, &debugLogger);
    powerModule.logStartupStatus();
#endif
    logBootStage("settings");

    // Show boot splash only on true power-on (not crash reboots or firmware uploads)
    if (resetReason == ESP_RST_POWERON) {
        // True cold boot: brief non-blocking splash for immediate visual confirmation
        display.showBootSplash();
        bootSplashHoldActive = true;
        bootSplashHoldUntilMs = millis() + BOOT_SPLASH_HOLD_MS;
    } else {
        showInitialScanningScreen();
    }
    logBootStage("boot_ui");

    // Initialize display preview driver
    displayPreviewModule.begin(&display);
    
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

    // Standalone perf CSV logger (SD only).
    perfSdLogger.begin(storageManager.isReady() && storageManager.isSDCard());
    if (perfSdLogger.isEnabled()) {
        SerialLog.printf("[Perf] SD logger enabled (%s)\n", perfSdLogger.csvPath());
    } else {
        SerialLog.println("[Perf] SD logger disabled (no SD)");
    }
    logBootStage("storage");

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

    DebugLogConfig bootCfg = settingsManager.getDebugLogConfig();
    uint32_t logMask = buildLogCategoryBitmap(bootCfg);
    const V1Settings& bootSettings = settingsManager.get();
    const char* scenario = "default";
#ifdef GIT_SHA
    const char* gitSha = GIT_SHA;
#else
    const char* gitSha = "unknown";
#endif
    const char* resetStr = resetReasonToString(resetReason);
    SerialLog.printf("BOOT bootId=%lu reset=%s git=%s scenario=%s wifi=%s logCats=0x%08lX\n",
                    (unsigned long)bootId,
                    resetStr,
                    gitSha,
                    scenario,
                    bootSettings.enableWifi ? "on" : "off",
                    (unsigned long)logMask);

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
    bootReady = true;
    bleClient.setBootReady(true);
    SerialLog.printf("[Boot] Ready gate opened at %lu ms\n", millis());
    logBootStage("core_pipeline");

#ifndef REPLAY_MODE
    // Initialize BLE client with proxy settings from preferences
    const V1Settings& bleSettings = settingsManager.get();
    SerialLog.printf("Starting BLE (proxy: %s, name: %s)\n", 
                  bleSettings.proxyBLE ? "enabled" : "disabled",
                  bleSettings.proxyName.c_str());

    // Initialize BLE stack first (required before any BLE operations)
    if (!bleClient.initBLE(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
        SerialLog.println("BLE initialization failed!");
        fatalBootError("BLE init failed", true);
    }
    
    // Start normal scanning
    SerialLog.println("Starting BLE scan for V1...");
    if (!bleClient.begin(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
        SerialLog.println("BLE scan failed to start!");
        fatalBootError("BLE scan failed", true);
    }
    
    // Register data callback
    bleClient.onDataReceived(onV1Data);
    
    // Register V1 connection callback for auto-push
    bleClient.onV1Connected(onV1Connected);
    logBootStage("ble_start");
#else
    SerialLog.println("[REPLAY_MODE] BLE disabled - using packet replay for UI testing");
#endif
    
    // Auto-start WiFi if enabled in dev settings
    if (settingsManager.get().enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled - starting AP now...");
        getWifiOrchestrator().startWifi();
        SerialLog.println("Setup complete - BLE scanning, WiFi auto-started");
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
    static constexpr unsigned long FREQ_UI_MAX_MS = 100;
    static constexpr unsigned long FREQ_UI_PREVIEW_MAX_MS = 250;
    static constexpr unsigned long CARD_UI_MAX_MS = 150;
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

    // WiFi priority mode: web UI can deprioritize BLE background work, but never
    // at the expense of establishing/maintaining V1 connectivity.
    // Use hysteresis so a delayed poll does not immediately flap ENABLED/DISABLED.
    constexpr unsigned long WIFI_PRIORITY_ENABLE_TIMEOUT_MS = 3500;
    constexpr unsigned long WIFI_PRIORITY_DISABLE_TIMEOUT_MS = 8000;
    const bool wifiPriorityAllowed = bleClient.isConnected();
    const bool wifiPriorityCurrent = bleClient.isWifiPriority();
    const unsigned long uiTimeoutMs = wifiPriorityCurrent ? WIFI_PRIORITY_DISABLE_TIMEOUT_MS
                                                          : WIFI_PRIORITY_ENABLE_TIMEOUT_MS;
    const bool uiActive = wifiManager.isUiActive(uiTimeoutMs);
    const OBDState obdState = obdHandler.getState();
    const bool obdBleCritical =
        obdHandler.isScanActive() ||
        obdState == OBDState::CONNECTING ||
        obdState == OBDState::INITIALIZING;
    // Keep BLE background suppression active through OBD scan/connect/init so
    // proxy advertising or scan resumes do not interrupt OBD pairing flow.
    const bool wifiPriority = wifiPriorityAllowed && (uiActive || obdBleCritical);
    if (wifiPriority != wifiPriorityCurrent) {
        bleClient.setWifiPriority(wifiPriority);
    }
    
    // Process BLE events (includes blocking connect/discovery during reconnect)
    uint32_t bleProcessStartUs = PERF_TIMESTAMP_US();
    bleClient.process();
    perfRecordBleProcessUs(PERF_TIMESTAMP_US() - bleProcessStartUs);
#endif
    
    // Process queued BLE data (safe for SPI - runs in main loop context)
    uint32_t bleDrainStartUs = PERF_TIMESTAMP_US();
    bleQueueModule.process();
    perfRecordBleDrainUs(PERF_TIMESTAMP_US() - bleDrainStartUs);

    if (obdAutoConnectPending && now >= obdAutoConnectAtMs) {
        obdAutoConnectPending = false;
        obdHandler.tryAutoConnect();
    }

    if (obdHandler.update()) {
        OBDData obdData = obdHandler.getData();
        if (obdData.valid) {
            voiceModule.updateSpeedSample(obdData.speed_mph, obdData.timestamp_ms);
        }
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
        // Skip display pipeline if preview is running (don't overwrite demo)
        if (!displayPreviewModule.isRunning()) {
            uint32_t nowMs = millis();
            if (parsedTsMs != 0 && nowMs >= parsedTsMs) {
                perfRecordNotifyToDisplayMs(nowMs - parsedTsMs);
            }
            if (!overloadThisLoop) {
                uint32_t dispPipeStartUs = PERF_TIMESTAMP_US();
                displayPipelineModule.handleParsed(nowMs);
                perfRecordDispPipeUs(PERF_TIMESTAMP_US() - dispPipeStartUs);
                lastFreqUiMs = nowMs;
            }
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

    if (!skipNonCoreThisLoop) {
        // Process WiFi/web server
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
                    holdScanDwell = scanDwellMs < MIN_SCAN_SCREEN_DWELL_MS;
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
    perfMetricsCheckReport();

    // Short FreeRTOS delay to yield CPU without capping loop at ~200 Hz
    vTaskDelay(pdMS_TO_TICKS(1));
    lastLoopUs = micros() - loopStartUs;
}
