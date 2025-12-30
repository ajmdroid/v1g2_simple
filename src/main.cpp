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
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "wifi_manager.h"
#include "settings.h"
#include "alert_logger.h"
#include "serial_logger.h"
#include "time_manager.h"
#include "alert_db.h"
#include "touch_handler.h"
#include "v1_profiles.h"
#include "battery_manager.h"
#include "../include/config.h"
#include <vector>
#include <algorithm>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// Global objects
V1BLEClient bleClient;
PacketParser parser;
V1Display display;
TouchHandler touchHandler;

// Queue for BLE data - decouples BLE callbacks from display updates
static QueueHandle_t bleDataQueue = nullptr;
struct BLEDataPacket {
    uint8_t data[256];  // Max expected packet size
    size_t length;
    uint16_t charUUID;  // Last 16-bit of characteristic UUID to identify source
};

unsigned long lastDisplayUpdate = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastLvTick = 0;
unsigned long lastRxMillis = 0;
unsigned long lastDisplayDraw = 0;  // Throttle display updates
const unsigned long DISPLAY_DRAW_MIN_MS = 33;  // Min 33ms between draws (~30fps)

// Local mute override - takes immediate effect on tap before V1 confirms
static bool localMuteOverride = false;
static bool localMuteActive = false;
static unsigned long localMuteTimestamp = 0;
static unsigned long unmuteSentTimestamp = 0;  // Track when we sent unmute to V1
const unsigned long LOCAL_MUTE_TIMEOUT_MS = 2000;  // Clear override 2s after alert ends
const unsigned long UNMUTE_GRACE_MS = 1000;  // Force unmuted state for 1s after sending unmute command

// Track muted alert to detect stronger signals
static uint8_t mutedAlertStrength = 0;
static Band mutedAlertBand = BAND_NONE;
static uint32_t mutedAlertFreq = 0;

// Triple-tap detection for profile cycling
static unsigned long lastTapTime = 0;
static int tapCount = 0;
const unsigned long TAP_WINDOW_MS = 600;  // Window for 3 taps
const unsigned long TAP_DEBOUNCE_MS = 150; // Minimum time between taps


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
        // Non-blocking send to queue - if queue is full, drop the packet
        BaseType_t result = xQueueSend(bleDataQueue, &pkt, 0);
        if (result != pdTRUE) {
            // Queue full - data dropped (logged for debugging)
            static unsigned long lastQueueFullLog = 0;
            unsigned long now = millis();
            if (now - lastQueueFullLog > 1000) {
                SerialLog.println("WARNING: BLE queue full, dropping packets!");
                lastQueueFullLog = now;
            }
        }
    }
}

static void startAutoPush(int slotIndex) {
    static const char* slotNames[] = {"Default", "Highway", "Passenger Comfort"};
    int clampedIndex = std::max(0, std::min(2, slotIndex));
    autoPushState.slotIndex = clampedIndex;
    autoPushState.slot = settingsManager.getActiveSlot();
    autoPushState.profileLoaded = false;
    autoPushState.profile = V1Profile();
    autoPushState.step = AUTO_PUSH_STEP_WAIT_READY;
    autoPushState.nextStepAtMs = millis() + 500;
    SerialLog.printf("[AutoPush] V1 connected - applying '%s' profile (slot %d)...\n",
                     slotNames[clampedIndex], clampedIndex);
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
                SerialLog.printf("[AutoPush] Loading profile: %s\n", slot.profileName.c_str());
                V1Profile profile;
                if (v1ProfileManager.loadProfile(slot.profileName, profile)) {
                    autoPushState.profile = profile;
                    autoPushState.profileLoaded = true;
                    if (bleClient.writeUserBytes(profile.settings.bytes)) {
                        SerialLog.println("[AutoPush] Profile settings pushed successfully");
                    } else {
                        SerialLog.println("[AutoPush] ERROR: Failed to push profile settings");
                    }
                } else {
                    SerialLog.printf("[AutoPush] ERROR: Failed to load profile '%s'\n", slot.profileName.c_str());
                    autoPushState.profileLoaded = false;
                }
            } else {
                SerialLog.println("[AutoPush] No profile configured for active slot");
                autoPushState.profileLoaded = false;
            }

            if (autoPushState.profileLoaded) {
                autoPushState.step = AUTO_PUSH_STEP_DISPLAY;
                autoPushState.nextStepAtMs = now + 100;
            } else {
                autoPushState.step = AUTO_PUSH_STEP_MODE;
                autoPushState.nextStepAtMs = now + (autoPushState.slot.mode != V1_MODE_UNKNOWN ? 100 : 0);
            }
            return;
        }

        case AUTO_PUSH_STEP_DISPLAY: {
            bleClient.setDisplayOn(autoPushState.profile.displayOn);
            SerialLog.printf("[AutoPush] Display set to: %s\n",
                             autoPushState.profile.displayOn ? "ON" : "OFF");
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
                    SerialLog.printf("[AutoPush] Mode set to: %s\n", modeName);
                } else {
                    SerialLog.println("[AutoPush] ERROR: Failed to set mode");
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
                    SerialLog.printf("[AutoPush] Volume set - main: %d, muted: %d\n", mainVol, muteVol);
                } else {
                    SerialLog.println("[AutoPush] ERROR: Failed to set volume");
                }
            }

            SerialLog.println("[AutoPush] Complete");
            autoPushState.step = AUTO_PUSH_STEP_IDLE;
            autoPushState.nextStepAtMs = 0;
            return;
        }

        default:
            autoPushState.step = AUTO_PUSH_STEP_IDLE;
            return;
    }
}

// Callback when V1 connection is fully established
// Handles auto-push of default profile and mode
void onV1Connected() {
    const V1Settings& s = settingsManager.get();
    int activeSlotIndex = std::max(0, std::min(2, s.activeSlot));
    if (activeSlotIndex != s.activeSlot) {
        SerialLog.printf("[AutoPush] WARNING: activeSlot out of range (%d). Using slot %d instead.\n",
                      s.activeSlot, activeSlotIndex);
    }
    
    // Save this V1's address to SD card cache if not already present
    // Also check for device-specific default profile
    int deviceDefaultSlot = -1;  // -1 means use global setting
    
    if (alertLogger.isReady()) {
        String connectedAddr = bleClient.getConnectedAddress().toString().c_str();
        if (connectedAddr.length() == 17) {
            fs::FS* fs = alertLogger.getFilesystem();
            
            // Check if address already exists in known_v1.txt
            bool addressExists = false;
            File file = fs->open("/known_v1.txt", FILE_READ);
            if (file) {
                while (file.available()) {
                    String line = file.readStringUntil('\n');
                    line.trim();
                    if (line == connectedAddr) {
                        addressExists = true;
                        break;
                    }
                }
                file.close();
            }
            
            // Append if new
            if (!addressExists) {
                File file = fs->open("/known_v1.txt", FILE_APPEND);
                if (file) {
                    file.println(connectedAddr);
                    file.close();
                    SerialLog.printf("[V1Cache] Added new V1 address: %s\n", connectedAddr.c_str());
                } else {
                    SerialLog.println("[V1Cache] Failed to open known_v1.txt for writing");
                }
            }
            
            // Check for device-specific default profile in known_v1_profiles.txt
            File profileFile = fs->open("/known_v1_profiles.txt", FILE_READ);
            if (profileFile) {
                while (profileFile.available()) {
                    String line = profileFile.readStringUntil('\n');
                    line.trim();
                    int sep = line.indexOf('|');
                    if (sep > 0) {
                        String addr = line.substring(0, sep);
                        if (addr == connectedAddr) {
                            deviceDefaultSlot = line.substring(sep + 1).toInt();
                            SerialLog.printf("[AutoPush] Found device-specific profile: slot %d\n", deviceDefaultSlot);
                            break;
                        }
                    }
                }
                profileFile.close();
            }
        }
    }
    
    if (!s.autoPushEnabled) {
        SerialLog.println("[AutoPush] Disabled, skipping");
        return;
    }

    // Use device-specific slot if set (1-3 in file, convert to 0-2 index), otherwise global
    int slotToUse = activeSlotIndex;
    if (deviceDefaultSlot >= 1 && deviceDefaultSlot <= 3) {
        slotToUse = deviceDefaultSlot - 1;  // Convert 1-3 to 0-2 index
        SerialLog.printf("[AutoPush] Using device-specific slot %d (index %d)\n", deviceDefaultSlot, slotToUse);
    } else {
        SerialLog.printf("[AutoPush] Using global active slot %d\n", activeSlotIndex + 1);
    }

    startAutoPush(slotToUse);
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
    BLEDataPacket pkt;
    
    // Process all queued packets
    while (xQueueReceive(bleDataQueue, &pkt, 0) == pdTRUE) {
        // Forward raw data to proxy clients (JBV1) - done here in main loop to avoid SPI conflicts
        // Pass the source characteristic UUID so data is forwarded to the correct proxy characteristic
        bleClient.forwardToProxy(pkt.data, pkt.length, pkt.charUUID);
        
        // Accumulate and frame on 0xAA ... 0xAB so we don't choke on chunked notifications
        rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
    }
#endif
    
    // If no data accumulated, return
    if (rxBuffer.empty()) {
        return;
    }

    // Trim runaway buffers
    if (rxBuffer.size() > 512) {
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() - 256);
    }

    const size_t MIN_HEADER_SIZE = 6;
    const size_t MAX_PACKET_SIZE = 512;

    while (true) {
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

        // Check for user bytes response (0x12) - V1 settings pull
        if (packetSize >= 12 && packetPtr[3] == PACKET_ID_RESP_USER_BYTES) {
            // Payload starts at byte 5, length is 6 bytes
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            SerialLog.printf("V1 user bytes raw: %02X %02X %02X %02X %02X %02X\n",
                userBytes[0], userBytes[1], userBytes[2], userBytes[3], userBytes[4], userBytes[5]);
            SerialLog.printf("  xBand=%d, kBand=%d, kaBand=%d, laser=%d\n",
                userBytes[0] & 0x01, (userBytes[0] >> 1) & 0x01, 
                (userBytes[0] >> 2) & 0x01, (userBytes[0] >> 3) & 0x01);
            v1ProfileManager.setCurrentSettings(userBytes);
            SerialLog.println("Received V1 user bytes!");
            rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);
            continue;  // Don't pass to parser
        }

        // ALWAYS erase packet from buffer after attempting to parse
        // This prevents stale packets from accumulating when display updates are throttled
        bool parseOk = parser.parse(packetPtr, packetSize);
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);
        
        if (parseOk) {
            DisplayState state = parser.getDisplayState();

            // Cache alert status to avoid repeated calls
            bool hasAlerts = parser.hasAlerts();

            // DEBUG: Log raw V1 mute state vs local state (for flicker debugging)
            static bool lastLoggedMuted = false;
            static bool lastV1Muted = false;
            bool v1MutedRaw = state.muted;  // Capture V1's raw mute state before overrides
            
            // Apply local mute override IMMEDIATELY - lock it in before any logic
            if (localMuteActive && localMuteOverride) {
                state.muted = true;
            }
            
            // If we recently sent unmute command, force unmuted until V1 catches up
            if (unmuteSentTimestamp > 0) {
                unsigned long timeSinceUnmute = millis() - unmuteSentTimestamp;
                if (timeSinceUnmute < UNMUTE_GRACE_MS) {
                    state.muted = false;  // Override V1's lagging muted state
                } else {
                    unmuteSentTimestamp = 0;  // Grace period expired, trust V1 again
                }
            }
            
            // Track mute state changes (no logging in hot path)
            if (state.muted != lastLoggedMuted || v1MutedRaw != lastV1Muted) {
                lastLoggedMuted = state.muted;
                lastV1Muted = v1MutedRaw;
            }

            // Handle timeout logic separately
            if (localMuteActive) {
                // While alerts are active, ALWAYS keep override (no timeout)
                // Timeout only applies when no alerts (waiting for V1 response)
                if (hasAlerts) {
                    // Reset timestamp so we get fresh timeout window when alert goes away
                    localMuteTimestamp = millis();
                } else {
                    // No alerts - check timeout
                    unsigned long now = millis();
                    if (now - localMuteTimestamp >= LOCAL_MUTE_TIMEOUT_MS) {
                        // Timeout and no alerts - clear override and send unmute
                        SerialLog.println("Local mute override timed out (no alerts) - sending unmute to V1");
                        localMuteActive = false;
                        localMuteOverride = false;
                        mutedAlertStrength = 0;
                        mutedAlertBand = BAND_NONE;
                        state.muted = false;  // Clear muted state on timeout
                        bleClient.setMute(false);
                        unmuteSentTimestamp = millis();  // Track when we sent unmute
                    }
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

                // Auto-unmute logic: new stronger signal or different band
                if (localMuteActive && localMuteOverride) {
                    bool strongerSignal = false;
                    bool differentAlert = false;
                    bool higherPriorityBand = false;

                    // Check if it's a different band first
                    if (priority.band != mutedAlertBand && priority.band != BAND_NONE) {
                        differentAlert = true;
                        SerialLog.printf("Different band detected: %d -> %d\n", mutedAlertBand, priority.band);
                    }

                    // Check for higher priority band (Ka > K > X, Laser is special)
                    // If muted K and Ka shows up (even weak) -> unmute
                    // If muted X and K or Ka shows up -> unmute
                    if (mutedAlertBand == BAND_K && priority.band == BAND_KA) {
                        higherPriorityBand = true;
                        SerialLog.println("Higher priority band: K muted, Ka detected");
                    } else if (mutedAlertBand == BAND_X && (priority.band == BAND_KA || priority.band == BAND_K)) {
                        higherPriorityBand = true;
                        SerialLog.printf("Higher priority band: X muted, %s detected\n", 
                                    priority.band == BAND_KA ? "Ka" : "K");
                    } else if (mutedAlertBand == BAND_LASER && priority.band != BAND_LASER && priority.band != BAND_NONE) {
                        higherPriorityBand = true;
                        SerialLog.printf("Radar band after Laser: %d\n", priority.band);
                    }

                    // Only check stronger signal if band changed - same band getting stronger is not a new threat
                    // (e.g., sweeping laser or approaching radar should stay muted)
                    if (priority.band != mutedAlertBand && currentStrength >= mutedAlertStrength + 2) {
                        strongerSignal = true;
                        SerialLog.printf("Stronger signal on different band: %d -> %d\n", mutedAlertStrength, currentStrength);
                    }

                    // Check if it's a different frequency (for same band, >15 MHz difference)
                    // Only applies to radar bands (laser freq is always 0)
                    if (priority.band == mutedAlertBand && priority.frequency > 0 && mutedAlertFreq > 0) {
                        uint32_t freqDiff = (priority.frequency > mutedAlertFreq) ? 
                                           (priority.frequency - mutedAlertFreq) : 
                                           (mutedAlertFreq - priority.frequency);
                        if (freqDiff > 50) {  // More than 50 MHz different
                            differentAlert = true;
                            SerialLog.printf("Different frequency: %lu -> %lu (diff: %lu)\n", 
                                        mutedAlertFreq, priority.frequency, freqDiff);
                        }
                    }

                    // Auto-unmute for stronger, different, or higher priority alert
                    if (strongerSignal || differentAlert || higherPriorityBand) {
                        SerialLog.println("Auto-unmuting for new/stronger/priority alert");
                        localMuteActive = false;
                        localMuteOverride = false;
                        state.muted = false;
                        mutedAlertStrength = 0;
                        mutedAlertBand = BAND_NONE;
                        mutedAlertFreq = 0;
                        if (bleClient.setMute(false)) {
                            unmuteSentTimestamp = millis();
                        } else {
                            SerialLog.println("Auto-unmute failed to send MUTE_OFF");
                        }
                    }
                }

                // Update display FIRST for lowest latency
                display.update(priority, state, alertCount);
                
                // Logging happens after display update (lower priority than visual feedback)
                time_t now = time(nullptr);
                if (now > 1609459200) {  // Valid if after 2021-01-01
                    alertLogger.setTimestampUTC((uint32_t)now);
                    alertDB.setTimestampUTC((uint32_t)now);
                }
                alertLogger.logAlert(priority, state, alertCount);
                alertDB.logAlert(priority, state, alertCount);
            } else {
                // No alerts - clear mute override only after timeout has passed
                if (localMuteActive) {
                    // Don't timeout if V1 still shows active bands - alert might return
                    if (state.activeBands != BAND_NONE) {
                        // V1 display still shows bands, keep mute active and skip update
                        continue;
                    }
                    
                    unsigned long timeSinceMute = millis() - localMuteTimestamp;
                    if (timeSinceMute >= LOCAL_MUTE_TIMEOUT_MS) {
                        SerialLog.println("Alert cleared - clearing local mute override and sending unmute to V1");
                        localMuteActive = false;
                        localMuteOverride = false;
                        mutedAlertStrength = 0;
                        mutedAlertBand = BAND_NONE;
                        mutedAlertFreq = 0;
                        // Send unmute command to V1
                        bleClient.setMute(false);
                        unmuteSentTimestamp = millis();  // Track when we sent unmute
                    } else {
                        // Still waiting for timeout - skip display update to avoid color flash
                        continue;
                    }
                }
                
                display.update(state);
                alertLogger.updateStateOnClear(state);
                
                // Update timestamp before logging (ensures real-time accuracy)
                time_t now = time(nullptr);
                if (now > 1609459200) {  // Valid if after 2021-01-01
                    alertDB.setTimestampUTC((uint32_t)now);
                }
                alertDB.logClear();
            }
        }
    }
}

void setup() {
    // Wait for USB to stabilize after upload
    delay(100);
    
    // Create BLE data queue early - before any BLE operations
    bleDataQueue = xQueueCreate(64, sizeof(BLEDataPacket));  // Increased from 32 to handle web server blocking
    rxBuffer.reserve(1024);
    
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
    
    if (batteryManager.isOnBattery()) {
        Serial.printf("[Battery] Voltage: %dmV (%d%%)\n", 
                      batteryManager.getVoltageMillivolts(), 
                      batteryManager.getPercentage());
    }
#endif
    
    // Initialize display
    if (!display.begin()) {
        SerialLog.println("Display initialization failed!");
        while (1) delay(1000);
    }

    // Brief delay to ensure panel is fully cleared before enabling backlight
    delay(100);

    // Show boot splash only on true power-on (not crash reboots or firmware uploads)
    if (resetReason == ESP_RST_POWERON) {
        // True cold boot - show splash (shorter duration for faster boot)
        display.showBootSplash();
        delay(1500);  // Reduced from 2000ms
    }
    // After splash (or skipping it), show scanning screen until connected
    display.showScanning();
    
// Initialize settings first to get active profile slot and last V1 address
    settingsManager.begin();
    
    // Show the current profile indicator
    display.drawProfileIndicator(settingsManager.get().activeSlot);
    
    // If you want to show the demo, call display.showDemo() manually elsewhere (e.g., via a button or menu)
    
    lastLvTick = millis();

    // Mount SD card for alert logging (non-fatal if missing)
    alertLogger.begin();
    
    // Initialize serial logger to SD card (for debugging in the field)
    SerialLog.begin();
    if (SerialLog.isEnabled()) {
        SerialLog.println("[Setup] Serial logging to SD enabled");
    }
    
    // Initialize time manager (NTP-only, no SD card dependency)
    timeManager.begin(alertLogger.isReady() ? alertLogger.getFilesystem() : nullptr);
    
    // Initialize SQLite alert database (uses same SD card)
    if (alertLogger.isReady()) {
        if (alertDB.begin()) {
            SerialLog.printf("[Setup] AlertDB ready - %s\n", alertDB.statusText().c_str());
            SerialLog.printf("[Setup] Total alerts in DB: %lu\n", alertDB.getTotalAlerts());
        } else {
            SerialLog.println("[Setup] AlertDB init failed - using CSV fallback");
        }
    }
    
    // Initialize V1 profile manager (uses alert logger's filesystem)
    if (alertLogger.isReady()) {
        v1ProfileManager.begin(alertLogger.getFilesystem());
    }
    
    // Load known V1 addresses from SD card for fast reconnect
    std::vector<std::string> knownV1Addresses;
    bool skipFastReconnect = false;
    
    // After firmware flash, delete cache and skip fast reconnect to force fresh connection
    if (resetReason == ESP_RST_SW || resetReason == ESP_RST_UNKNOWN) {
        SerialLog.println("[V1Cache] Firmware flash detected - clearing V1 cache for fresh connection");
        if (alertLogger.isReady()) {
            fs::FS* fs = alertLogger.getFilesystem();
            if (fs->exists("/known_v1.txt")) {
                fs->remove("/known_v1.txt");
                SerialLog.println("[V1Cache] Deleted known_v1.txt");
            }
        }
        skipFastReconnect = true;
    }
    
    if (!skipFastReconnect && alertLogger.isReady()) {
        fs::FS* fs = alertLogger.getFilesystem();
        File file = fs->open("/known_v1.txt", FILE_READ);
        if (file) {
            SerialLog.println("[V1Cache] Loading known V1 addresses from SD...");
            while (file.available()) {
                String line = file.readStringUntil('\n');
                line.trim();
                if (line.length() == 17 && line.indexOf(':') > 0) {  // MAC format: aa:bb:cc:dd:ee:ff
                    knownV1Addresses.push_back(line.c_str());
                    SerialLog.printf("[V1Cache]   - %s\n", line.c_str());
                }
            }
            file.close();
            SerialLog.printf("[V1Cache] Loaded %d known V1 address(es)\n", knownV1Addresses.size());
        } else {
            SerialLog.println("[V1Cache] No known_v1.txt found (will be created on first connection)");
        }
    }
    
    SerialLog.println("==============================");
    SerialLog.println("WiFi Configuration:");
    SerialLog.printf("  enableWifi: %s\n", settingsManager.get().enableWifi ? "YES" : "NO");
    SerialLog.printf("  wifiMode: %d\n", settingsManager.get().wifiMode);
    SerialLog.printf("  apSSID: %s\n", settingsManager.get().apSSID.c_str());
    SerialLog.println("==============================");
    
    // Initialize WiFi manager
    SerialLog.println("Starting WiFi manager...");
    wifiManager.begin();
        
        // Set up callbacks for web interface
        wifiManager.setStatusCallback([]() {
            return "\"v1_connected\":" + String(bleClient.isConnected() ? "true" : "false");
        });
        
        wifiManager.setAlertCallback([]() {
            String json = "{";
            if (parser.hasAlerts()) {
                AlertData alert = parser.getPriorityAlert();
                json += "\"active\":true,";
                json += "\"band\":\"";
                if (alert.band == BAND_KA) json += "Ka";
                else if (alert.band == BAND_K) json += "K";
                else if (alert.band == BAND_X) json += "X";
                else if (alert.band == BAND_LASER) json += "LASER";
                else json += "None";
                json += "\",";
                json += "\"strength\":" + String(alert.frontStrength) + ",";
                json += "\"frequency\":" + String(alert.frequency) + ",";
                json += "\"direction\":" + String(alert.direction);
            } else {
                json += "\"active\":false";
            }
            json += "}";
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
        
        // Set up filesystem callback for V1 device cache
        wifiManager.setFilesystemCallback([]() -> fs::FS* {
            if (alertLogger.isReady()) {
                return alertLogger.getFilesystem();
            }
            return nullptr;
        });
        
        SerialLog.println("WiFi initialized");
    
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
    
    // Try fast reconnect with each known V1 address from SD card (skip after firmware flash)
    bool fastReconnectAttempted = false;
    if (!skipFastReconnect) {
        for (const auto& addr : knownV1Addresses) {
            SerialLog.printf("[FastReconnect] Trying %s...\n", addr.c_str());
            bleClient.setTargetAddress(NimBLEAddress(addr, BLE_ADDR_PUBLIC));
            
            if (bleClient.fastReconnect()) {
                SerialLog.printf("[FastReconnect] Connected to %s!\n", addr.c_str());
                fastReconnectAttempted = true;
                break;
            } else {
                SerialLog.printf("[FastReconnect] Failed for %s, trying next...\n", addr.c_str());
            }
        }
    } else {
        SerialLog.println("[FastReconnect] Skipped after firmware flash");
    }
    
    // If fast reconnect worked, skip normal scan
    if (fastReconnectAttempted && bleClient.isConnected()) {
        SerialLog.println("[FastReconnect] Success - skipping scan");
    } else {
        // All cached addresses failed, start normal scanning
        SerialLog.println("[FastReconnect] All known addresses failed, starting general scan for V1...");
        if (!bleClient.begin(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
            SerialLog.println("BLE scan failed to start!");
            display.showDisconnected();
            while (1) delay(1000);
        }
    }
    
    // Register data callback
    bleClient.onDataReceived(onV1Data);
    
    // Register V1 connection callback for auto-push
    bleClient.onV1Connected(onV1Connected);
#else
    SerialLog.println("[REPLAY_MODE] BLE disabled - using packet replay for UI testing");
#endif
    
    // Initialize touch handler (SDA=17, SCL=18, addr=AXS_TOUCH_ADDR for AXS15231B touch, rst=-1 for no reset)
    SerialLog.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        SerialLog.println("Touch handler initialized successfully");
    } else {
        SerialLog.println("WARNING: Touch handler failed to initialize - continuing anyway");
    }
    
    SerialLog.println("Setup complete - WiFi and BLE enabled");
}

void loop() {
#if 1  // WiFi and BLE enabled
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
#endif

    // Check for touch - single tap for mute (only with active alert), triple-tap for profile cycle (only without alert)
    int16_t touchX, touchY;
    bool hasActiveAlert = parser.hasAlerts();
    
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
                    
                    const char* slotNames[] = {"Default", "Highway", "Comfort"};
                    SerialLog.printf("PROFILE CHANGE: Switched to '%s' (slot %d)\n", slotNames[newSlot], newSlot);
                    
                    // Update display to show new profile
                    display.drawProfileIndicator(newSlot);
                    
                    // If connected to V1 and auto-push is enabled, push the new profile
                    if (bleClient.isConnected() && s.autoPushEnabled) {
                        SerialLog.println("Pushing new profile to V1...");
                        onV1Connected();  // Re-use the connection callback to push profile
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
            
            if (!hasActiveAlert) {
                SerialLog.println("MUTE BLOCKED: No active alert to mute");
            } else {
                // Get current mute state from parser and toggle it
                DisplayState state = parser.getDisplayState();
                bool currentMuted = state.muted;
                bool newMuted = !currentMuted;
                
                // Apply local override immediately for instant visual feedback
                localMuteOverride = newMuted;
                localMuteActive = true;
                localMuteTimestamp = millis();
                
                // Store current alert details for detecting stronger signals
                if (newMuted) {
                    AlertData priority = parser.getPriorityAlert();
                    mutedAlertStrength = std::max(priority.frontStrength, priority.rearStrength);
                    mutedAlertBand = priority.band;
                    mutedAlertFreq = priority.frequency;
                    SerialLog.printf("Muted alert: band=%d, strength=%d, freq=%lu\n", 
                                mutedAlertBand, mutedAlertStrength, mutedAlertFreq);
                } else {
                    // Unmuting - clear stored alert
                    mutedAlertStrength = 0;
                    mutedAlertBand = BAND_NONE;
                    mutedAlertFreq = 0;
                }
                
                SerialLog.printf("Current mute state: %s -> Sending: %s\n", 
                              currentMuted ? "MUTED" : "UNMUTED",
                              newMuted ? "MUTE_ON" : "MUTE_OFF");
                
                // Send mute command to V1
                bool cmdSent = bleClient.setMute(newMuted);
                SerialLog.printf("Mute command sent: %s\n", cmdSent ? "OK" : "FAIL");
            }
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
    unsigned long now = millis();
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
        lastDisplayUpdate = now;
        
        // Check connection status
        static bool wasConnected = false;
        bool isConnected = bleClient.isConnected();
        
        // Only trigger state changes on actual transitions
        if (isConnected != wasConnected) {
            if (isConnected) {
                display.showResting(); // stay on resting view until data arrives
                SerialLog.println("V1 connected!");
            } else {
                display.showScanning();
                SerialLog.println("V1 disconnected - Scanning...");
            }
            wasConnected = isConnected;
        }

        // If connected but not seeing traffic, re-request alert data periodically
        static unsigned long lastReq = 0;
        if (isConnected && (now - lastRxMillis) > 2000 && (now - lastReq) > 1000) {
            SerialLog.println("No data recently; re-requesting alert data...");
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
    
    // Status update (print to serial)
    if (now - lastStatusUpdate >= STATUS_UPDATE_MS) {
        lastStatusUpdate = now;
        
        if (bleClient.isConnected()) {
            DisplayState state = parser.getDisplayState();
            if (parser.hasAlerts()) {
                SerialLog.printf("Active alerts: %d\n", parser.getAlertCount());
            }
        }
    }
    
#endif

    delay(5);  // Minimal yield for watchdog
}
