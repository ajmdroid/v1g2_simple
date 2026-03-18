/**
 * main_setup_helpers.cpp — Setup-time helper functions extracted from main.cpp.
 *
 * Keeps main.cpp focused on setup()/loop() orchestration while preserving
 * existing behavior and call ordering.
 */

#include "main_internals.h"
#include "main_globals.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include <algorithm>

#include "../include/config.h"
#include "audio_beep.h"
#include "battery_manager.h"
#include "ble_client.h"
#include "display.h"
#include "display_mode.h"
#include "packet_parser.h"
#include "perf_sd_logger.h"
#include "settings.h"
#include "settings_runtime_sync.h"
#include "storage_manager.h"
#include "time_service.h"
#include "touch_handler.h"
#include "v1_devices.h"
#include "v1_profiles.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/lockout/lockout_boot_storage.h"
#include "modules/lockout/lockout_index.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/lockout/lockout_store.h"
#include "modules/lockout/road_map_reader.h"
#include "modules/lockout/signal_observation_sd_logger.h"
#include "modules/perf/debug_macros.h"
#include "modules/touch/tap_gesture_module.h"
#include <driver/gpio.h>

#include "../include/main_globals.h"

namespace {

struct V1ConnectedAutoPushSelection {
    int activeSlotIndex = 0;
    String connectedAddress;
    uint8_t deviceDefaultProfile = 0;
    int selectedSlotIndex = 0;
};

V1ConnectedAutoPushSelection resolveV1ConnectedAutoPushSelection(const V1Settings& settings) {
    V1ConnectedAutoPushSelection selection;
    selection.activeSlotIndex = std::max(0, std::min(2, settings.activeSlot));
    selection.selectedSlotIndex = selection.activeSlotIndex;

    NimBLEAddress connected = bleClient.getConnectedAddress();
    if (!connected.isNull()) {
        selection.connectedAddress = normalizeV1DeviceAddress(String(connected.toString().c_str()));
    }
    if (selection.connectedAddress.length() == 0) {
        selection.connectedAddress = normalizeV1DeviceAddress(settings.lastV1Address);
    }

    if (selection.connectedAddress.length() > 0 && v1DeviceStore.isReady()) {
        v1DeviceStore.upsertDevice(selection.connectedAddress);
        selection.deviceDefaultProfile = v1DeviceStore.getDeviceDefaultProfile(selection.connectedAddress);
        if (selection.deviceDefaultProfile >= 1 && selection.deviceDefaultProfile <= 3) {
            selection.selectedSlotIndex = static_cast<int>(selection.deviceDefaultProfile) - 1;
        }
    }

    return selection;
}

static constexpr const char* LOCKOUT_ZONES_PATH = "/v1simple_lockout_zones.json";
static constexpr const char* LOCKOUT_ZONES_BINARY_PATH = LockoutStore::kBinaryPath;
static constexpr const char* LOCKOUT_PENDING_PATH = "/v1simple_lockout_pending.json";

bool loadPendingLearnerJsonDocument(JsonDocument& outDoc) {
    if (!storageManager.isReady()) {
        return false;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!(fs && fs->exists(LOCKOUT_PENDING_PATH))) {
        SerialLog.println("[Learner] No saved pending candidate file found");
        return false;
    }

    File f = fs->open(LOCKOUT_PENDING_PATH, "r");
    if (!(f && f.size() > 0 && f.size() < 32768)) {
        if (f) {
            f.close();
        }
        return false;
    }

    const DeserializationError err = deserializeJson(outDoc, f);
    f.close();
    if (err) {
        SerialLog.printf("[Learner] Pending JSON parse error: %s\n", err.c_str());
        return false;
    }
    return true;
}

}  // namespace

void onV1ConnectImmediate() {
    mainRuntimeState.v1ConnectedAtMs = millis();

    // Start a new perf CSV session so scoring tools can isolate
    // V1-connected data from idle boot noise.
    perfSdLogger.startNewSession();
}

void onV1Connected() {
    const V1Settings& s = settingsManager.get();
    const V1ConnectedAutoPushSelection selection = resolveV1ConnectedAutoPushSelection(s);

    const AutoPushSlot& slot = settingsManager.getSlot(selection.selectedSlotIndex);
    SerialLog.printf("[AutoPush] onV1Connected autoPush=%s activeSlot=%d selectedSlot=%d defaultProfile=%u addr='%s' profile='%s' mode=%d\n",
                     s.autoPushEnabled ? "on" : "off",
                     selection.activeSlotIndex,
                     selection.selectedSlotIndex,
                     static_cast<unsigned>(selection.deviceDefaultProfile),
                     selection.connectedAddress.c_str(),
                     slot.profileName.c_str(),
                     static_cast<int>(slot.mode));
    if (selection.activeSlotIndex != s.activeSlot) {
        AUTO_PUSH_LOGF("[AutoPush] WARNING: activeSlot out of range (%d). Using slot %d instead.\n",
                       s.activeSlot, selection.activeSlotIndex);
    }

    if (!s.autoPushEnabled) {
        AUTO_PUSH_LOGLN("[AutoPush] Disabled, skipping");
        return;
    }

    if (selection.deviceDefaultProfile >= 1 && selection.deviceDefaultProfile <= 3) {
        AUTO_PUSH_LOGF("[AutoPush] Using per-device default profile %u -> slot %d\n",
                       static_cast<unsigned>(selection.deviceDefaultProfile),
                       selection.selectedSlotIndex);
    } else {
        AUTO_PUSH_LOGF("[AutoPush] Using global activeSlot: %d\n", selection.selectedSlotIndex);
    }

    const auto queueResult = autoPushModule.queueSlotPush(selection.selectedSlotIndex);
    if (queueResult != AutoPushModule::QueueResult::QUEUED) {
        AUTO_PUSH_LOGF("[AutoPush] Skipped queue on connect, result=%d\n",
                       static_cast<int>(queueResult));
    }
}

void initializeStorageAndProfiles() {
    // Mount storage (SD if available, else LittleFS) for profiles and settings.
    SerialLog.println("[Setup] Mounting storage...");
    if (storageManager.begin()) {
        SerialLog.printf("[Setup] Storage ready: %s\n", storageManager.statusText().c_str());
        v1ProfileManager.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        v1DeviceStore.begin(storageManager.getFilesystem(), storageManager.getLittleFS());
        audio_init_buffers();  // Allocate audio decode buffers in PSRAM (before audio_init_sd).
        audio_init_sd();  // Initialize SD-based frequency voice audio.

        // Retry settings restore now that SD is mounted
        // (settings.begin() runs before storage, so restore may have failed)
        if (settingsManager.checkAndRestoreFromSD()) {
            // Settings were restored from SD - update display with restored brightness.
            display.setBrightness(settingsManager.get().brightness);
        }

        const String restoredLastKnownV1 = normalizeV1DeviceAddress(settingsManager.get().lastV1Address);
        if (restoredLastKnownV1.length() > 0 && v1DeviceStore.isReady()) {
            v1DeviceStore.upsertDevice(restoredLastKnownV1);
        }

        // Validate profile references in auto-push slots.
        // Clear references to profiles that don't exist.
        settingsManager.validateProfileReferences(v1ProfileManager);
    } else {
        SerialLog.println("[Setup] Storage unavailable - profiles will be disabled");
    }
}

void applyLockoutPolicyAndLoadZonesFromStorage() {
    // Apply persisted Ka lockout policy before loading/sanitizing lockout zones.
    SettingsRuntimeSync::syncLockoutBandLearningPolicy(settingsManager.get());

    lockoutStore.begin(&lockoutIndex);

    if (!storageManager.isReady()) {
        return;
    }

    fs::FS* fs = storageManager.getFilesystem();
    if (!fs) {
        return;
    }

    LockoutBootLoadResult loadResult;
    if (!loadLockoutZonesBinaryFirst(*fs,
                                     lockoutStore,
                                     LOCKOUT_ZONES_BINARY_PATH,
                                     LOCKOUT_ZONES_PATH,
                                     LockoutStore::kJsonMigratedBackupPath,
                                     normalizeLegacyLockoutRadiusScale,
                                     &loadResult)) {
        switch (loadResult.outcome) {
            case LockoutBootLoadOutcome::NoneFound:
                SerialLog.println("[Lockout] No saved zones file found");
                break;
            case LockoutBootLoadOutcome::BinaryFailedNoJson:
                SerialLog.printf("[Lockout] Binary load failed and no JSON migration source remains: %s\n",
                                 LOCKOUT_ZONES_BINARY_PATH);
                break;
            case LockoutBootLoadOutcome::JsonInvalid:
                SerialLog.printf("[Lockout] JSON migration source invalid: %s\n", LOCKOUT_ZONES_PATH);
                break;
            case LockoutBootLoadOutcome::LoadedBinary:
            case LockoutBootLoadOutcome::MigratedJson:
            case LockoutBootLoadOutcome::LoadedJsonSavePending:
                break;
        }
        return;
    }

    switch (loadResult.outcome) {
        case LockoutBootLoadOutcome::LoadedBinary:
            SerialLog.printf("[Lockout] Loaded %lu zones from %s\n",
                             static_cast<unsigned long>(lockoutStore.stats().entriesLoaded),
                             LOCKOUT_ZONES_BINARY_PATH);
            break;
        case LockoutBootLoadOutcome::MigratedJson:
        case LockoutBootLoadOutcome::LoadedJsonSavePending:
            SerialLog.printf("[Lockout] Loaded %lu zones from %s\n",
                             static_cast<unsigned long>(lockoutStore.stats().entriesLoaded),
                             LOCKOUT_ZONES_PATH);
            if (loadResult.legacyRadiusMigrations > 0) {
                SerialLog.printf("[Lockout] Normalized %lu legacy zone radius values (x10->x1 scale)\n",
                                 static_cast<unsigned long>(loadResult.legacyRadiusMigrations));
            }
            if (loadResult.outcome == LockoutBootLoadOutcome::MigratedJson) {
                SerialLog.printf("[Lockout] Migrated zones to %s\n", LOCKOUT_ZONES_BINARY_PATH);
                if (loadResult.archivedJson) {
                    SerialLog.printf("[Lockout] Archived migrated JSON to %s\n",
                                     LockoutStore::kJsonMigratedBackupPath);
                } else {
                    SerialLog.printf("[Lockout] WARN: Failed to archive migrated JSON from %s\n",
                                     LOCKOUT_ZONES_PATH);
                }
            } else {
                SerialLog.printf("[Lockout] WARN: Failed to migrate zones to %s; will retry later\n",
                                 LOCKOUT_ZONES_BINARY_PATH);
            }
            break;
        case LockoutBootLoadOutcome::NoneFound:
        case LockoutBootLoadOutcome::BinaryFailedNoJson:
        case LockoutBootLoadOutcome::JsonInvalid:
            break;
    }
}

uint32_t initializeBootPerformanceLoggers() {
    const uint32_t bootId = nextBootId();
    perfSdLogger.setBootId(bootId);
    signalObservationSdLogger.setBootId(bootId);

    // Standalone perf CSV loggers (SD only).
    const bool sdEnabled = storageManager.isReady() && storageManager.isSDCard();
    perfSdLogger.begin(sdEnabled);
    if (perfSdLogger.isEnabled()) {
        SerialLog.printf("[PERF] SD logger enabled (%s)\n", perfSdLogger.csvPath());
    } else {
        SerialLog.println("[PERF] SD logger disabled (no SD)");
    }
    signalObservationSdLogger.begin(sdEnabled);
    if (signalObservationSdLogger.isEnabled()) {
        SerialLog.printf("[LockoutSD] Candidate logger enabled (%s)\n", signalObservationSdLogger.csvPath());
    } else {
        SerialLog.println("[LockoutSD] Candidate logger disabled (no SD)");
    }

    return bootId;
}

void restorePendingLearnerCandidates() {
    JsonDocument doc;
    if (!loadPendingLearnerJsonDocument(doc)) {
        return;
    }

    if (lockoutLearner.fromJson(doc, timeService.nowEpochMsOr0())) {
        SerialLog.printf("[Learner] Restored %u pending candidates from %s\n",
                         static_cast<unsigned>(lockoutLearner.activeCandidateCount()),
                         LOCKOUT_PENDING_PATH);
    } else {
        SerialLog.printf("[Learner] Ignoring invalid pending file format: %s\n",
                         LOCKOUT_PENDING_PATH);
    }
}

void initializeTouchAndDisplayControls() {
    // Initialize touch handler early - before BLE to avoid interleaved logs
    SerialLog.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        SerialLog.println("Touch handler initialized successfully");
    } else {
        SerialLog.println("WARNING: Touch handler failed to initialize - continuing anyway");
    }

    // Initialize BOOT button (GPIO 0) for brightness adjustment
    pinMode(BOOT_BUTTON_GPIO, INPUT_PULLUP);
    const V1Settings& displaySettings = settingsManager.get();
    display.setBrightness(displaySettings.brightness);  // Apply saved brightness
    audio_set_volume(displaySettings.voiceVolume);      // Apply saved voice volume
    SerialLog.printf("[Settings] Applied saved brightness: %d, voice volume: %d\n",
                     displaySettings.brightness, displaySettings.voiceVolume);
}

namespace {

void configureUiAutoPushModule() {
    // Initialize auto-push module after settings/profiles are ready
    autoPushModule.begin(&settingsManager, &v1ProfileManager, &bleClient, &display);
}

void configureUiTouchInteractionModules() {
    configureTouchUiModule();

    tapGestureModule.begin(&touchHandler,
                           &settingsManager,
                           &display,
                           &bleClient,
                           &parser,
                           &autoPushModule,
                           &alertPersistenceModule,
                           &displayMode);
}

}  // namespace

void configureUiInteractionModules() {
    configureUiAutoPushModule();
    configureUiTouchInteractionModules();
}

void logBootSummaryAndWifiStartup(uint32_t bootId, esp_reset_reason_t resetReason) {
    const V1Settings& bootSettings = settingsManager.get();
    const char* scenario = "default";
#ifdef GIT_SHA
    const char* gitSha = GIT_SHA;
#else
    const char* gitSha = "unknown";
#endif
    const char* resetStr = resetReasonToString(resetReason);
    SerialLog.printf("BOOT bootId=%lu reset=%s git=%s scenario=%s wifiMaster=%s wifiAtBoot=%s\n",
                     static_cast<unsigned long>(bootId),
                     resetStr,
                     gitSha,
                     scenario,
                     bootSettings.enableWifi ? "on" : "off",
                     bootSettings.enableWifiAtBoot ? "on" : "off");

    // WiFi startup behavior - either auto-start or wait for BOOT button
    if (!bootSettings.enableWifi) {
        SerialLog.println("[WiFi] Master disabled - startup and loop processing skipped");
    } else if (bootSettings.enableWifiAtBoot) {
        SerialLog.println("[WiFi] Auto-start enabled (dev setting)");
    } else {
        SerialLog.println("[WiFi] Off by default - start with BOOT long-press");
    }
}

void initializeEarlyBootDiagnostics() {
    // Wait for USB to stabilize after upload.
    delay(50);

    // Release GPIO hold from deep sleep (backlight was held off during sleep).
    // Must happen before display init re-configures the pin.
    gpio_deep_sleep_hold_dis();
    gpio_hold_dis(static_cast<gpio_num_t>(LCD_BL));

    // Backlight is handled in display.begin() (inverted PWM for Waveshare).
    Serial.begin(115200);
    delay(30);  // Conservative USB CDC settle.

    // PANIC BREADCRUMBS: Log crash info FIRST (before any other init).
    logPanicBreadcrumbs();

    // Check NVS health early - before other subsystems start using it.
    nvsHealthCheck();
}
