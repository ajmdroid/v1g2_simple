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
#include "modules/speed/speed_source_selector.h"
#include "modules/perf/debug_macros.h"
#include "time_service.h"
#include <driver/gpio.h>
#include <esp_sleep.h>
#include "../include/display_driver.h"
#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <vector>
#include <algorithm>
#include <cstring>
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

static uint32_t normalizeLegacyLockoutRadiusScale(JsonDocument& doc) {
    JsonArray zones = doc["zones"].as<JsonArray>();
    if (zones.isNull()) {
        return 0;
    }

    // Legacy lockout files stored radiusE5 in a 10x scale.
    // Heuristic is safe: legacy valid range started at 450.
    static constexpr uint16_t LEGACY_RADIUS_MIN_E5 = 450;
    uint32_t migrated = 0;
    for (JsonObject zone : zones) {
        if (zone["rad"].isNull()) {
            continue;
        }
        const uint16_t raw = zone["rad"].as<uint16_t>();
        if (raw < LEGACY_RADIUS_MIN_E5) {
            continue;
        }
        const uint16_t normalized = clampLockoutLearnerRadiusE5Value(
            static_cast<int>((raw + 5u) / 10u));
        if (normalized != raw) {
            zone["rad"] = normalized;
            ++migrated;
        }
    }
    return migrated;
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
    if (LittleFS.begin(false)) {  // false = never auto-format during panic logging
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
    // Initialize BLE stack early (no scan yet) once settings/storage are ready.
    const V1Settings& blePreInitSettings = settingsManager.get();
    logBootCheckpoint("ble_preinit_begin");
    const unsigned long blePreInitStartMs = millis();
    if (!bleClient.initBLE(blePreInitSettings.proxyBLE, blePreInitSettings.proxyName.c_str())) {
        SerialLog.println("BLE pre-initialization failed!");
        fatalBootError("BLE pre-init failed", true);
    }
    SerialLog.printf("[BootTiming] ble_preinit_ms=%lu\n", millis() - blePreInitStartMs);
    logBootStage("ble_preinit");

    // Start scan early so discovery overlaps remaining setup work.
    // Connection state-machine work still waits for bootReady gate later in setup().
    bleClient.onDataReceived(onV1Data);
    bleClient.onV1Connected(onV1Connected);
    logBootCheckpoint("ble_callbacks_registered");
    const V1Settings& bleScanSettings = settingsManager.get();
    SerialLog.printf("Starting BLE scan for V1 early (proxy: %s, name: %s)\n",
                     bleScanSettings.proxyBLE ? "enabled" : "disabled",
                     bleScanSettings.proxyName.c_str());
    logBootCheckpoint("ble_scan_begin");
    const unsigned long bleScanStartMs = millis();
    if (!bleClient.begin(bleScanSettings.proxyBLE, bleScanSettings.proxyName.c_str())) {
        SerialLog.println("BLE scan failed to start!");
        fatalBootError("BLE scan failed", true);
    }
    SerialLog.printf("[BootTiming] ble_scan_start_ms=%lu\n", millis() - bleScanStartMs);
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
    cameraRuntimeModule.begin(settingsManager.get().gpsEnabled && settingsManager.get().cameraEnabled);
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
    logBootStage("core_pipeline");

#ifndef REPLAY_MODE
    SerialLog.println("BLE scan already active from early setup path");
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
    const bool obdServiceEnabled = settingsManager.get().obdEnabled;
    const OBDState obdState = obdHandler.getState();
    const bool obdBleCritical =
        obdServiceEnabled &&
        (obdHandler.isScanActive() ||
         obdState == OBDState::CONNECTING ||
         obdState == OBDState::INITIALIZING);
    // Keep BLE background suppression active through OBD scan/connect/init so
    // proxy advertising or scan resumes do not interrupt OBD pairing flow.
    const bool wifiPriority = wifiPriorityAllowed && (uiActive || obdBleCritical);
    const bool holdActive = (now - wifiPriorityLastTransitionMs) < WIFI_PRIORITY_MIN_HOLD_MS;
    if (wifiPriority != wifiPriorityCurrent && !holdActive) {
        bleClient.setWifiPriority(wifiPriority);
        wifiPriorityLastTransitionMs = now;
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
        uint32_t lockoutStartUs = PERF_TIMESTAMP_US();
        signalCaptureModule.capturePriorityObservation(nowMs, parser, gpsStatus);
        lockoutEnforcer.process(nowMs, timeService.nowEpochMsOr0(), parser, gpsStatus);
        perfRecordLockoutUs(PERF_TIMESTAMP_US() - lockoutStartUs);

        // Feed lockout decision into display indicator before rendering.
        const auto& lockRes = lockoutEnforcer.lastResult();
        display.setLockoutIndicator(lockRes.evaluated && lockRes.shouldMute);

        bool lockoutPrioritySuppressed = false;

        // ENFORCE mute execution: send mute to V1 when lockout decides to suppress.
        // Rate-limited: only send once per lockout-match cycle (not every frame).
        {
            static LockoutRuntimeMuteState lockoutMuteState;
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
        }

        // Skip display pipeline if preview is running (don't overwrite demo)
        if (!displayPreviewModule.isRunning()) {
            if (parsedTsMs != 0 && nowMs >= parsedTsMs) {
                perfRecordNotifyToDisplayMs(nowMs - parsedTsMs);
            }
            // No overload guard: handleParsed's internal 30ms throttle gates
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
    // Live V1 alerts always preempt camera lifecycle/rendering.
    uint32_t cameraStartUs = PERF_TIMESTAMP_US();
    cameraRuntimeModule.process(now, skipNonCoreThisLoop, overloadThisLoop, parser.hasAlerts());
    perfRecordCameraUs(PERF_TIMESTAMP_US() - cameraStartUs);

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
    perfMetricsCheckReport();

    // Periodic time persistence (every 5 min) — ensures NVS has a recent epoch
    // for restoration after deep sleep battery death or hard power loss.
    timeService.periodicSave(now);

    // Lockout learner: ingest observations, manage candidates, promote (Tier 7)
    lockoutLearner.process(now, timeService.nowEpochMsOr0());

    // Lockout store: periodic save when dirty (Tier 7 — best-effort, never block)
    {
        static uint32_t lastLockoutSaveMs = 0;
        static uint32_t lastLockoutSaveAttemptMs = 0;
        static constexpr uint32_t LOCKOUT_SAVE_INTERVAL_MS = 60000;  // 60s
        static constexpr uint32_t LOCKOUT_SAVE_RETRY_MS = 5000;      // Retry backoff on contention/failure
        if (lockoutStore.isDirty() && storageManager.isReady() &&
            (now - lastLockoutSaveMs) >= LOCKOUT_SAVE_INTERVAL_MS &&
            (now - lastLockoutSaveAttemptMs) >= LOCKOUT_SAVE_RETRY_MS) {
            lastLockoutSaveAttemptMs = now;
            static constexpr const char* LOCKOUT_ZONES_PATH = "/v1simple_lockout_zones.json";
            JsonDocument doc;
            lockoutStore.toJson(doc);
            fs::FS* fs = storageManager.getFilesystem();
            bool saveOk = false;
            bool saveDeferred = false;
            if (fs) {
                if (storageManager.isSDCard()) {
                    // Core 1 path must never block waiting for SD ownership.
                    StorageManager::SDTryLock sdLock(storageManager.getSDMutex());
                    if (sdLock) {
                        saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_ZONES_PATH, doc);
                    } else {
                        saveDeferred = true;
                        static uint32_t lastLockoutSaveSkipLogMs = 0;
                        if ((now - lastLockoutSaveSkipLogMs) >= 10000) {
                            lastLockoutSaveSkipLogMs = now;
                            Serial.println("[Lockout] Save deferred (SD busy or DMA-starved)");
                        }
                    }
                } else {
                    // LittleFS fallback path (single-CPU filesystem access).
                    saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_ZONES_PATH, doc);
                }
            }
            if (saveOk) {
                lastLockoutSaveMs = now;
                lockoutStore.clearDirty();
                Serial.printf("[Lockout] Saved %lu zones to %s\n",
                              static_cast<unsigned long>(lockoutStore.stats().entriesSaved),
                              LOCKOUT_ZONES_PATH);
            } else if (!saveDeferred) {
                Serial.println("[Lockout] Save failed");
            }
        }
    }
    // Learner pending candidates: periodic best-effort save (Tier 7).
    {
        static uint32_t lastLearnerSaveMs = 0;
        static uint32_t lastLearnerSaveAttemptMs = 0;
        static constexpr uint32_t LEARNER_SAVE_INTERVAL_MS = 15000;  // 15s
        static constexpr uint32_t LEARNER_SAVE_RETRY_MS = 5000;      // Retry backoff
        if (lockoutLearner.isDirty() && storageManager.isReady() &&
            (now - lastLearnerSaveMs) >= LEARNER_SAVE_INTERVAL_MS &&
            (now - lastLearnerSaveAttemptMs) >= LEARNER_SAVE_RETRY_MS) {
            lastLearnerSaveAttemptMs = now;
            static constexpr const char* LOCKOUT_PENDING_PATH = "/v1simple_lockout_pending.json";
            JsonDocument doc;
            lockoutLearner.toJson(doc);
            fs::FS* fs = storageManager.getFilesystem();
            bool saveOk = false;
            bool saveDeferred = false;
            if (fs) {
                if (storageManager.isSDCard()) {
                    // Core 1 path must never block waiting for SD ownership.
                    StorageManager::SDTryLock sdLock(storageManager.getSDMutex());
                    if (sdLock) {
                        saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_PENDING_PATH, doc);
                    } else {
                        saveDeferred = true;
                    }
                } else {
                    // LittleFS fallback path (single-CPU filesystem access).
                    saveOk = StorageManager::writeJsonFileAtomic(*fs, LOCKOUT_PENDING_PATH, doc);
                }
            }
            if (saveOk) {
                lastLearnerSaveMs = now;
                lockoutLearner.clearDirty();
                Serial.printf("[Learner] Saved %u pending candidates to %s\n",
                              static_cast<unsigned>(lockoutLearner.activeCandidateCount()),
                              LOCKOUT_PENDING_PATH);
            } else if (!saveDeferred) {
                Serial.println("[Learner] Pending save failed");
            }
        }
    }

    // Short FreeRTOS delay to yield CPU without capping loop at ~200 Hz
    vTaskDelay(pdMS_TO_TICKS(1));
    lastLoopUs = micros() - loopStartUs;
}
