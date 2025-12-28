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
#include "alert_db.h"
#include "touch_handler.h"
#include "v1_profiles.h"
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
    uint8_t data[64];  // Max expected packet size
    size_t length;
    uint16_t charUUID;  // Last 16-bit of characteristic UUID to identify source
};

unsigned long lastDisplayUpdate = 0;
unsigned long lastStatusUpdate = 0;
unsigned long lastLvTick = 0;
unsigned long lastRxMillis = 0;
unsigned long lastDisplayDraw = 0;  // Throttle display updates
const unsigned long DISPLAY_DRAW_MIN_MS = 100;  // Min 100ms between draws

// Local mute override - takes immediate effect on tap before V1 confirms
static bool localMuteOverride = false;
static bool localMuteActive = false;
static unsigned long localMuteTimestamp = 0;
const unsigned long LOCAL_MUTE_TIMEOUT_MS = 800;  // Clear override 800ms after alert ends

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


// Callback for BLE data reception - just queues data, doesn't process
// This runs in BLE task context, so we avoid SPI operations here
void onV1Data(const uint8_t* data, size_t length, uint16_t charUUID) {
    if (bleDataQueue && length > 0 && length <= sizeof(BLEDataPacket::data)) {
        BLEDataPacket pkt;
        memcpy(pkt.data, data, length);
        pkt.length = length;
        pkt.charUUID = charUUID;
        // Non-blocking send to queue - if queue is full, drop the packet
        xQueueSendFromISR(bleDataQueue, &pkt, nullptr);
    }
}

// Callback when V1 connection is fully established
// Handles auto-push of default profile and mode
void onV1Connected() {
    const V1Settings& s = settingsManager.get();
    
    if (!s.autoPushEnabled) {
        Serial.println("[AutoPush] Disabled, skipping");
        return;
    }
    
    // Get the active slot configuration
    AutoPushSlot activeSlot = settingsManager.getActiveSlot();
    const char* slotNames[] = {"Default", "Highway", "Passenger Comfort"};
    Serial.printf("[AutoPush] V1 connected - applying '%s' profile (slot %d)...\n", 
                  slotNames[s.activeSlot], s.activeSlot);
    
    // Small delay to ensure V1 is ready
    delay(500);
    
    // Push profile if configured
    if (activeSlot.profileName.length() > 0) {
        Serial.printf("[AutoPush] Loading profile: %s\n", activeSlot.profileName.c_str());
        V1Profile profile;
        if (v1ProfileManager.loadProfile(activeSlot.profileName, profile)) {
            // Write user bytes to V1
            if (bleClient.writeUserBytes(profile.settings.bytes)) {
                Serial.println("[AutoPush] Profile settings pushed successfully");
            } else {
                Serial.println("[AutoPush] ERROR: Failed to push profile settings");
            }
            
            // Set display on/off based on profile
            delay(100);
            bleClient.setDisplayOn(profile.displayOn);
            Serial.printf("[AutoPush] Display set to: %s\n", profile.displayOn ? "ON" : "OFF");
        } else {
            Serial.printf("[AutoPush] ERROR: Failed to load profile '%s'\n", activeSlot.profileName.c_str());
        }
    } else {
        Serial.println("[AutoPush] No profile configured for active slot");
    }
    
    // Set mode if configured (not UNKNOWN)
    if (activeSlot.mode != V1_MODE_UNKNOWN) {
        delay(100);
        if (bleClient.setMode(static_cast<uint8_t>(activeSlot.mode))) {
            const char* modeName = "Unknown";
            if (activeSlot.mode == V1_MODE_ALL_BOGEYS) modeName = "All Bogeys";
            else if (activeSlot.mode == V1_MODE_LOGIC) modeName = "Logic";
            else if (activeSlot.mode == V1_MODE_ADVANCED_LOGIC) modeName = "Advanced Logic";
            Serial.printf("[AutoPush] Mode set to: %s\n", modeName);
        } else {
            Serial.println("[AutoPush] ERROR: Failed to set mode");
        }
    }
    
    // Set volumes if configured (not 0xFF = no change)
    uint8_t mainVol = settingsManager.getSlotVolume(s.activeSlot);
    uint8_t muteVol = settingsManager.getSlotMuteVolume(s.activeSlot);
    
    if (mainVol != 0xFF || muteVol != 0xFF) {
        delay(100);
        if (bleClient.setVolume(mainVol, muteVol)) {
            Serial.printf("[AutoPush] Volume set - main: %d, muted: %d\n", mainVol, muteVol);
        } else {
            Serial.println("[AutoPush] ERROR: Failed to set volume");
        }
    }
    
    Serial.println("[AutoPush] Complete");
}

// Process queued BLE data - called from main loop (safe for SPI)
void processBLEData() {
    BLEDataPacket pkt;
    
    // Process all queued packets
    while (xQueueReceive(bleDataQueue, &pkt, 0) == pdTRUE) {
        // Forward raw data to proxy clients (JBV1) - done here in main loop to avoid SPI conflicts
        // Pass the source characteristic UUID so data is forwarded to the correct proxy characteristic
        bleClient.forwardToProxy(pkt.data, pkt.length, pkt.charUUID);
        
        // Accumulate and frame on 0xAA ... 0xAB so we don't choke on chunked notifications
        rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
    }
    
    // If no data accumulated, return
    if (rxBuffer.empty()) {
        return;
    }

    // Trim runaway buffers
    if (rxBuffer.size() > 512) {
        rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() - 256);
    }

    // Discard anything before the first start byte
    auto startIt = std::find(rxBuffer.begin(), rxBuffer.end(), ESP_PACKET_START);
    if (startIt == rxBuffer.end()) {
        rxBuffer.clear();
        return;
    }
    if (startIt != rxBuffer.begin()) {
        rxBuffer.erase(rxBuffer.begin(), startIt);
    }

    while (true) {
        auto s = std::find(rxBuffer.begin(), rxBuffer.end(), ESP_PACKET_START);
        if (s == rxBuffer.end()) {
            rxBuffer.clear();
            break;
        }
        auto e = std::find(s + 1, rxBuffer.end(), ESP_PACKET_END);
        if (e == rxBuffer.end()) {
            // wait for more data
            if (s != rxBuffer.begin()) {
                rxBuffer.erase(rxBuffer.begin(), s);
            }
            break;
        }

        std::vector<uint8_t> packet(s, e + 1);
        rxBuffer.erase(rxBuffer.begin(), e + 1);

        Serial.print("RX: ");
        for (uint8_t b : packet) {
            Serial.printf("%02X ", b);
        }
        Serial.println();

        lastRxMillis = millis();

        // Check for user bytes response (0x12) - V1 settings pull
        if (packet.size() >= 12 && packet[3] == PACKET_ID_RESP_USER_BYTES) {
            // Payload starts at byte 5, length is 6 bytes
            uint8_t userBytes[6];
            memcpy(userBytes, &packet[5], 6);
            Serial.printf("V1 user bytes raw: %02X %02X %02X %02X %02X %02X\n",
                userBytes[0], userBytes[1], userBytes[2], userBytes[3], userBytes[4], userBytes[5]);
            Serial.printf("  xBand=%d, kBand=%d, kaBand=%d, laser=%d\n",
                userBytes[0] & 0x01, (userBytes[0] >> 1) & 0x01, 
                (userBytes[0] >> 2) & 0x01, (userBytes[0] >> 3) & 0x01);
            v1ProfileManager.setCurrentSettings(userBytes);
            Serial.println("Received V1 user bytes!");
            continue;  // Don't pass to parser
        }

        if (parser.parse(packet.data(), packet.size())) {
            DisplayState state = parser.getDisplayState();

            // Cache alert status to avoid repeated calls
            bool hasAlerts = parser.hasAlerts();

            // Apply local mute override FIRST before any other logic
            if (localMuteActive) {
                // While alerts are active, ALWAYS apply override (no timeout)
                // Timeout only applies when no alerts (waiting for V1 response)
                if (hasAlerts) {
                    // Force muted state while alert is active
                    state.muted = localMuteOverride;
                    // Reset timestamp so we get fresh timeout window when alert goes away
                    localMuteTimestamp = millis();
                } else {
                    // No alerts - check timeout
                    unsigned long now = millis();
                    if (now - localMuteTimestamp < LOCAL_MUTE_TIMEOUT_MS) {
                        state.muted = localMuteOverride;
                    } else {
                        // Timeout and no alerts - clear override
                        Serial.println("Local mute override timed out (no alerts)");
                        localMuteActive = false;
                        localMuteOverride = false;
                        mutedAlertStrength = 0;
                        mutedAlertBand = BAND_NONE;
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
                        Serial.printf("Different band detected: %d -> %d\n", mutedAlertBand, priority.band);
                    }

                    // Check for higher priority band (Ka > K > X, Laser is special)
                    // If muted K and Ka shows up (even weak) -> unmute
                    // If muted X and K or Ka shows up -> unmute
                    if (mutedAlertBand == BAND_K && priority.band == BAND_KA) {
                        higherPriorityBand = true;
                        Serial.println("Higher priority band: K muted, Ka detected");
                    } else if (mutedAlertBand == BAND_X && (priority.band == BAND_KA || priority.band == BAND_K)) {
                        higherPriorityBand = true;
                        Serial.printf("Higher priority band: X muted, %s detected\n", 
                                    priority.band == BAND_KA ? "Ka" : "K");
                    } else if (mutedAlertBand == BAND_LASER && priority.band != BAND_LASER && priority.band != BAND_NONE) {
                        higherPriorityBand = true;
                        Serial.printf("Radar band after Laser: %d\n", priority.band);
                    }

                    // Only check stronger signal if band changed - same band getting stronger is not a new threat
                    // (e.g., sweeping laser or approaching radar should stay muted)
                    if (priority.band != mutedAlertBand && currentStrength >= mutedAlertStrength + 2) {
                        strongerSignal = true;
                        Serial.printf("Stronger signal on different band: %d -> %d\n", mutedAlertStrength, currentStrength);
                    }

                    // Check if it's a different frequency (for same band, >15 MHz difference)
                    // Only applies to radar bands (laser freq is always 0)
                    if (priority.band == mutedAlertBand && priority.frequency > 0 && mutedAlertFreq > 0) {
                        uint32_t freqDiff = (priority.frequency > mutedAlertFreq) ? 
                                           (priority.frequency - mutedAlertFreq) : 
                                           (mutedAlertFreq - priority.frequency);
                        if (freqDiff > 50) {  // More than 50 MHz different
                            differentAlert = true;
                            Serial.printf("Different frequency: %lu -> %lu (diff: %lu)\n", 
                                        mutedAlertFreq, priority.frequency, freqDiff);
                        }
                    }

                    // Auto-unmute for stronger, different, or higher priority alert
                    if (strongerSignal || differentAlert || higherPriorityBand) {
                        Serial.println("Auto-unmuting for new/stronger/priority alert");
                        localMuteActive = false;
                        localMuteOverride = false;
                        state.muted = false;
                        mutedAlertStrength = 0;
                        mutedAlertBand = BAND_NONE;
                        mutedAlertFreq = 0;
                    }
                }

                display.update(priority, state, alertCount);
                alertLogger.logAlert(priority, state, alertCount);
                alertDB.logAlert(priority, state, alertCount);  // SQLite logging

                Serial.printf("Alert: %s, Dir: %d, Front: %d, Rear: %d, Freq: %lu MHz, Count: %d, Bands: 0x%02X\n",
                              priority.band == BAND_KA ? "Ka" :
                              priority.band == BAND_K ? "K" :
                              priority.band == BAND_X ? "X" :
                              priority.band == BAND_LASER ? "Laser" : "None",
                              priority.direction,
                              priority.frontStrength,
                              priority.rearStrength,
                              priority.frequency,
                              alertCount,
                              state.activeBands);
            } else {
                // No alerts - clear mute override only after timeout has passed
                if (localMuteActive) {
                    unsigned long timeSinceMute = millis() - localMuteTimestamp;
                    if (timeSinceMute >= LOCAL_MUTE_TIMEOUT_MS) {
                        Serial.println("Alert cleared - clearing local mute override and sending unmute to V1");
                        localMuteActive = false;
                        localMuteOverride = false;
                        mutedAlertStrength = 0;
                        mutedAlertBand = BAND_NONE;
                        mutedAlertFreq = 0;
                        // Send unmute command to V1
                        bleClient.setMute(false);
                    } else {
                        // Still waiting for timeout - skip display update to avoid color flash
                        continue;
                    }
                }
                
                static unsigned long lastStateLog = 0;
                if (millis() - lastStateLog > 2000) {
                    Serial.printf("DisplayState update: bands=0x%02X arrows=0x%02X bars=%d mute=%d\n",
                                  state.activeBands, state.arrows, state.signalBars, state.muted);
                    lastStateLog = millis();
                }
                display.update(state);
                alertLogger.updateStateOnClear(state);
                alertDB.logClear();  // SQLite logging
            }
        }
    }
}

void setup() {
    // Wait for USB to stabilize after upload
    delay(100);
    
    // Create BLE data queue early - before any BLE operations
    bleDataQueue = xQueueCreate(32, sizeof(BLEDataPacket));
    
// Backlight is handled in display.begin() (inverted PWM for Waveshare)

#if defined(PIN_POWER_ON) && PIN_POWER_ON >= 0
    // Cut panel power until we intentionally bring it up
    pinMode(PIN_POWER_ON, OUTPUT);
    digitalWrite(PIN_POWER_ON, LOW);
#endif

    Serial.begin(115200);
    delay(500);  // Give serial time to connect
    
    Serial.println("\n===================================");
    Serial.println("V1 Gen2 Simple Display");
    Serial.println("Firmware: " FIRMWARE_VERSION);
    Serial.print("Board: ");
    Serial.println(DISPLAY_NAME);
    Serial.println("===================================\n");
    
    // Initialize display
    if (!display.begin()) {
        Serial.println("Display initialization failed!");
        while (1) delay(1000);
    }

    // Add extra delay to ensure panel is fully cleared before enabling backlight
    delay(200);

    // Show RDF boot splash (backlight will be enabled after splash is drawn)
    display.showBootSplash();
    delay(4000);
    // After splash, show scanning screen until connected
    display.showScanning();
    
    // Initialize settings first to get active profile slot
    settingsManager.begin();
    
    // Show the current profile indicator
    display.drawProfileIndicator(settingsManager.get().activeSlot);
    
    // If you want to show the demo, call display.showDemo() manually elsewhere (e.g., via a button or menu)
    
    lastLvTick = millis();

    // Mount SD card for alert logging (non-fatal if missing)
    alertLogger.begin();
    
    // Initialize SQLite alert database (uses same SD card)
    if (alertLogger.isReady()) {
        if (alertDB.begin()) {
            Serial.printf("[Setup] AlertDB ready - %s\n", alertDB.statusText().c_str());
            Serial.printf("[Setup] Total alerts in DB: %lu\n", alertDB.getTotalAlerts());
        } else {
            Serial.println("[Setup] AlertDB init failed - using CSV fallback");
        }
    }
    
    // Initialize V1 profile manager (uses alert logger's filesystem)
    if (alertLogger.isReady()) {
        v1ProfileManager.begin(alertLogger.getFilesystem());
    }
    
    Serial.println("==============================");
    Serial.println("WiFi Configuration:");
    Serial.printf("  enableWifi: %s\n", settingsManager.get().enableWifi ? "YES" : "NO");
    Serial.printf("  wifiMode: %d\n", settingsManager.get().wifiMode);
    Serial.printf("  apSSID: %s\n", settingsManager.get().apSSID.c_str());
    Serial.println("==============================");
    
    // Initialize WiFi manager
    Serial.println("Starting WiFi manager...");
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
        
        Serial.println("WiFi initialized");
    
    // Initialize BLE client with proxy settings from preferences
    const V1Settings& bleSettings = settingsManager.get();
    Serial.printf("Starting BLE (proxy: %s, name: %s)\n", 
                  bleSettings.proxyBLE ? "enabled" : "disabled",
                  bleSettings.proxyName.c_str());
    
    if (!bleClient.begin(bleSettings.proxyBLE, bleSettings.proxyName.c_str())) {
        Serial.println("BLE initialization failed!");
        display.showDisconnected();
        while (1) delay(1000);
    }
    
    // Register data callback
    bleClient.onDataReceived(onV1Data);
    
    // Register V1 connection callback for auto-push
    bleClient.onV1Connected(onV1Connected);
    
    // Initialize touch handler (SDA=17, SCL=18, addr=AXS_TOUCH_ADDR for AXS15231B touch, rst=-1 for no reset)
    Serial.println("Initializing touch handler...");
    if (touchHandler.begin(17, 18, AXS_TOUCH_ADDR, -1)) {
        Serial.println("Touch handler initialized successfully");
    } else {
        Serial.println("WARNING: Touch handler failed to initialize - continuing anyway");
    }
    
    Serial.println("Setup complete - WiFi and BLE enabled");
}

void loop() {
#if 1  // WiFi and BLE enabled
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
            
            Serial.printf("Tap detected: count=%d, x=%d, y=%d, hasAlert=%d\n", tapCount, touchX, touchY, hasActiveAlert);
            
            // Check for triple-tap to cycle profiles (ONLY when no active alert)
            if (tapCount >= 3) {
                tapCount = 0;  // Reset tap count
                
                if (hasActiveAlert) {
                    Serial.println("PROFILE CHANGE BLOCKED: Active alert present - tap to mute instead");
                } else {
                    // Cycle to next profile slot: 0 -> 1 -> 2 -> 0
                    const V1Settings& s = settingsManager.get();
                    int newSlot = (s.activeSlot + 1) % 3;
                    settingsManager.setActiveSlot(newSlot);
                    settingsManager.save();
                    
                    const char* slotNames[] = {"Default", "Highway", "Comfort"};
                    Serial.printf("PROFILE CHANGE: Switched to '%s' (slot %d)\n", slotNames[newSlot], newSlot);
                    
                    // Update display to show new profile
                    display.drawProfileIndicator(newSlot);
                    
                    // If connected to V1 and auto-push is enabled, push the new profile
                    if (bleClient.isConnected() && s.autoPushEnabled) {
                        Serial.println("Pushing new profile to V1...");
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
            Serial.printf("Processing %d tap(s) as mute toggle\n", tapCount);
            tapCount = 0;
            
            if (!hasActiveAlert) {
                Serial.println("MUTE BLOCKED: No active alert to mute");
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
                    Serial.printf("Muted alert: band=%d, strength=%d, freq=%lu\n", 
                                mutedAlertBand, mutedAlertStrength, mutedAlertFreq);
                } else {
                    // Unmuting - clear stored alert
                    mutedAlertStrength = 0;
                    mutedAlertBand = BAND_NONE;
                    mutedAlertFreq = 0;
                }
                
                Serial.printf("Current mute state: %s -> Sending: %s\n", 
                              currentMuted ? "MUTED" : "UNMUTED",
                              newMuted ? "MUTE_ON" : "MUTE_OFF");
                
                // Send mute command to V1
                bool cmdSent = bleClient.setMute(newMuted);
                Serial.printf("Mute command sent: %s\n", cmdSent ? "OK" : "FAIL");
            }
        }
    }
    
    // Process BLE events
    bleClient.process();
    
    // Process queued BLE data (safe for SPI - runs in main loop context)
    processBLEData();

    // Process WiFi/web server
    wifiManager.process();
    
    // Update display periodically
    unsigned long now = millis();
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
        lastDisplayUpdate = now;
        
        // Check connection status
        static bool wasConnected = false;
        bool isConnected = bleClient.isConnected();
        
        if (isConnected && !wasConnected) {
            display.showResting(); // stay on resting view until data arrives
            Serial.println("V1 connected!");
        } else if (!isConnected && wasConnected) {
            display.showScanning();
            Serial.println("V1 disconnected - Scanning...");
        }
        
        wasConnected = isConnected;

        // If connected but not seeing traffic, re-request alert data periodically
        static unsigned long lastReq = 0;
        if (isConnected && (now - lastRxMillis) > 2000 && (now - lastReq) > 1000) {
            Serial.println("No data recently; re-requesting alert data...");
            bleClient.requestAlertData();
            lastReq = now;
        }
    }
    
    // Status update (print to serial)
    if (now - lastStatusUpdate >= STATUS_UPDATE_MS) {
        lastStatusUpdate = now;
        
        if (bleClient.isConnected()) {
            DisplayState state = parser.getDisplayState();
            if (parser.hasAlerts()) {
                Serial.printf("Active alerts: %d\n", parser.getAllAlerts().size());
            }
        }
    }
    
#endif

    delay(10);
}
