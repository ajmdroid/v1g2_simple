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
#include "battery_manager.h"
#include "storage_manager.h"
#include "debug_logger.h"
#include "audio_beep.h"
#include "gps_handler.h"
#include "lockout_manager.h"
#include "auto_lockout_manager.h"
#include "obd_handler.h"
#include "camera_manager.h"
#include "perf_metrics.h"
#include "../include/config.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/camera/camera_alert_module.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/touch/touch_ui_module.h"
#include "modules/touch/tap_gesture_module.h"
#include "modules/wifi/wifi_orchestrator_module.h"
#include "modules/power/power_module.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/ble/connection_state_module.h"
#include "modules/display/display_pipeline_module.h"
#include "modules/camera/camera_load_coordinator_module.h"
#include "modules/obd/obd_auto_connector_module.h"
#include "modules/lockout/auto_lockout_maintenance_module.h"
#include "esp_heap_caps.h"
#include "esp_core_dump.h"
#include "modules/voice/voice_module.h"
#include "modules/speed_volume/speed_volume_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/perf/debug_macros.h"
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

// GPS and Lockout managers (optional modules)
// GPS is static allocation to avoid heap fragmentation - use begin()/end() to enable/disable
GPSHandler gpsHandler;
LockoutManager lockouts;
AutoLockoutManager autoLockouts;

// Alert persistence module
AlertPersistenceModule alertPersistenceModule;

// Voice Module - handles voice announcement decisions
VoiceModule voiceModule;

// OBD-II handler uses global obdHandler from obd_handler.cpp
// (included via obd_handler.h extern declaration)

unsigned long lastDisplayUpdate = 0;

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

static uint32_t buildLogCategoryBitmap(const DebugLogConfig& cfg) {
    uint32_t mask = 0;
    if (cfg.alerts) mask |= (1u << 0);
    if (cfg.wifi) mask |= (1u << 1);
    if (cfg.ble) mask |= (1u << 2);
    if (cfg.gps) mask |= (1u << 3);
    if (cfg.obd) mask |= (1u << 4);
    if (cfg.system) mask |= (1u << 5);
    if (cfg.display) mask |= (1u << 6);
    if (cfg.perfMetrics) mask |= (1u << 7);
    if (cfg.audio) mask |= (1u << 8);
    if (cfg.camera) mask |= (1u << 9);
    if (cfg.lockout) mask |= (1u << 10);
    if (cfg.touch) mask |= (1u << 11);
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

static constexpr unsigned long OBD_CONNECT_DELAY_MS = 5000;  // 5 second delay after V1 connects

// Volume fade module - reduce V1 volume after X seconds of continuous alert
VolumeFadeModule volumeFadeModule;

// Speed volume module - boost volume at highway speeds
SpeedVolumeModule speedVolumeModule;

// Camera alerts + test/demo handler
CameraAlertModule cameraAlertModule;

// Auto-push profile state machine
AutoPushModule autoPushModule;
TouchUiModule touchUiModule;
TapGestureModule tapGestureModule;
PowerModule powerModule;
BleQueueModule bleQueueModule;
ConnectionStateModule connectionStateModule;
DisplayPipelineModule displayPipelineModule;
CameraLoadCoordinator cameraLoadCoordinator;
ObdAutoConnector obdAutoConnector;
AutoLockoutMaintenance autoLockoutMaintenance;
DisplayRestoreModule displayRestoreModule;

// Callback for BLE data reception - just queues data, doesn't process
// This runs in BLE task context, so we avoid SPI operations here
void onV1Data(const uint8_t* data, size_t length, uint16_t charUUID) {
    bleQueueModule.onNotify(data, length, charUUID);
}

// WiFi orchestration helper encapsulates WiFi start + callback wiring
static WifiOrchestrator& getWifiOrchestrator() {
    static WifiOrchestrator orchestrator(
        wifiManager,
        debugLogger,
        bleClient,
        parser,
        settingsManager,
        storageManager,
        gpsHandler,
        cameraManager,
        cameraAlertModule,
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
    
    if (settingsManager.isFeaturesRuntimeEnabled()) {
        // Schedule delayed OBD auto-connect if OBD is enabled
        if (s.obdEnabled) {
            obdAutoConnector.scheduleAfterConnect(OBD_CONNECT_DELAY_MS);
            SerialLog.printf("[OBD] V1 connected - will attempt OBD connect in %lums\n", OBD_CONNECT_DELAY_MS);
        }
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
    // Wait for USB to stabilize after upload
    delay(100);
// Backlight is handled in display.begin() (inverted PWM for Waveshare)

#if defined(PIN_POWER_ON) && PIN_POWER_ON >= 0
    // Cut panel power until we intentionally bring it up
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, LOW);
#endif

    Serial.begin(115200);
    delay(200);  // Reduced from 500ms - brief delay for serial init
    
    // PANIC BREADCRUMBS: Log crash info FIRST (before any other init)
    logPanicBreadcrumbs();
    
    // Check NVS health early - before other subsystems start using it
    nvsHealthCheck();
    
    SerialLog.println("\n===================================");
    SerialLog.println("V1 Gen2 Simple Display");
    SerialLog.println("Firmware: " FIRMWARE_VERSION);
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
    
    // Initialize battery manager EARLY - needs to latch power on if running on battery
    // This must happen before any long-running init to prevent shutdown
#if defined(DISPLAY_WAVESHARE_349)
    batteryManager.begin();
    
    // DEBUG: Simulate battery for testing UI (uncomment to test)
    // batteryManager.simulateBattery(3800);  // 60% battery
#endif
    
    // Initialize display
    if (!display.begin()) {
        SerialLog.println("Display initialization failed!");
        fatalBootError("Display init failed", false);
    }
    
    // Brief delay to ensure panel is fully cleared before enabling backlight
    delay(100);

    // Initialize settings BEFORE showing any styled screens (need displayStyle setting)
    settingsManager.begin();
    const bool featuresRuntimeEnabled = settingsManager.isFeaturesRuntimeEnabled();

#if defined(DISPLAY_WAVESHARE_349)
    powerModule.begin(&batteryManager, &display, &settingsManager, &debugLogger);
    powerModule.logStartupStatus();
#endif

    // Show boot splash only on true power-on (not crash reboots or firmware uploads)
    if (resetReason == ESP_RST_POWERON) {
        // True cold boot - show splash
        display.showBootSplash();
        delay(2500);  // Show logo for 2.5 seconds
    }
    // After splash (or skipping it), show scanning screen until connected
    display.showScanning();
    
    // Show the current profile indicator
    display.drawProfileIndicator(settingsManager.get().activeSlot);

    // Initialize display preview driver
    displayPreviewModule.begin(&display);

    // Initialize auxiliary coordinators
    cameraLoadCoordinator.begin(&cameraManager, &storageManager, &debugLogger);
    if (featuresRuntimeEnabled) {
        obdAutoConnector.begin(&obdHandler);
    }

    if (featuresRuntimeEnabled) {
        // Initialize camera alert module (display + detection helpers)
        cameraAlertModule.begin(&display, &settingsManager, &cameraManager, &gpsHandler);
    }
    
    // If you want to show the demo, call display.showDemo() manually elsewhere (e.g., via a button or menu)

    // Mount storage (SD if available, else LittleFS) for profiles and settings
    SerialLog.println("[Setup] Mounting storage...");
    if (storageManager.begin()) {
        SerialLog.printf("[Setup] Storage ready: %s\n", storageManager.statusText().c_str());
        v1ProfileManager.begin(storageManager.getFilesystem());
        audio_init_sd();  // Initialize SD-based frequency voice audio

        // Ensure auto-lockout log exists on SD for crash-safe learning replay
        if (storageManager.isSDCard()) {
            fs::FS* fs = storageManager.getFilesystem();
            if (fs && !fs->exists("/v1simple_auto_lockouts.log")) {
                File logFile = fs->open("/v1simple_auto_lockouts.log", "w");
                if (logFile) {
                    logFile.close();
                    SerialLog.println("[Setup] Created auto-lockout log on SD");
                } else {
                    SerialLog.println("[Setup] WARNING: Unable to create auto-lockout log on SD");
                }
            }
        }
        
        // Validate profile references in auto-push slots
        // Clear references to profiles that don't exist
        settingsManager.validateProfileReferences(v1ProfileManager);
        
        // Retry settings restore now that SD is mounted
        // (settings.begin() runs before storage, so restore may have failed)
        if (settingsManager.checkAndRestoreFromSD()) {
            // Settings were restored from SD - update display with restored brightness
            display.setBrightness(settingsManager.get().brightness);
        }
        
        if (featuresRuntimeEnabled) {
            // Initialize lockout managers (requires storage to be ready)
            autoLockouts.setLockoutManager(&lockouts);
            lockouts.loadFromJSON("/v1profiles/lockouts.json");
            autoLockouts.loadFromJSON("/v1simple/auto_lockouts.json");
            SerialLog.printf("[Setup] Loaded %d lockout zones, %d learning clusters\n",
                            lockouts.getLockoutCount(), autoLockouts.getClusterCount());
            
            // Enable async log mode for auto-lockout (background SD writes)
            // This prevents alert hot path from blocking on SD I/O
            autoLockouts.setAsyncLogMode(true);

            autoLockoutMaintenance.begin(&autoLockouts);
            
            // Initialize GPS if enabled in settings (static allocation - just call begin())
            if (settingsManager.isGpsEnabled()) {
                SerialLog.println("[Setup] GPS enabled - initializing...");
                gpsHandler.begin();
                
                // Start camera database loading immediately in background task
                // With binary format this only takes ~1.6s for 71k cameras
                // Loading runs in parallel with BLE/WiFi init
                if (storageManager.isSDCard()) {
                    SerialLog.println("[Setup] Starting camera database load (background)...");
                    cameraLoadCoordinator.startImmediateLoad();
                }
            } else {
                SerialLog.println("[Setup] GPS disabled in settings");
            }
            
            // Initialize OBD if enabled in settings
            if (settingsManager.isObdEnabled()) {
                SerialLog.println("[Setup] OBD enabled - initializing...");
                obdHandler.begin();
            } else {
                SerialLog.println("[Setup] OBD disabled in settings");
            }
        }
    } else {
        SerialLog.println("[Setup] Storage unavailable - profiles will be disabled");
    }

    // Initialize auto-push module after settings/profiles are ready
    autoPushModule.begin(&settingsManager, &v1ProfileManager, &bleClient, &display);

    // Touch/UI module callbacks
    TouchUiModule::Callbacks touchCbs{
        .isWifiSetupActive = [] { return wifiManager.isSetupModeActive(); },
        .stopWifiSetup = [] { wifiManager.stopSetupMode(true); },
        .startWifi = [] { getWifiOrchestrator().startWifi(); },
        .drawWifiIndicator = [] { display.drawWiFiIndicator(); },
        .restoreDisplay = [] {
            if (bleClient.isConnected()) {
                display.forceNextRedraw();
                DisplayState state = parser.getDisplayState();
                if (parser.hasAlerts()) {
                    AlertData priority = parser.getPriorityAlert();
                    const auto& alerts = parser.getAllAlerts();
                    cameraAlertModule.updateCardStateForV1(true);
                    display.update(priority, alerts.data(), parser.getAlertCount(), state);
                } else {
                    cameraAlertModule.updateCardStateForV1(false);
                    display.update(state);
                }
            } else {
                display.forceNextRedraw();
                display.showResting();
            }
        },
        .deleteDebugLogs = [] {
            bool success = debugLogger.clear();
            if (success) {
                Serial.println("[Main] Debug logs deleted via touch UI");
            }
            return success;
        }
    };

    touchUiModule.begin(&display, &touchHandler, &settingsManager, touchCbs);

    tapGestureModule.begin(&touchHandler,
                           &settingsManager,
                           &display,
                           &bleClient,
                           &parser,
                           &cameraAlertModule,
                           &autoPushModule,
                           &alertPersistenceModule,
                           &displayMode);

    // Initialize debug logger after storage is mounted
    debugLogger.begin();
    
    // Restore cached time from NVS (if available and not too stale)
    // This sets the RTC to a reasonable time before GPS/NTP sync completes
    debugLogger.restoreTimeFromCache();
    
    {
        DebugLogConfig cfg = settingsManager.getDebugLogConfig();
        DebugLogFilter filter{cfg.alerts, cfg.wifi, cfg.ble, cfg.gps, cfg.obd, cfg.system, cfg.display, cfg.perfMetrics, cfg.audio, cfg.camera, cfg.lockout, cfg.touch};
        debugLogger.setFilter(filter);
    }
    debugLogger.setEnabled(settingsManager.get().enableDebugLogging);
    if (debugLogger.isEnabledFor(DebugLogCategory::System)) {
        debugLogger.logf(DebugLogCategory::System, "Debug logging enabled (storage=%s, format=JSON)", 
                         storageManager.statusText().c_str());
    }

    uint32_t bootId = nextBootId();
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
    if (debugLogger.isEnabledFor(DebugLogCategory::System)) {
        debugLogger.logf(DebugLogCategory::System,
                         "BOOT bootId=%lu reset=%s git=%s scenario=%s wifi=%s logCats=0x%08lX",
                         (unsigned long)bootId,
                         resetStr,
                         gitSha,
                         scenario,
                         bootSettings.enableWifi ? "on" : "off",
                         (unsigned long)logMask);
        
        // If this was a crash recovery, log the panic info from LittleFS to SD card debug log
        bool wasCrash = (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_INT_WDT || 
                         resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT);
        if (wasCrash && LittleFS.exists("/panic.txt")) {
            File f = LittleFS.open("/panic.txt", "r");
            if (f) {
                String panicInfo = f.readString();
                f.close();
                // Log each line as separate entries for readability
                int start = 0;
                int end;
                while ((end = panicInfo.indexOf('\n', start)) != -1) {
                    String line = panicInfo.substring(start, end);
                    line.trim();
                    if (line.length() > 0) {
                        debugLogger.logf(DebugLogCategory::System, "PANIC_RECOVERY: %s", line.c_str());
                    }
                    start = end + 1;
                }
                // Handle last line without newline
                if (start < (int)panicInfo.length()) {
                    String line = panicInfo.substring(start);
                    line.trim();
                    if (line.length() > 0) {
                        debugLogger.logf(DebugLogCategory::System, "PANIC_RECOVERY: %s", line.c_str());
                    }
                }
            }
        }
    }

    // Emit RUN header for benchmark tracking (debug log only, not serial)
    if (debugLogger.isEnabledFor(DebugLogCategory::System)) {
        JsonDocument runDoc;
        runDoc["fw"] = FIRMWARE_VERSION;
        #ifdef GIT_SHA
        runDoc["git"] = GIT_SHA;
        #else
        runDoc["git"] = "unknown";
        #endif
        #ifdef BUILD_TIMESTAMP
        runDoc["build"] = BUILD_TIMESTAMP;
        #else
        runDoc["build"] = "unknown";
        #endif
        runDoc["board"] = "waveshare-349";
        runDoc["queueDepth"] = 48;
        runDoc["drawMinMs"] = 30;
        const V1Settings& runSettings = settingsManager.get();
        runDoc["wifi"] = runSettings.enableWifi;
        runDoc["proxy"] = runSettings.proxyBLE;
        runDoc["logPerf"] = runSettings.logPerfMetrics;
        runDoc["scenario"] = "default";
        
        String runJson;
        serializeJson(runDoc, runJson);
        debugLogger.logf(DebugLogCategory::System, "RUN %s", runJson.c_str());
    }

    // WiFi startup behavior - either auto-start or wait for BOOT button
    if (settingsManager.get().enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled (dev setting)");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "WiFi auto-start enabled (dev setting)");
        }
    } else {
        SerialLog.println("[WiFi] Off by default - start with BOOT long-press");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "WiFi auto-start disabled (manual BOOT press required)");
        }
    }
    
    // Initialize touch handler early - before BLE to avoid interleaved logs
    SerialLog.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        SerialLog.println("Touch handler initialized successfully");
    } else {
        SerialLog.println("WARNING: Touch handler failed to initialize - continuing anyway");
    }
    
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
    alertPersistenceModule.begin(&bleClient, &parser, &display, &settingsManager, &obdHandler, &gpsHandler);
    voiceModule.begin(&settingsManager, &bleClient, &obdHandler, &gpsHandler);
    speedVolumeModule.begin(&settingsManager, &bleClient, &parser, &voiceModule, &volumeFadeModule);
    volumeFadeModule.begin(&settingsManager);
    LockoutManager* lockoutPtr = featuresRuntimeEnabled ? &lockouts : nullptr;
    AutoLockoutManager* autoLockoutPtr = featuresRuntimeEnabled ? &autoLockouts : nullptr;
    displayPipelineModule.begin(&displayMode,
                                &display,
                                &parser,
                                &settingsManager,
                                &gpsHandler,
                                lockoutPtr,
                                autoLockoutPtr,
                                &bleClient,
                                &cameraAlertModule,
                                &alertPersistenceModule,
                                &volumeFadeModule,
                                &voiceModule,
                                &speedVolumeModule,
                                &debugLogger);
    bleQueueModule.begin(&bleClient, &parser, &v1ProfileManager, &displayPreviewModule, &powerModule);
    connectionStateModule.begin(&bleClient, &parser, &display, &powerModule, &bleQueueModule);
    displayRestoreModule.begin(&display, &parser, &bleClient, &displayPreviewModule, &cameraAlertModule);

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
#else
    SerialLog.println("[REPLAY_MODE] BLE disabled - using packet replay for UI testing");
#endif
    
    // Auto-start WiFi if enabled in dev settings
    if (settingsManager.get().enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled - starting AP now...");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "WiFi auto-start: starting AP");
        }
        getWifiOrchestrator().startWifi();
        SerialLog.println("Setup complete - BLE scanning, WiFi auto-started");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "Setup complete (WiFi auto-started)");
        }
    } else {
        SerialLog.println("Setup complete - BLE scanning, WiFi off until BOOT long-press");
        if (debugLogger.isEnabledFor(DebugLogCategory::Wifi)) {
            debugLogger.log(DebugLogCategory::Wifi, "Setup complete (WiFi idle until BOOT long-press)");
        }
    }
}

void loop() {
    const bool featuresRuntimeEnabled = settingsManager.isFeaturesRuntimeEnabled();
    unsigned long loopStartUs = micros();
    unsigned long now = millis();
    static constexpr unsigned long AUDIO_TICK_MAX_MS = 25;
    static constexpr unsigned long OVERLOAD_LOOP_US = 25000;
    static constexpr unsigned long FREQ_UI_MAX_MS = 100;
    static unsigned long lastAudioTickMs = 0;
    static unsigned long lastFreqUiMs = 0;
    static unsigned long lastLoopUs = 0;
    bool skipNonCoreThisLoop = (now - lastAudioTickMs) >= AUDIO_TICK_MAX_MS;
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
            if (debugLogger.isEnabledFor(DebugLogCategory::System)) {
                debugLogger.logf(DebugLogCategory::System, "RUN_START trigger=%s millis=%lu", trigger, now);
            }
        }
    }

    if (!overloadThisLoop) {
        // Update BLE indicator: show when V1 is connected; color reflects JBV1 connection
        // Third param is "receiving" - true if we got V1 packets in last 2s (heartbeat visual)
        unsigned long lastRx = bleQueueModule.getLastRxMillis();
        bool bleReceiving = (now - lastRx) < 2000;
        display.setBLEProxyStatus(bleClient.isConnected(), bleClient.isProxyClientConnected(), bleReceiving);
    }
    
    // Process audio amp timeout (disables amp after 3s of inactivity)
    audio_process_amp_timeout();
    lastAudioTickMs = now;

    // Drive color preview (band cycle) first; skip other updates if active
    if (!overloadThisLoop) {
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
    // WiFi priority mode: suppress BLE scans when UI is actively being used
    // This improves web responsiveness by reducing radio contention
    // Timeout: 2 seconds after last HTTP request
    bool uiActive = wifiManager.isUiActive(2000);
    if (uiActive != bleClient.isWifiPriority()) {
        bleClient.setWifiPriority(uiActive);
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
    
    // Drive display pipeline separately from BLE drain (decoupled for accurate timing)
    // This is intentionally outside the bleDrain timing to isolate display latency
    if (bleQueueModule.consumeParsedFlag()) {
        // Skip display pipeline if preview is running (don't overwrite demo)
        if (!displayPreviewModule.isRunning()) {
            uint32_t nowMs = millis();
            uint32_t parsedTs = bleQueueModule.getLastParsedTimestamp();
            if (parsedTs != 0 && nowMs >= parsedTs) {
                perfRecordNotifyToDisplayMs(nowMs - parsedTs);
            }
            if (!overloadThisLoop) {
                uint32_t dispPipeStartUs = PERF_TIMESTAMP_US();
                displayPipelineModule.handleParsed(nowMs);
                perfRecordDispPipeUs(PERF_TIMESTAMP_US() - dispPipeStartUs);
                lastFreqUiMs = nowMs;
            }
        }
    }

    if (!displayPreviewModule.isRunning() && (now - lastFreqUiMs) >= FREQ_UI_MAX_MS) {
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

    // Drive auto-push state machine (non-blocking)
    autoPushModule.process();

    if (!skipNonCoreThisLoop) {
        // Process WiFi/web server
        uint32_t wifiStartUs = PERF_TIMESTAMP_US();
        wifiManager.process();
        perfRecordWifiProcessUs(PERF_TIMESTAMP_US() - wifiStartUs);
    }
    
    if (!skipNonCoreThisLoop && featuresRuntimeEnabled) {
        // Process GPS updates (if enabled - static allocation uses isEnabled())
        if (gpsHandler.isEnabled()) {
            uint32_t gpsStartUs = PERF_TIMESTAMP_US();
            gpsHandler.update();
            perfRecordGpsUs(PERF_TIMESTAMP_US() - gpsStartUs);
            
            // Auto-disable GPS if module not detected after timeout
            if (gpsHandler.isDetectionComplete() && !gpsHandler.isModuleDetected()) {
                Serial.println("[GPS] Module not detected - disabling GPS");
                gpsHandler.end();  // Static allocation - use end() instead of delete
                settingsManager.setGpsEnabled(false);
            }
        }
    }
    
    if (!skipNonCoreThisLoop && featuresRuntimeEnabled) {
        // Camera alerts + cache maintenance (requires GPS with valid fix)
        {
            uint32_t camStartUs = PERF_TIMESTAMP_US();
            cameraAlertModule.process();
            perfRecordCameraUs(PERF_TIMESTAMP_US() - camStartUs);
        }
    }

    perfRecordLoopJitterUs(micros() - loopStartUs);
    StorageManager::updateDmaHeapCache();  // Keep DMA cache fresh for SD gating
    perfRecordHeapStats(ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                        StorageManager::getCachedFreeDma(), StorageManager::getCachedLargestDma());
    
    if (!skipNonCoreThisLoop && featuresRuntimeEnabled) {
        // OBD processing and delayed auto-connect
        {
            uint32_t obdStartUs = PERF_TIMESTAMP_US();
            obdAutoConnector.process(now);
            perfRecordObdUs(PERF_TIMESTAMP_US() - obdStartUs);
        }
    }

    // Deferred camera database loading - runs once after V1 connects
    if (!skipNonCoreThisLoop && featuresRuntimeEnabled) {
        cameraLoadCoordinator.process(bleClient.isConnected());
    }

    // Check if V1 has active alerts - determines if camera shows as main display or card
    // Also treat persisted alerts as "V1 owns display" to avoid camera overwriting them
    bool previewActive = displayPreviewModule.isRunning();
    if (!previewActive) {
        bool v1HasActiveAlerts = parser.hasAlerts() || alertPersistenceModule.isPersistenceActive();
        cameraAlertModule.updateMainDisplay(v1HasActiveAlerts);
    }
    
    // Speed-based volume: delegate to module (rate-limited internally)
    speedVolumeModule.process(now);
    
    if (!skipNonCoreThisLoop && featuresRuntimeEnabled) {
        // Periodic auto-lockout maintenance (promotion/demotion + persistence)
        {
            uint32_t lockoutStartUs = PERF_TIMESTAMP_US();
            autoLockoutMaintenance.process(now);
            perfRecordLockoutUs(PERF_TIMESTAMP_US() - lockoutStartUs);
        }
    }
    
    // Update display periodically
    now = millis();
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
        lastDisplayUpdate = now;
        if (!displayPreviewModule.isRunning()) {
            // Handle connection state transitions (connect/disconnect, stale data re-request)
            connectionStateModule.process(now);
        }
    }
    
    // Flush debug log buffer periodically (batched writes for SD performance)
    debugLogger.update();

    // Periodic perf metrics report (stability diagnostics)
    perfMetricsCheckReport();

    // Short FreeRTOS delay to yield CPU without capping loop at ~200 Hz
    vTaskDelay(pdMS_TO_TICKS(1));
    lastLoopUs = micros() - loopStartUs;
}
