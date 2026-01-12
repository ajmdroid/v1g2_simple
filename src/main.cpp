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
#include "wifi_manager.h"
#include "settings.h"
#include "touch_handler.h"
#include "v1_profiles.h"
#include "battery_manager.h"
#include "storage_manager.h"
#include "audio_beep.h"
#include "../include/config.h"
#define SerialLog Serial  // Alias: serial logger removed; use Serial directly
#include <FS.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Gate verbose logs behind debug switches (keep off in normal builds)
static constexpr bool DEBUG_LOGS = false;          // General debug logging (packet dumps, status)
static constexpr bool AUTOPUSH_DEBUG_LOGS = false;  // AutoPush-specific verbose logs
#define DEBUG_LOGF(...) do { if (DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define DEBUG_LOGLN(msg) do { if (DEBUG_LOGS) SerialLog.println(msg); } while (0)
#define AUTO_PUSH_LOGF(...) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.printf(__VA_ARGS__); } while (0)
#define AUTO_PUSH_LOGLN(msg) do { if (AUTOPUSH_DEBUG_LOGS) SerialLog.println(msg); } while (0)

// Global objects
V1BLEClient bleClient;
PacketParser parser;
V1Display display;
TouchHandler touchHandler;

// Queue for BLE data - decouples BLE callbacks from display updates
static QueueHandle_t bleDataQueue = nullptr;
struct BLEDataPacket {
    uint8_t data[128];  // V1 packets are typically <60 bytes; 128 gives headroom
    size_t length;
    uint16_t charUUID;  // Last 16-bit of characteristic UUID to identify source
    uint32_t tsMs;      // Timestamp for latency measurement
};

unsigned long lastDisplayUpdate = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastLvTick = 0;
unsigned long lastRxMillis = 0;
unsigned long lastDisplayDraw = 0;  // Throttle display updates
const unsigned long DISPLAY_DRAW_MIN_MS = 20;  // Min 20ms between draws (~50fps) to reduce display lag
static unsigned long lastAlertGapRecoverMs = 0;  // Throttle recovery when bands show but alerts are missing

// Color preview state machine to keep demo visible and cycle bands
static bool colorPreviewActive = false;
static unsigned long colorPreviewStartMs = 0;
static unsigned long colorPreviewDurationMs = 0;
static int colorPreviewStep = 0;
static bool colorPreviewEnded = false;

struct ColorPreviewStep {
    unsigned long offsetMs;
    Band band;
    uint8_t bars;
    Direction dir;
    uint32_t freqMHz;
    bool muted;
};

// Sequence: X, K, Ka, Laser, then Ka muted - with arrow cycle and cards
static const ColorPreviewStep COLOR_PREVIEW_STEPS[] = {
    {300, BAND_X, 3, DIR_FRONT, 10525, false},
    {1300, BAND_K, 5, DIR_SIDE, 24150, false},
    {2300, BAND_KA, 6, DIR_REAR, 35500, false},
    {3300, BAND_LASER, 8, static_cast<Direction>(DIR_FRONT | DIR_REAR), 0, false},
    {4300, BAND_KA, 5, DIR_FRONT, 34700, true}   // Muted Ka to show mute badge
};
static constexpr int COLOR_PREVIEW_STEP_COUNT = sizeof(COLOR_PREVIEW_STEPS) / sizeof(COLOR_PREVIEW_STEPS[0]);

enum class DisplayMode {
    IDLE,
    LIVE
};
static DisplayMode displayMode = DisplayMode::IDLE;

// Voice alerts tracking - announces priority alert when no app is connected
static Band lastVoiceAlertBand = BAND_NONE;
static Direction lastVoiceAlertDirection = DIR_NONE;
static unsigned long lastVoiceAlertTime = 0;
static constexpr unsigned long VOICE_ALERT_COOLDOWN_MS = 5000;  // Min 5s between announcements

// WiFi manual startup - user must long-press BOOT to start AP

void requestColorPreviewHold(uint32_t durationMs) {
    colorPreviewActive = true;
    colorPreviewStartMs = millis();
    colorPreviewDurationMs = durationMs;
    colorPreviewStep = 0;
    colorPreviewEnded = false;
}

bool isColorPreviewRunning() {
    return colorPreviewActive;
}

void cancelColorPreview() {
    if (colorPreviewActive) {
        colorPreviewActive = false;
        colorPreviewEnded = true;  // Restore resting view on next loop
    }
}

static inline bool isColorPreviewActive() {
    if (!colorPreviewActive) return false;
    unsigned long now = millis();
    if (now - colorPreviewStartMs >= colorPreviewDurationMs) {
        colorPreviewActive = false;
        colorPreviewEnded = true;
        return false;
    }
    return true;
}

static void driveColorPreview() {
    if (!colorPreviewActive) return;

    unsigned long now = millis();
    unsigned long elapsed = now - colorPreviewStartMs;

    // Advance through band samples while within duration
    while (colorPreviewStep < COLOR_PREVIEW_STEP_COUNT && elapsed >= COLOR_PREVIEW_STEPS[colorPreviewStep].offsetMs) {
        const auto& step = COLOR_PREVIEW_STEPS[colorPreviewStep];
        AlertData previewAlert{};
        previewAlert.band = step.band;
        previewAlert.direction = step.dir;
        previewAlert.frontStrength = step.bars;
        previewAlert.rearStrength = 0;
        previewAlert.frequency = step.freqMHz;
        previewAlert.isValid = true;

        DisplayState previewState{};
        previewState.activeBands = step.band;
        previewState.arrows = step.dir;
        previewState.signalBars = step.bars;
        previewState.muted = step.muted;

        // Build array with secondary alerts to show cards during preview
        // Start adding secondary alerts from step 1 onwards
        AlertData allAlerts[3];
        int alertCount = 1;
        allAlerts[0] = previewAlert;
        
        if (colorPreviewStep >= 1) {
            // Add X band as secondary card
            allAlerts[alertCount].band = BAND_X;
            allAlerts[alertCount].direction = DIR_FRONT;
            allAlerts[alertCount].frontStrength = 3;
            allAlerts[alertCount].frequency = 10525;
            allAlerts[alertCount].isValid = true;
            previewState.activeBands = static_cast<Band>(previewState.activeBands | BAND_X);
            alertCount++;
        }
        if (colorPreviewStep >= 2) {
            // Add K band as secondary card
            allAlerts[alertCount].band = BAND_K;
            allAlerts[alertCount].direction = DIR_REAR;
            allAlerts[alertCount].frontStrength = 0;
            allAlerts[alertCount].rearStrength = 4;
            allAlerts[alertCount].frequency = 24150;
            allAlerts[alertCount].isValid = true;
            previewState.activeBands = static_cast<Band>(previewState.activeBands | BAND_K);
            alertCount++;
        }

        display.update(previewAlert, allAlerts, alertCount, previewState);
        colorPreviewStep++;
    }

    if (elapsed >= colorPreviewDurationMs) {
        colorPreviewActive = false;
        colorPreviewEnded = true;
    }
}

// Mute debounce - prevent flicker from rapid state changes  
static bool debouncedMuteState = false;
static unsigned long lastMuteChangeMs = 0;
static constexpr unsigned long MUTE_DEBOUNCE_MS = 150;  // Ignore mute changes within 150ms

// Alert persistence - show last alert in grey after V1 clears it
static AlertData persistedAlert;
static unsigned long alertClearedTime = 0;
static bool alertPersistenceActive = false;

// Triple-tap detection for profile cycling
static unsigned long lastTapTime = 0;
static int tapCount = 0;
const unsigned long TAP_WINDOW_MS = 600;  // Window for 3 taps
const unsigned long TAP_DEBOUNCE_MS = 150; // Minimum time between taps

// Brightness adjustment mode (BOOT button triggered)
static bool brightnessAdjustMode = false;
static uint8_t brightnessAdjustValue = 200;  // Current brightness slider value
static uint8_t volumeAdjustValue = 75;       // Current voice volume slider value
static int activeSlider = 0;                 // 0=brightness, 1=volume
static unsigned long lastVolumeChangeMs = 0; // Debounce for test voice playback
static unsigned long bootPressStart = 0;     // For long-press detection
static bool bootWasPressed = false;
static bool wifiToggleTriggered = false;     // Track if WiFi toggle already fired during this press
const unsigned long BOOT_DEBOUNCE_MS = 300;  // Debounce for BOOT button
const unsigned long AP_TOGGLE_LONG_PRESS_MS = 4000;  // Long press duration for WiFi toggle
const unsigned long VOLUME_TEST_DEBOUNCE_MS = 1000;  // Debounce for test voice after volume change


// Buffer for accumulating BLE data in main loop context
static std::vector<uint8_t> rxBuffer;

enum AutoPushStep {
    AUTO_PUSH_STEP_IDLE = 0,
    AUTO_PUSH_STEP_WAIT_READY,
    AUTO_PUSH_STEP_PROFILE,
    AUTO_PUSH_STEP_DISPLAY,
    AUTO_PUSH_STEP_MODE,
    AUTO_PUSH_STEP_VOLUME,
};

struct AutoPushState {
    AutoPushStep step = AUTO_PUSH_STEP_IDLE;
    unsigned long nextStepAtMs = 0;
    int slotIndex = 0;
    AutoPushSlot slot;
    V1Profile profile;
    bool profileLoaded = false;
};

static AutoPushState autoPushState;


// Callback for BLE data reception - just queues data, doesn't process
// This runs in BLE task context, so we avoid SPI operations here
void onV1Data(const uint8_t* data, size_t length, uint16_t charUUID) {
    if (bleDataQueue && length > 0 && length <= sizeof(BLEDataPacket::data)) {
        BLEDataPacket pkt;
        memcpy(pkt.data, data, length);
        pkt.length = length;
        pkt.charUUID = charUUID;
        pkt.tsMs = millis();
        // Non-blocking send to queue - if queue is full, drop the packet
        BaseType_t result = xQueueSend(bleDataQueue, &pkt, 0);
        if (result != pdTRUE) {
            BLEDataPacket dropped;
            xQueueReceive(bleDataQueue, &dropped, 0);  // Drop oldest to make room
            xQueueSend(bleDataQueue, &pkt, 0);
        }
    }
}

static void startAutoPush(int slotIndex) {
    static const char* slotNames[] = {"Default", "Highway", "Passenger Comfort"};
    int clampedIndex = std::max(0, std::min(2, slotIndex));
    autoPushState.slotIndex = clampedIndex;
    autoPushState.slot = settingsManager.getSlot(clampedIndex);
    autoPushState.profileLoaded = false;
    autoPushState.profile = V1Profile();
    autoPushState.step = AUTO_PUSH_STEP_WAIT_READY;
    autoPushState.nextStepAtMs = millis() + 500;
    AUTO_PUSH_LOGF("[AutoPush] V1 connected - applying '%s' profile (slot %d)...\n",
                   slotNames[clampedIndex], clampedIndex);
    
    // Show the profile indicator on display when auto-push starts
    display.drawProfileIndicator(clampedIndex);
}

static void processAutoPush() {
    if (autoPushState.step == AUTO_PUSH_STEP_IDLE) {
        return;
    }

    if (!bleClient.isConnected()) {
        autoPushState.step = AUTO_PUSH_STEP_IDLE;
        return;
    }

    unsigned long now = millis();
    if (now < autoPushState.nextStepAtMs) {
        return;
    }

    switch (autoPushState.step) {
        case AUTO_PUSH_STEP_WAIT_READY:
            autoPushState.step = AUTO_PUSH_STEP_PROFILE;
            autoPushState.nextStepAtMs = now;
            return;

        case AUTO_PUSH_STEP_PROFILE: {
            const AutoPushSlot& slot = autoPushState.slot;
            if (slot.profileName.length() > 0) {
                AUTO_PUSH_LOGF("[AutoPush] Loading profile: %s\n", slot.profileName.c_str());
                V1Profile profile;
                if (v1ProfileManager.loadProfile(slot.profileName, profile)) {
                    autoPushState.profile = profile;
                    autoPushState.profileLoaded = true;
                    
                    // Apply slot-level Mute to Zero setting to user bytes before pushing
                    bool slotMuteToZero = settingsManager.getSlotMuteToZero(autoPushState.slotIndex);
                    AUTO_PUSH_LOGF("[AutoPush] Slot %d MZ setting: %s\n", 
                                    autoPushState.slotIndex, slotMuteToZero ? "ON" : "OFF");
                    AUTO_PUSH_LOGF("[AutoPush] Profile byte0 before: 0x%02X\n", profile.settings.bytes[0]);
                    
                    V1UserSettings modifiedSettings = profile.settings;
                    if (slotMuteToZero) {
                        // MZ enabled: clear bit 4 (inverted logic)
                        modifiedSettings.bytes[0] &= ~0x10;
                    } else {
                        // MZ disabled: set bit 4
                        modifiedSettings.bytes[0] |= 0x10;
                    }
                    AUTO_PUSH_LOGF("[AutoPush] Modified byte0: 0x%02X (bit4=%d means MZ=%s)\n",
                                    modifiedSettings.bytes[0], 
                                    (modifiedSettings.bytes[0] & 0x10) ? 1 : 0,
                                    (modifiedSettings.bytes[0] & 0x10) ? "OFF" : "ON");
                    
                    if (bleClient.writeUserBytes(modifiedSettings.bytes)) {
                        AUTO_PUSH_LOGF("[AutoPush] Profile settings pushed (MZ=%s)\n", 
                                        slotMuteToZero ? "ON" : "OFF");
                        // Request read-back to verify V1 accepted the settings
                        delay(100);
                        bleClient.requestUserBytes();
                        AUTO_PUSH_LOGLN("[AutoPush] Requested user bytes read-back for verification");
                    } else {
                        AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to push profile settings");
                    }
                } else {
                    AUTO_PUSH_LOGF("[AutoPush] ERROR: Failed to load profile '%s'\n", slot.profileName.c_str());
                    autoPushState.profileLoaded = false;
                }
            } else {
                AUTO_PUSH_LOGLN("[AutoPush] No profile configured for active slot");
                autoPushState.profileLoaded = false;
            }

            // Always proceed to display step to apply slot's dark mode setting
            autoPushState.step = AUTO_PUSH_STEP_DISPLAY;
            autoPushState.nextStepAtMs = now + 100;
            return;
        }

        case AUTO_PUSH_STEP_DISPLAY: {
            // Use slot-level dark mode setting (inverted: darkMode=true means display OFF)
            bool slotDarkMode = settingsManager.getSlotDarkMode(autoPushState.slotIndex);
            bool displayOn = !slotDarkMode;  // Dark mode = display off
            bleClient.setDisplayOn(displayOn);
            AUTO_PUSH_LOGF("[AutoPush] Display set to: %s (darkMode=%s)\n",
                           displayOn ? "ON" : "OFF", slotDarkMode ? "true" : "false");
            autoPushState.step = AUTO_PUSH_STEP_MODE;
            autoPushState.nextStepAtMs = now + (autoPushState.slot.mode != V1_MODE_UNKNOWN ? 100 : 0);
            return;
        }

        case AUTO_PUSH_STEP_MODE: {
            if (autoPushState.slot.mode != V1_MODE_UNKNOWN) {
                const char* modeName = "Unknown";
                if (autoPushState.slot.mode == V1_MODE_ALL_BOGEYS) modeName = "All Bogeys";
                else if (autoPushState.slot.mode == V1_MODE_LOGIC) modeName = "Logic";
                else if (autoPushState.slot.mode == V1_MODE_ADVANCED_LOGIC) modeName = "Advanced Logic";

                if (bleClient.setMode(static_cast<uint8_t>(autoPushState.slot.mode))) {
                    AUTO_PUSH_LOGF("[AutoPush] Mode set to: %s\n", modeName);
                } else {
                    AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to set mode");
                }
            }

            bool volumeChangeNeeded =
                (settingsManager.getSlotVolume(autoPushState.slotIndex) != 0xFF ||
                 settingsManager.getSlotMuteVolume(autoPushState.slotIndex) != 0xFF);
            autoPushState.step = AUTO_PUSH_STEP_VOLUME;
            autoPushState.nextStepAtMs = now + (volumeChangeNeeded ? 100 : 0);
            return;
        }

        case AUTO_PUSH_STEP_VOLUME: {
            uint8_t mainVol = settingsManager.getSlotVolume(autoPushState.slotIndex);
            uint8_t muteVol = settingsManager.getSlotMuteVolume(autoPushState.slotIndex);
            if (mainVol != 0xFF || muteVol != 0xFF) {
                if (bleClient.setVolume(mainVol, muteVol)) {
                    AUTO_PUSH_LOGF("[AutoPush] Volume set - main: %d, muted: %d\n", mainVol, muteVol);
                } else {
                    AUTO_PUSH_LOGLN("[AutoPush] ERROR: Failed to set volume");
                }
            }

            AUTO_PUSH_LOGLN("[AutoPush] Complete");
            autoPushState.step = AUTO_PUSH_STEP_IDLE;
            autoPushState.nextStepAtMs = 0;
            return;
        }

        default:
            autoPushState.step = AUTO_PUSH_STEP_IDLE;
            return;
    }
}

// Start WiFi after BLE connects to avoid radio contention during connection
void startWifi() {
    if (wifiManager.isSetupModeActive()) return;
    
    SerialLog.println("[WiFi] Starting WiFi (manual start)...");
    wifiManager.begin();
    
    // Reduce WiFi TX power to minimize interference with BLE
    // WIFI_POWER_11dBm is a good balance - enough for local AP, less BLE interference
    WiFi.setTxPower(WIFI_POWER_11dBm);
    SerialLog.println("[WiFi] TX power reduced to 11dBm for BLE coexistence");
    
    // Set up callbacks for web interface
    wifiManager.setStatusCallback([]() {
        return "\"v1_connected\":" + String(bleClient.isConnected() ? "true" : "false");
    });
    
    wifiManager.setAlertCallback([]() {
        JsonDocument doc;
        if (parser.hasAlerts()) {
            AlertData alert = parser.getPriorityAlert();
            doc["active"] = true;
            const char* bandStr = "None";
            if (alert.band == BAND_KA) bandStr = "Ka";
            else if (alert.band == BAND_K) bandStr = "K";
            else if (alert.band == BAND_X) bandStr = "X";
            else if (alert.band == BAND_LASER) bandStr = "LASER";
            doc["band"] = bandStr;
            doc["strength"] = alert.frontStrength;
            doc["frequency"] = alert.frequency;
            doc["direction"] = alert.direction;
        } else {
            doc["active"] = false;
        }
        String json;
        serializeJson(doc, json);
        return json;
    });
    
    // Set up command callback for dark mode and mute
    wifiManager.setCommandCallback([](const char* cmd, bool state) {
        if (strcmp(cmd, "display") == 0) {
            return bleClient.setDisplayOn(state);
        } else if (strcmp(cmd, "mute") == 0) {
            return bleClient.setMute(state);
        }
        return false;
    });

    // Provide filesystem access for web profile/device APIs
    wifiManager.setFilesystemCallback([]() -> fs::FS* {
        return storageManager.isReady() ? storageManager.getFilesystem() : nullptr;
    });
    
    SerialLog.println("[WiFi] Initialized");
}

// Callback when V1 connection is fully established
// Handles auto-push of default profile and mode
void onV1Connected() {
    const V1Settings& s = settingsManager.get();
    int activeSlotIndex = std::max(0, std::min(2, s.activeSlot));
    if (activeSlotIndex != s.activeSlot) {
        AUTO_PUSH_LOGF("[AutoPush] WARNING: activeSlot out of range (%d). Using slot %d instead.\n",
                        s.activeSlot, activeSlotIndex);
    }
    
    if (!s.autoPushEnabled) {
        AUTO_PUSH_LOGLN("[AutoPush] Disabled, skipping");
        return;
    }

    // Use global activeSlot
    AUTO_PUSH_LOGF("[AutoPush] Using global activeSlot: %d\n", activeSlotIndex);

    startAutoPush(activeSlotIndex);
}

#ifdef REPLAY_MODE
// ============================
// Packet Replay for UI Testing
// ============================

// Sample V1 packets for testing (captured from real device)
// Format: 0xAA <dest> <origin> <packetID> <len> <payload...> <checksum> 0xAB

// Alert packet: Ka 33.800 GHz, front, strength 5
const uint8_t REPLAY_PACKET_KA_ALERT[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,  // Header: alert data, 12 bytes payload
    0x04, 0x01, 0x05, 0x00,        // Band=Ka(4), direction=front(1), frontStrength=5, rearStrength=0
    0x00, 0xD0, 0x2F, 0x01,        // Frequency: 33.800 GHz (0x012FD000 = 19980288 in 100kHz units)
    0x00, 0x00, 0x00, 0x01,        // Count=1, flags
    0xE8, 0xAB                     // Checksum, end
};

// Display data packet: Ka active, 3 signal bars, muted
const uint8_t REPLAY_PACKET_DISPLAY_MUTED[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,  // Header: display data, 8 bytes payload
    0x04, 0x01, 0x03, 0x01,        // activeBands=Ka(4), arrows=front(1), signalBars=3, muted=1
    0x00, 0x00, 0x00, 0x00,        // Padding
    0x00, 0x00, 0x00, 0x00,
    0x8A, 0xAB                     // Checksum, end
};

// Display data: X band active, not muted
const uint8_t REPLAY_PACKET_DISPLAY_X[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,
    0x01, 0x01, 0x04, 0x00,        // X band, front, 4 bars, not muted
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x7E, 0xAB
};

// Alert: K band 24.150 GHz, rear, strength 3
const uint8_t REPLAY_PACKET_K_ALERT[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,
    0x02, 0x02, 0x00, 0x03,        // Band=K(2), direction=rear(2), frontStrength=0, rearStrength=3
    0x00, 0x6C, 0xBE, 0x03,        // Frequency: 24.150 GHz
    0x00, 0x00, 0x00, 0x02,        // Count=2
    0xD9, 0xAB
};

// Laser alert
const uint8_t REPLAY_PACKET_LASER[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,
    0x08, 0x01, 0x08, 0x00,        // Band=Laser(8), direction=front, strength=8
    0x00, 0x00, 0x00, 0x00,        // No frequency for laser
    0x00, 0x00, 0x00, 0x01,
    0xA8, 0xAB
};

// Clear/no alert
const uint8_t REPLAY_PACKET_CLEAR[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,
    0x00, 0x00, 0x00, 0x00,        // No bands, no arrows, no bars, not muted
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x73, 0xAB
};

struct ReplayPacket {
    const uint8_t* data;
    size_t length;
    unsigned long delayMs;  // Delay before next packet
};

// Replay sequence: simulate realistic alert scenarios
const ReplayPacket replaySequence[] = {
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 2000},           // Start clear
    {REPLAY_PACKET_KA_ALERT, sizeof(REPLAY_PACKET_KA_ALERT), 100},     // Ka alert appears
    {REPLAY_PACKET_DISPLAY_MUTED, sizeof(REPLAY_PACKET_DISPLAY_MUTED), 1000},  // Show muted
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 1500},          // Clear
    {REPLAY_PACKET_DISPLAY_X, sizeof(REPLAY_PACKET_DISPLAY_X), 100},   // X band
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 2000},          // Clear
    {REPLAY_PACKET_K_ALERT, sizeof(REPLAY_PACKET_K_ALERT), 100},       // K rear
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 1500},          // Clear
    {REPLAY_PACKET_LASER, sizeof(REPLAY_PACKET_LASER), 100},           // Laser!
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 3000},          // Clear and loop
};

const size_t REPLAY_SEQUENCE_LENGTH = sizeof(replaySequence) / sizeof(replaySequence[0]);

unsigned long lastReplayTime = 0;
size_t replayIndex = 0;

void processReplayData() {
    unsigned long now = millis();
    
    // Check if it's time for the next packet
    if (now - lastReplayTime < replaySequence[replayIndex].delayMs) {
        return;
    }
    
    // Inject packet into rxBuffer (same as BLE would)
    const ReplayPacket& pkt = replaySequence[replayIndex];
    rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
    
    SerialLog.printf("[REPLAY] Injected packet %d/%d (%d bytes)\n", 
                     replayIndex + 1, REPLAY_SEQUENCE_LENGTH, pkt.length);
    
    // Advance to next packet
    lastReplayTime = now;
    replayIndex = (replayIndex + 1) % REPLAY_SEQUENCE_LENGTH;
}
#endif // REPLAY_MODE

// Process queued BLE data - called from main loop (safe for SPI)
void processBLEData() {
#ifdef REPLAY_MODE
    // In replay mode, inject test packets instead of reading from BLE
    processReplayData();
#else
    // Normal BLE mode
    if (colorPreviewActive) {
        return;  // Hold preview on-screen; skip live data
    }
    BLEDataPacket pkt;
    uint32_t latestPktTs = 0;
    
    // Process all queued packets (with safety cap to prevent unbounded growth)
    constexpr size_t RX_BUFFER_MAX = 512;  // Hard cap on buffer size
    while (xQueueReceive(bleDataQueue, &pkt, 0) == pdTRUE) {
        // NOTE: Proxy forwarding is done immediately in the BLE callback (forwardToProxyImmediate)
        // for minimal latency. We don't forward again here to avoid duplicate packets.
        
        // Accumulate and frame on 0xAA ... 0xAB so we don't choke on chunked notifications
        // Skip accumulation if buffer is at capacity (will be trimmed below)
        if (rxBuffer.size() < RX_BUFFER_MAX) {
            rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
        }
        latestPktTs = pkt.tsMs;
    }
    
    // Process the proxy queue to actually send data to connected apps (JBV1/V1 Companion)
    bleClient.processProxyQueue();
#endif
    
    // If no data accumulated, return
    if (rxBuffer.empty()) {
        return;
    }

    // Trim runaway buffers - more aggressive to prevent memory pressure
    // Normal V1 packets are <100 bytes, so 256 is plenty for recovery
    if (rxBuffer.size() > 256) {
        // Keep last 128 bytes (enough for one complete packet with framing)
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() - 128);
    }

    const size_t MIN_HEADER_SIZE = 6;
    const size_t MAX_PACKET_SIZE = 512;

    while (true) {
        auto dumpPacket = [&](const uint8_t* p, size_t len) {
            if (!DEBUG_LOGS) return;  // Skip packet dumps unless debug enabled
            SerialLog.printf("PKT id=0x%02X len=%u: ", p[3], static_cast<unsigned>(len));
            for (size_t i = 0; i < len; i++) {
                SerialLog.printf("%02X ", p[i]);
            }
            SerialLog.println();
        };

        auto startIt = std::find(rxBuffer.begin(), rxBuffer.end(), ESP_PACKET_START);
        if (startIt == rxBuffer.end()) {
            rxBuffer.clear();
            break;
        }
        if (startIt != rxBuffer.begin()) {
            rxBuffer.erase(rxBuffer.begin(), startIt);
            continue;
        }
        if (rxBuffer.size() < MIN_HEADER_SIZE) {
            break;
        }

        uint8_t lenField = rxBuffer[4];
        if (lenField == 0) {
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }

        size_t packetSize = 6 + lenField;
        if (packetSize > MAX_PACKET_SIZE) {
            SerialLog.printf("WARNING: BLE packet too large (%u bytes) - resyncing\n", (unsigned)packetSize);
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }
        if (rxBuffer.size() < packetSize) {
            break;
        }
        if (rxBuffer[packetSize - 1] != ESP_PACKET_END) {
            SerialLog.println("WARNING: Packet missing end marker - resyncing");
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }

        // Parse directly from rxBuffer - no heap allocation
        const uint8_t* packetPtr = rxBuffer.data();
        
        lastRxMillis = millis();
        if (latestPktTs == 0) {
            latestPktTs = lastRxMillis;
        }

        // Check for user bytes response (0x12) - V1 settings pull
        if (packetSize >= 12 && packetPtr[3] == PACKET_ID_RESP_USER_BYTES) {
            // Payload starts at byte 5, length is 6 bytes
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            DEBUG_LOGF("V1 user bytes raw: %02X %02X %02X %02X %02X %02X\n",
                userBytes[0], userBytes[1], userBytes[2], userBytes[3], userBytes[4], userBytes[5]);
            DEBUG_LOGF("  xBand=%d, kBand=%d, kaBand=%d, laser=%d\n",
                userBytes[0] & 0x01, (userBytes[0] >> 1) & 0x01, 
                (userBytes[0] >> 2) & 0x01, (userBytes[0] >> 3) & 0x01);
            bleClient.onUserBytesReceived(userBytes);
            v1ProfileManager.setCurrentSettings(userBytes);
            DEBUG_LOGLN("Received V1 user bytes!");
            rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);
            continue;  // Don't pass to parser
        }

        // ALWAYS erase packet from buffer after attempting to parse
        // This prevents stale packets from accumulating when display updates are throttled
        bool parseOk = parser.parse(packetPtr, packetSize);

        // Debug: dump display/alert packets for capture/replay (throttled to 1 Hz per ID)
        if (packetPtr[3] == PACKET_ID_DISPLAY_DATA || packetPtr[3] == PACKET_ID_ALERT_DATA) {
            static unsigned long lastDump31 = 0;
            static unsigned long lastDump43 = 0;
            unsigned long dumpNow = millis();
            if ((packetPtr[3] == PACKET_ID_DISPLAY_DATA && dumpNow - lastDump31 > 1000) ||
                (packetPtr[3] == PACKET_ID_ALERT_DATA && dumpNow - lastDump43 > 1000)) {
                dumpPacket(packetPtr, packetSize);
                if (packetPtr[3] == PACKET_ID_DISPLAY_DATA) lastDump31 = dumpNow;
                else lastDump43 = dumpNow;
            }
        }
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);
        
        if (parseOk) {
            if (isColorPreviewActive()) {
                continue;  // Keep preview on-screen briefly
            }
            DisplayState state = parser.getDisplayState();

            // Cache alert status to avoid repeated calls
            bool hasAlerts = parser.hasAlerts();

            // Recovery: display shows bands but no alert table arrived
            if (!hasAlerts && state.activeBands != BAND_NONE) {
                unsigned long gapNow = millis();
                if (gapNow - lastAlertGapRecoverMs > 100) {  // 100ms - quick recovery for lost alert packets
                    DEBUG_LOGLN("Alert gap: bands active but no alerts; re-requesting alert data");
                    parser.resetAlertAssembly();
                    bleClient.requestAlertData();
                    lastAlertGapRecoverMs = gapNow;
                }
            }

            // V1 is source of truth for mute state
            // Mute debounce: prevent flicker from rapid V1 state changes
            unsigned long muteNow = millis();
            if (state.muted != debouncedMuteState) {
                if (muteNow - lastMuteChangeMs > MUTE_DEBOUNCE_MS) {
                    debouncedMuteState = state.muted;
                    lastMuteChangeMs = muteNow;
                } else {
                    // Within debounce window - keep previous state
                    state.muted = debouncedMuteState;
                }
            }
            
            // Throttle display updates to prevent crashes
            unsigned long now = millis();
            if (now - lastDisplayDraw < DISPLAY_DRAW_MIN_MS) {
                continue; // Skip this draw
            }
            lastDisplayDraw = now;
            
            if (hasAlerts) {
                AlertData priority = parser.getPriorityAlert();
                int alertCount = parser.getAlertCount();
                uint8_t currentStrength = std::max(priority.frontStrength, priority.rearStrength);
                const auto& currentAlerts = parser.getAllAlerts();
                
                displayMode = DisplayMode::LIVE;

                // Voice alerts: announce new priority alert when no phone app connected
                // Skip if alert is muted on V1 - user has already acknowledged/dismissed it
                const V1Settings& settings = settingsManager.get();
                bool muteForVolZero = settings.muteVoiceIfVolZero && state.mainVolume == 0;
                if (settings.voiceAlertsEnabled && 
                    !muteForVolZero &&
                    !state.muted &&  // Don't announce muted alerts
                    !bleClient.isProxyClientConnected() &&
                    priority.isValid &&
                    priority.band != BAND_NONE) {
                    
                    unsigned long now = millis();
                    bool bandChanged = (priority.band != lastVoiceAlertBand);
                    bool directionChanged = (priority.direction != lastVoiceAlertDirection);
                    bool cooldownPassed = (now - lastVoiceAlertTime >= VOICE_ALERT_COOLDOWN_MS);
                    
                    // Announce when band or direction changes (with cooldown to prevent spam)
                    if ((bandChanged || directionChanged) && cooldownPassed) {
                        // Convert V1 band to audio band
                        AlertBand audioBand;
                        bool validBand = true;
                        switch (priority.band) {
                            case BAND_LASER: audioBand = AlertBand::LASER; break;
                            case BAND_KA:    audioBand = AlertBand::KA;    break;
                            case BAND_K:     audioBand = AlertBand::K;     break;
                            case BAND_X:     audioBand = AlertBand::X;     break;
                            default:         validBand = false;            break;
                        }
                        
                        // Convert V1 direction to audio direction
                        // V1 uses bitmask (FRONT=1, SIDE=2, REAR=4), we simplify to primary direction
                        AlertDirection audioDir;
                        if (priority.direction & DIR_FRONT) {
                            audioDir = AlertDirection::AHEAD;
                        } else if (priority.direction & DIR_REAR) {
                            audioDir = AlertDirection::BEHIND;
                        } else {
                            audioDir = AlertDirection::SIDE;
                        }
                        
                        if (validBand) {
                            DEBUG_LOGF("[VoiceAlert] Announcing: band=%d freq=%lu dir=%d\n", 
                                       (int)audioBand, priority.frequency, (int)audioDir);
                            // Use frequency voice if available (SD card), falls back to simple alert
                            play_frequency_voice(audioBand, (uint16_t)priority.frequency, audioDir);
                            lastVoiceAlertBand = priority.band;
                            lastVoiceAlertDirection = priority.direction;
                            lastVoiceAlertTime = millis();
                        }
                    }
                }

                // V1 is source of truth for mute state - no auto-unmute logic
                // Display just shows what V1 reports

                // Update display FIRST for lowest latency
                // Pass all alerts for multi-alert card display
                display.update(priority, currentAlerts.data(), alertCount, state);
                
                // Save priority alert for potential persistence when alert clears
                persistedAlert = priority;
                alertPersistenceActive = false;  // Cancel any active persistence
                alertClearedTime = 0;
                
            } else {
                // No alerts from V1
                displayMode = DisplayMode::IDLE;
                
                // Reset voice alert tracking when alerts clear
                lastVoiceAlertBand = BAND_NONE;
                lastVoiceAlertDirection = DIR_NONE;
                
                // Alert persistence: show last alert in grey for configured duration
                const V1Settings& s = settingsManager.get();
                uint8_t persistSec = settingsManager.getSlotAlertPersistSec(s.activeSlot);
                unsigned long now = millis();
                
                // Clear persistence on profile slot change (handles web UI profile switches)
                static int lastPersistenceSlot = -1;
                if (s.activeSlot != lastPersistenceSlot) {
                    lastPersistenceSlot = s.activeSlot;
                    persistedAlert = AlertData();
                    alertPersistenceActive = false;
                    alertClearedTime = 0;
                }
                
                if (persistSec > 0 && persistedAlert.isValid) {
                    // Start persistence timer on transition from alerts to no-alerts
                    if (alertClearedTime == 0) {
                        alertClearedTime = now;
                        alertPersistenceActive = true;
                    }
                    
                    // Check if persistence timer still active
                    unsigned long persistMs = persistSec * 1000UL;
                    if (alertPersistenceActive && (now - alertClearedTime) < persistMs) {
                        // Show persisted alert in dark grey
                        display.updatePersisted(persistedAlert, state);
                    } else {
                        // Persistence expired - show normal resting
                        if (alertPersistenceActive) {
                            alertPersistenceActive = false;
                        }
                        display.update(state);
                    }
                } else {
                    // Persistence disabled or no valid persisted alert
                    alertPersistenceActive = false;
                    alertClearedTime = 0;
                    display.update(state);
                }
            }
        }
    }
}

void setup() {
    // Wait for USB to stabilize after upload
    delay(100);
    
    // Create BLE data queue early - before any BLE operations
    bleDataQueue = xQueueCreate(64, sizeof(BLEDataPacket));  // Increased from 32 to handle web server blocking
    rxBuffer.reserve(256);  // Trimmed at 256 bytes anyway; normal packets <100 bytes
    
// Backlight is handled in display.begin() (inverted PWM for Waveshare)

#if defined(PIN_POWER_ON) && PIN_POWER_ON >= 0
    // Cut panel power until we intentionally bring it up
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, LOW);
#endif

    Serial.begin(115200);
    delay(200);  // Reduced from 500ms - brief delay for serial init
    
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
        while (1) delay(1000);
    }
    
    // Display battery status after display is initialized
#if defined(DISPLAY_WAVESHARE_349)
    SerialLog.printf("[Battery] Power source: %s\n", 
                     batteryManager.isOnBattery() ? "BATTERY" : "USB");
    SerialLog.printf("[Battery] Icon display: %s\n",
                     batteryManager.hasBattery() ? "YES" : "NO");
    if (batteryManager.hasBattery()) {
        SerialLog.printf("[Battery] Voltage: %dmV (%d%%)\n", 
                      batteryManager.getVoltageMillivolts(), 
                      batteryManager.getPercentage());
    }
#endif

    // Brief delay to ensure panel is fully cleared before enabling backlight
    delay(100);

    // Initialize settings BEFORE showing any styled screens (need displayStyle setting)
    settingsManager.begin();

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
    
    // If you want to show the demo, call display.showDemo() manually elsewhere (e.g., via a button or menu)
    
    lastLvTick = millis();

    // Mount storage (SD if available, else LittleFS) for profiles and settings
    SerialLog.println("[Setup] Mounting storage...");
    if (storageManager.begin()) {
        SerialLog.printf("[Setup] Storage ready: %s\n", storageManager.statusText().c_str());
        v1ProfileManager.begin(storageManager.getFilesystem());
        audio_init_sd();  // Initialize SD-based frequency voice audio
    } else {
        SerialLog.println("[Setup] Storage unavailable - profiles will be disabled");
    }

    SerialLog.println("==============================");
    SerialLog.println("WiFi Configuration:");
    SerialLog.printf("  enableWifi: %s\n", settingsManager.get().enableWifi ? "YES" : "NO");
    SerialLog.printf("  wifiMode: %d\n", settingsManager.get().wifiMode);
    SerialLog.printf("  apSSID: %s\n", settingsManager.get().apSSID.c_str());
    SerialLog.println("==============================");
    
    // WiFi is off by default; long-press BOOT (~2s) to start the AP when needed
    SerialLog.println("[WiFi] Off by default - start with BOOT long-press");
    
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
    brightnessAdjustValue = settingsManager.get().brightness;
    volumeAdjustValue = settingsManager.get().voiceVolume;
    display.setBrightness(brightnessAdjustValue);  // Apply saved brightness
    audio_set_volume(volumeAdjustValue);           // Apply saved voice volume
    SerialLog.printf("[Settings] Applied saved brightness: %d, voice volume: %d\n", 
                    brightnessAdjustValue, volumeAdjustValue);
#endif

#ifndef REPLAY_MODE
    // Initialize BLE client with proxy settings from preferences
    const V1Settings& bleSettings = settingsManager.get();
    SerialLog.printf("Starting BLE (proxy: %s, name: %s)\n", 
                  bleSettings.proxyBLE ? "enabled" : "disabled",
                  bleSettings.proxyName.c_str());

    // Initialize BLE stack first (required before any BLE operations)
    if (!bleClient.initBLE(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
        SerialLog.println("BLE initialization failed!");
        display.showDisconnected();
        while (1) delay(1000);
    }
    
    // Start normal scanning
    SerialLog.println("Starting BLE scan for V1...");
    if (!bleClient.begin(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
        SerialLog.println("BLE scan failed to start!");
        display.showDisconnected();
        while (1) delay(1000);
    }
    
    // Register data callback
    bleClient.onDataReceived(onV1Data);
    
    // Register V1 connection callback for auto-push
    bleClient.onV1Connected(onV1Connected);
#else
    SerialLog.println("[REPLAY_MODE] BLE disabled - using packet replay for UI testing");
#endif
    
    SerialLog.println("Setup complete - BLE scanning, WiFi off until BOOT long-press");
}

void loop() {
    // Update BLE indicator: show when V1 is connected; color reflects JBV1 connection
    display.setBLEProxyStatus(bleClient.isConnected(), bleClient.isProxyClientConnected());

    // Drive color preview (band cycle) first; skip other updates if active
    if (colorPreviewActive) {
        driveColorPreview();
    } else if (colorPreviewEnded) {
        // Preview finished - restore resting UI so it doesn't stick
        colorPreviewEnded = false;
        display.showResting();
    }

    // Process battery manager (updates cached readings at 1Hz, handles power button)
#if defined(DISPLAY_WAVESHARE_349)
    batteryManager.update();
    batteryManager.processPowerButton();
    
    // Check for critical battery - auto shutdown to prevent damage
    static bool lowBatteryWarningShown = false;
    static unsigned long criticalBatteryTime = 0;
    
    if (batteryManager.isOnBattery() && batteryManager.hasBattery()) {
        if (batteryManager.isCritical()) {
            // Show warning once, then shutdown after 5 seconds
            if (!lowBatteryWarningShown) {
                Serial.println("[Battery] CRITICAL - showing low battery warning");
                display.showLowBattery();
                lowBatteryWarningShown = true;
                criticalBatteryTime = millis();
            } else if (millis() - criticalBatteryTime > 5000) {
                Serial.println("[Battery] CRITICAL - auto shutdown to protect battery");
                batteryManager.powerOff();
            }
        } else {
            lowBatteryWarningShown = false;  // Reset if voltage recovers
        }
    }
    
    // BOOT button handling: short press = brightness adjust, long press = AP toggle
    bool bootPressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);  // Active low
    unsigned long now = millis();

    if (bootPressed && !bootWasPressed) {
        bootPressStart = now;
        wifiToggleTriggered = false;  // Reset for new press
    }

    // Check for long press while button is still held (immediate feedback)
    if (bootPressed && !wifiToggleTriggered && !brightnessAdjustMode) {
        unsigned long pressDuration = now - bootPressStart;
        if (pressDuration >= AP_TOGGLE_LONG_PRESS_MS) {
            wifiToggleTriggered = true;  // Prevent re-triggering
            bool wasActive = wifiManager.isSetupModeActive();
            if (wasActive) {
                wifiManager.stopSetupMode(true);
                SerialLog.println("[WiFi] AP stopped (button long press)");
            } else {
                startWifi();
                SerialLog.println("[WiFi] AP started (button long press)");
            }
            display.drawWiFiIndicator();
            display.flush();
        }
    }

    if (!bootPressed && bootWasPressed) {
        unsigned long pressDuration = now - bootPressStart;
        if (pressDuration >= BOOT_DEBOUNCE_MS && !wifiToggleTriggered) {
            // Only handle short press actions (not already handled by long press)
            if (brightnessAdjustMode) {
                // Exit settings adjustment mode and save both values
                brightnessAdjustMode = false;
                settingsManager.updateBrightness(brightnessAdjustValue);
                settingsManager.updateVoiceVolume(volumeAdjustValue);
                settingsManager.save();
                audio_set_volume(volumeAdjustValue);  // Apply new volume
                display.hideBrightnessSlider();
                display.showResting();  // Return to normal display
                SerialLog.printf("[Settings] Saved brightness: %d, volume: %d\n", brightnessAdjustValue, volumeAdjustValue);
            } else {
                // Enter settings adjustment mode (short press)
                brightnessAdjustMode = true;
                brightnessAdjustValue = settingsManager.get().brightness;
                volumeAdjustValue = settingsManager.get().voiceVolume;
                activeSlider = 0;  // Start with brightness selected
                display.showSettingsSliders(brightnessAdjustValue, volumeAdjustValue);
                SerialLog.printf("[Settings] Entering adjustment mode (brightness: %d, volume: %d)\n", 
                                brightnessAdjustValue, volumeAdjustValue);
            }
        }
    }

    bootWasPressed = bootPressed;
    
    // If in settings adjustment mode, handle touch for both sliders
    if (brightnessAdjustMode) {
        int16_t touchX, touchY;
        if (touchHandler.getTouchPoint(touchX, touchY)) {
            // Map touch X to slider value (40 to 600 pixels = slider area)
            // Note: Touch coordinates are inverted relative to display
            const int sliderX = 40;
            const int sliderWidth = SCREEN_WIDTH - 80;  // 560 pixels
            
            // Determine which slider was touched based on Y coordinate
            int touchedSlider = display.getActiveSliderFromTouch(touchY);
            
            if (touchedSlider >= 0 && touchX >= sliderX && touchX <= sliderX + sliderWidth) {
                activeSlider = touchedSlider;
                
                if (activeSlider == 0) {
                    // Brightness slider (range 80-255)
                    // Touch X is inverted: swipe right = lower X, swipe left = higher X
                    int newLevel = 255 - (((touchX - sliderX) * 175) / sliderWidth);
                    if (newLevel < 80) newLevel = 80;
                    if (newLevel > 255) newLevel = 255;
                    
                    if (newLevel != brightnessAdjustValue) {
                        brightnessAdjustValue = newLevel;
                        display.updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
                    }
                } else if (activeSlider == 1) {
                    // Voice volume slider (range 0-100)
                    int newVolume = 100 - (((touchX - sliderX) * 100) / sliderWidth);
                    if (newVolume < 0) newVolume = 0;
                    if (newVolume > 100) newVolume = 100;
                    
                    if (newVolume != volumeAdjustValue) {
                        volumeAdjustValue = newVolume;
                        audio_set_volume(volumeAdjustValue);  // Apply immediately for feedback
                        display.updateSettingsSliders(brightnessAdjustValue, volumeAdjustValue, activeSlider);
                        lastVolumeChangeMs = now;  // Track for test playback
                    }
                }
            }
        } else {
            // No touch - check if volume was recently changed and play test voice
            if (lastVolumeChangeMs > 0 && (now - lastVolumeChangeMs) >= VOLUME_TEST_DEBOUNCE_MS) {
                play_test_voice();
                lastVolumeChangeMs = 0;  // Reset so we don't play again
            }
        }
        return;  // Skip normal loop processing while in settings mode
    }
#endif

    // Check for touch - single tap for mute (only with active alert), triple-tap for profile cycle (only without alert)
    int16_t touchX, touchY;
    bool hasActiveAlert = parser.hasAlerts();

    // Helper to toggle mute state - sends command to V1
    // V1 is source of truth - display updates when V1 reports new state
    auto performMuteToggle = [&](const char* reason) {
        if (!hasActiveAlert) {
            SerialLog.println("MUTE BLOCKED: No active alert to mute");
            return;
        }

        DisplayState state = parser.getDisplayState();
        bool currentMuted = state.muted;
        bool newMuted = !currentMuted;

        SerialLog.printf("Mute: %s -> Sending: %s (%s)\n", 
                      currentMuted ? "MUTED" : "UNMUTED",
                      newMuted ? "MUTE_ON" : "MUTE_OFF",
                      reason);

        // Send mute command to V1
        bool cmdSent = bleClient.setMute(newMuted);
        SerialLog.printf("Mute command sent: %s\n", cmdSent ? "OK" : "FAIL");
    };
    
    if (touchHandler.getTouchPoint(touchX, touchY)) {
        unsigned long now = millis();
        
        // Debounce check
        if (now - lastTapTime >= TAP_DEBOUNCE_MS) {
            // Check if this tap is within the window of previous taps
            if (now - lastTapTime <= TAP_WINDOW_MS) {
                tapCount++;
            } else {
                // Window expired, start new count
                tapCount = 1;
            }
            lastTapTime = now;
            
            SerialLog.printf("Tap detected: count=%d, x=%d, y=%d, hasAlert=%d\n", tapCount, touchX, touchY, hasActiveAlert);

            // With an active alert, toggle mute immediately on first tap for responsiveness
            if (hasActiveAlert && tapCount == 1) {
                performMuteToggle("immediate tap");
                tapCount = 0;  // reset so follow-up taps start fresh
                return;        // skip triple-tap handling
            }
            
            // Check for triple-tap to cycle profiles (ONLY when no active alert)
            if (tapCount >= 3) {
                tapCount = 0;  // Reset tap count
                
                if (hasActiveAlert) {
                    SerialLog.println("PROFILE CHANGE BLOCKED: Active alert present - tap to mute instead");
                } else {
                    // Cycle to next profile slot: 0 -> 1 -> 2 -> 0
                    const V1Settings& s = settingsManager.get();
                    int newSlot = (s.activeSlot + 1) % 3;
                    settingsManager.setActiveSlot(newSlot);
                    displayMode = DisplayMode::IDLE;
                    
                    // Clear persisted alert state on profile change
                    persistedAlert = AlertData();
                    alertPersistenceActive = false;
                    
                    const char* slotNames[] = {"Default", "Highway", "Comfort"};
                    SerialLog.printf("PROFILE CHANGE: Switched to '%s' (slot %d)\n", slotNames[newSlot], newSlot);
                    
                    // Update display to show new profile
                    display.drawProfileIndicator(newSlot);
                    
                    // If connected to V1 and auto-push is enabled, push the new profile
                    // Note: Use startAutoPush directly to avoid device-specific override
                    if (bleClient.isConnected() && s.autoPushEnabled) {
                        SerialLog.println("Pushing new profile to V1...");
                        startAutoPush(newSlot);
                    }
                }
            }
        }
    } else {
        // No touch - check if we have a pending single/double tap to process as mute toggle
        unsigned long now = millis();
        if (tapCount > 0 && tapCount < 3 && (now - lastTapTime > TAP_WINDOW_MS)) {
            // Window expired with 1-2 taps - treat as mute toggle (ONLY with active alert)
            SerialLog.printf("Processing %d tap(s) as mute toggle\n", tapCount);
            tapCount = 0;
            performMuteToggle("deferred tap");
        }
    }
    
#ifndef REPLAY_MODE
    // Process BLE events
    bleClient.process();
#endif
    
    // Process queued BLE data (safe for SPI - runs in main loop context)
    // In REPLAY_MODE, this injects test packets; otherwise processes BLE queue
    processBLEData();

    // Drive auto-push state machine (non-blocking)
    processAutoPush();

    // Process WiFi/web server
    wifiManager.process();
    
    // Update display periodically
    now = millis();
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
        lastDisplayUpdate = now;
        if (!isColorPreviewActive()) {
            // Check connection status
            static bool wasConnected = false;
            bool isConnected = bleClient.isConnected();
            
            // Only trigger state changes on actual transitions
            if (isConnected != wasConnected) {
                if (isConnected) {
                    display.showResting(); // stay on resting view until data arrives
                    SerialLog.println("V1 connected!");
                } else {
                    // Reset stale state from previous connection
                    PacketParser::resetPriorityState();
                    PacketParser::resetSignalBarDecay();
                    V1Display::resetChangeTracking();
                    display.showScanning();
                    SerialLog.println("V1 disconnected - Scanning...");
                    displayMode = DisplayMode::IDLE;
                }
                wasConnected = isConnected;
            }

            // If connected but not seeing traffic, re-request alert data periodically
            static unsigned long lastReq = 0;
            if (isConnected && (now - lastRxMillis) > 2000 && (now - lastReq) > 1000) {
                DEBUG_LOGLN("No data recently; re-requesting alert data...");
                bleClient.requestAlertData();
                lastReq = now;
            }

            // Periodically refresh indicators (WiFi/battery) even when scanning
            if (!isConnected) {
                display.drawWiFiIndicator();
                display.drawBatteryIndicator();
                display.flush();  // Push canvas changes to physical display
            }
        }
    }
    
    // Status update (print to serial) - only when DEBUG_LOGS enabled
    if (DEBUG_LOGS && now - lastStatusUpdate >= STATUS_UPDATE_MS) {
        lastStatusUpdate = now;
        
        if (bleClient.isConnected()) {
            DisplayState state = parser.getDisplayState();
            if (parser.hasAlerts()) {
                SerialLog.printf("Active alerts: %d\n", parser.getAlertCount());
            }
        }
    }

    // Short FreeRTOS delay to yield CPU without capping loop at ~200 Hz
    vTaskDelay(pdMS_TO_TICKS(1));
}
