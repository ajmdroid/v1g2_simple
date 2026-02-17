/**
 * BLE command builders for Valentine1 Gen2
 *
 * Extracted from ble_client.cpp — packet construction and
 * send helpers for V1 commands (mute, mode, volume, user bytes, etc.).
 */

#include "ble_client.h"
#include "../include/ble_internals.h"
#include "../include/config.h"
#include "perf_metrics.h"
#include <cstring>

// ---- checksum helper (used only by command builders) --------------------

static inline uint8_t calcV1Checksum(const uint8_t* data, size_t len) {
    uint8_t sum = 0;
    for (size_t i = 0; i < len; ++i) {
        sum += data[i];
    }
    return sum;
}

// ---- command send primitives -------------------------------------------

bool V1BLEClient::sendCommand(const uint8_t* data, size_t length) {
    return sendCommandWithResult(data, length) == SendResult::SENT;
}

SendResult V1BLEClient::sendCommandWithResult(const uint8_t* data, size_t length) {
    // Hard failures first - these should not be retried
    if (!isConnected() || !pCommandChar) {
        return SendResult::FAILED;
    }
    if (!data || length == 0 || length > 64) {
        return SendResult::FAILED;
    }

    // Light pacing: non-blocking timestamp gate
    // Return NOT_YET if too soon - caller retains packet for retry
    static unsigned long lastCommandMs = 0;
    unsigned long nowMs = millis();
    if (lastCommandMs != 0 && nowMs - lastCommandMs < 5) {
        PERF_INC(cmdPaceNotYet);
        return SendResult::NOT_YET;
    }
    lastCommandMs = millis();
    
    bool ok = false;
    if (pCommandChar->canWrite()) {
        ok = pCommandChar->writeValue(data, length, true);
    } else if (pCommandChar->canWriteNoResponse()) {
        ok = pCommandChar->writeValue(data, length, false);
    } else {
        return SendResult::FAILED;  // Characteristic doesn't support write
    }
    
    if (!ok) {
        // Write failed after isConnected() check - likely transient (BLE busy/queue full)
        // Return NOT_YET to retry; if connection truly dead, next isConnected() will catch it
        PERF_INC(cmdBleBusy);
        return SendResult::NOT_YET;
    }
    return SendResult::SENT;
}

// ---- V1 protocol command builders -------------------------------------

bool V1BLEClient::requestAlertData() {
    // NOTE: Packet structure intentionally explicit (not abstracted) - January 20, 2026 review.
    // Matches V1 protocol docs exactly; easier to verify than a builder pattern.
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_REQ_START_ALERT,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    packet[5] = calcV1Checksum(packet, 5);

    BLE_LOGLN("Requesting alert data from V1...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::requestVersion() {
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_VERSION,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    packet[5] = calcV1Checksum(packet, 5);

    BLE_LOGLN("Requesting version info from V1...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setDisplayOn(bool on) {
    // For dark mode, we need to use reqTurnOffMainDisplay with proper payload
    // Mode 0 = completely off, Mode 1 = only BT icon visible
    // For "dark mode on" we want display OFF, for "dark mode off" we want display ON
    // 
    // Packet format derived from v1g2-t4s3 reference implementation:
    // - reqTurnOnMainDisplay: 7-byte packet with payloadLength=1, no actual payload data
    // - reqTurnOffMainDisplay: 8-byte packet with payloadLength=2, only 1 mode byte in payload
    
    if (on) {
        // Turn display back ON (exit dark mode)
        // Packet: AA DA E4 33 01 [checksum] AB  (7 bytes total)
        uint8_t packet[] = {
            ESP_PACKET_START,                               // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
            PACKET_ID_TURN_ON_DISPLAY,                      // [3] 0x33
            0x01,                                           // [4] payload length
            0x00,                                           // [5] checksum placeholder
            ESP_PACKET_END                                  // [6] 0xAB
        };
        
        // Calculate checksum over bytes 0-4 (5 bytes)
        packet[5] = calcV1Checksum(packet, 5);
        
        return sendCommand(packet, sizeof(packet));
    } else {
        // Turn display OFF (enter dark mode)
        // Per Kenny's implementation: payloadLength=2 but only 1 actual payload byte
        // Packet: AA DA E4 32 02 [mode] [checksum] AB  (8 bytes total)
        // mode=0: completely dark, mode=1: only BT icon visible
        uint8_t mode = 0x00;  // Completely dark
        uint8_t packet[] = {
            ESP_PACKET_START,                               // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
            PACKET_ID_TURN_OFF_DISPLAY,                     // [3] 0x32
            0x02,                                           // [4] payload length = 2
            mode,                                           // [5] mode byte
            0x00,                                           // [6] checksum placeholder
            ESP_PACKET_END                                  // [7] 0xAB
        };
        
        // Calculate checksum over bytes 0-5 (6 bytes)
        packet[6] = calcV1Checksum(packet, 6);
        
        return sendCommand(packet, sizeof(packet));
    }
}

bool V1BLEClient::setMute(bool muted) {
    uint8_t packetId = muted ? PACKET_ID_MUTE_ON : PACKET_ID_MUTE_OFF;
    // reqMuteOn/Off has no payload (payload length = 1, no actual payload bytes needed per V1 protocol)
    // Packet: AA DA E4 34/35 01 [checksum] AB
    uint8_t packet[] = {
        ESP_PACKET_START,                               // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
        packetId,                                       // [3] 0x34 or 0x35
        0x01,                                           // [4] payload length
        0x00,                                           // [5] checksum placeholder
        ESP_PACKET_END                                  // [6] 0xAB
    };

    packet[5] = calcV1Checksum(packet, 5);
    
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setMode(uint8_t mode) {
    // Packet ID 0x36 = REQCHANGEMODE
    // Mode: 0x01 = All Bogeys, 0x02 = Logic, 0x03 = Advanced Logic
    uint8_t packet[] = {
        ESP_PACKET_START,                               // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
        0x36,                                           // [3] REQCHANGEMODE
        0x02,                                           // [4] payload length = 2
        mode,                                           // [5] mode byte
        0x00,                                           // [6] checksum placeholder
        ESP_PACKET_END                                  // [7] 0xAB
    };
    
    // Calculate checksum over bytes 0-5 (6 bytes)
    packet[6] = calcV1Checksum(packet, 6);
    
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setVolume(uint8_t mainVolume, uint8_t mutedVolume) {
    // 0xFF means "don't change" - skip command entirely if either is undefined
    // V1 REQWRITEVOLUME sets BOTH values, so we need both to be valid (0-9)
    if (mainVolume == 0xFF || mutedVolume == 0xFF) {
        Serial.printf("setVolume: skipping - main=%d mute=%d (0xFF means not configured)\n", 
                      mainVolume, mutedVolume);
        return true;  // Success - nothing to do (user hasn't configured both)
    }
    
    // Clamp to valid range (0-9)
    if (mainVolume > 9) mainVolume = 9;
    if (mutedVolume > 9) mutedVolume = 9;
    
    // Packet ID 0x39 = REQWRITEVOLUME
    // Payload: mainVolume, mutedVolume (currentVolume), aux0
    uint8_t packet[] = {
        ESP_PACKET_START,                               // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
        PACKET_ID_REQ_WRITE_VOLUME,                     // [3] 0x39
        0x04,                                           // [4] payload length = 4 (3 data + checksum)
        mainVolume,                                     // [5] main volume 0-9
        mutedVolume,                                    // [6] muted volume 0-9  
        0x00,                                           // [7] aux0 (unused, set to 0)
        0x00,                                           // [8] checksum placeholder
        ESP_PACKET_END                                  // [9] 0xAB
    };
    
    // Calculate checksum over bytes 0-7 (8 bytes)
    packet[8] = calcV1Checksum(packet, 8);
    
    return sendCommand(packet, sizeof(packet));
}

// ---- user bytes read/write -------------------------------------------

bool V1BLEClient::requestUserBytes() {
    // Build packet: AA D0+dest E0+src 11 01 [checksum] AB
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_REQ_USER_BYTES,
        0x01,  // length
        0x00,  // checksum placeholder
        ESP_PACKET_END
    };

    packet[5] = calcV1Checksum(packet, 5);

    BLE_LOGLN("Requesting V1 user bytes...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::writeUserBytes(const uint8_t* bytes) {
    if (!bytes) {
        return false;
    }
    
    // Build packet: AA D0+dest E0+src 13 07 [6 bytes] [checksum] AB
    uint8_t packet[13];
    packet[0] = ESP_PACKET_START;
    packet[1] = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);
    packet[2] = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);
    packet[3] = PACKET_ID_WRITE_USER_BYTES;
    packet[4] = 0x07;  // length = 6 bytes + 1
    memcpy(&packet[5], bytes, 6);
    
    packet[11] = calcV1Checksum(packet, 11);
    packet[12] = ESP_PACKET_END;

    Serial.printf("Writing V1 user bytes: %02X %02X %02X %02X %02X %02X\n",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return sendCommand(packet, sizeof(packet));
}

V1BLEClient::WriteVerifyResult V1BLEClient::writeUserBytesVerified(const uint8_t* bytes, int maxRetries) {
    if (!bytes || !isConnected()) {
        return VERIFY_WRITE_FAILED;
    }

    // Note: Full verification is disabled because BLE responses come async via the main loop queue,
    // but this function blocks. The write typically succeeds - V1 is reliable.
    // We do multiple write attempts for robustness but don't wait for read-back verification.
    
    Serial.println("[VerifyPush] Writing to V1 (async verification not possible in blocking context)");
    
    for (int attempt = 1; attempt <= maxRetries; attempt++) {
        if (writeUserBytes(bytes)) {
            Serial.printf("[VerifyPush] Write command sent successfully (attempt %d/%d)\n", attempt, maxRetries);
            // Request a read-back — result comes async via main loop queue
            // No delay: V1 processes writes immediately, read-back is fire-and-forget
            requestUserBytes();
            return VERIFY_OK;
        }
        Serial.printf("[VerifyPush] Write attempt %d/%d failed, retrying...\n", attempt, maxRetries);
        // No delay between retries — BLE write failure is immediate (not connected/char null),
        // so retrying instantly is correct. If the link is up, next attempt succeeds immediately.
    }
    
    Serial.println("[VerifyPush] All write attempts failed");
    return VERIFY_WRITE_FAILED;
}

// ---- user bytes verification -----------------------------------------

void V1BLEClient::startUserBytesVerification(const uint8_t* expected) {
    if (!expected) {
        return;
    }
    memcpy(verifyExpected, expected, 6);
    verifyPending = true;
    verifyComplete = false;
    verifyMatch = false;
}

void V1BLEClient::onUserBytesReceived(const uint8_t* bytes) {
    if (verifyPending && bytes) {
        memcpy(verifyReceived, bytes, 6);
        verifyComplete = true;
        verifyMatch = (memcmp(verifyExpected, verifyReceived, 6) == 0);
        Serial.printf("[VerifyPush] Received user bytes: %02X%02X%02X%02X%02X%02X (match=%s)\n",
            verifyReceived[0], verifyReceived[1], verifyReceived[2],
            verifyReceived[3], verifyReceived[4], verifyReceived[5],
            verifyMatch ? "YES" : "NO");
        verifyPending = false;
    }
}
