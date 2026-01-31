// BLE Serial Module - Implementation
// Receives BLE packets over USB Serial for replay/testing

#include "ble_serial_module.h"
#include "ble_queue_module.h"

// Global instance
BLESerialModule bleSerialModule;

void BLESerialModule::begin(BleQueueModule* queue) {
    bleQueue = queue;
    enabled = false;
    linePos = 0;
    hasPendingPacket = false;
    packetsReceived = 0;
    lastPacketMs = 0;
    
    Serial.println("[BLESerial] Module initialized");
}

void BLESerialModule::setEnabled(bool en) {
    if (en != enabled) {
        enabled = en;
        if (enabled) {
            // Clear any pending state when enabling
            linePos = 0;
            hasPendingPacket = false;
            Serial.println("[BLESerial] Enabled - ready for packet replay");
        } else {
            Serial.println("[BLESerial] Disabled");
        }
    }
}

void BLESerialModule::process() {
    if (!enabled || !bleQueue) {
        return;
    }
    
    // First, check if we have a pending packet waiting to be sent
    if (hasPendingPacket) {
        unsigned long elapsed = millis() - delayStartMs;
        if (elapsed >= pendingDelayMs) {
            // Time to send the pending packet
            sendPacket(pendingPacket, pendingPacketLen);
            hasPendingPacket = false;
        } else {
            // Still waiting - don't read more serial yet
            return;
        }
    }
    
    // Read from serial and process lines
    while (Serial.available()) {
        char c = Serial.read();
        
        if (c == '\n' || c == '\r') {
            if (linePos > 0) {
                lineBuf[linePos] = '\0';
                processLine(lineBuf);
                linePos = 0;
            }
        } else if (linePos < LINE_BUF_SIZE - 1) {
            lineBuf[linePos++] = c;
        }
        
        // If we now have a pending packet with delay, stop reading
        if (hasPendingPacket && pendingDelayMs > 0) {
            break;
        }
    }
}

void BLESerialModule::processLine(const char* line) {
    // Skip empty lines and comments
    if (line[0] == '\0' || line[0] == '#') {
        return;
    }
    
    // Format options:
    // 1. "PKT:delay_ms:HEXDATA" - packet with timing delay
    // 2. "HEXDATA" - immediate packet (no delay)
    // 3. "hex=HEXDATA" - legacy format, immediate
    
    unsigned long delayMs = 0;
    const char* hexStart = line;
    
    if (strncmp(line, "PKT:", 4) == 0) {
        // Parse delay
        const char* delayStart = line + 4;
        char* colonPos = strchr(delayStart, ':');
        if (colonPos) {
            delayMs = strtoul(delayStart, nullptr, 10);
            hexStart = colonPos + 1;
        } else {
            Serial.println("[BLESerial] Invalid PKT format");
            return;
        }
    } else if (strncmp(line, "hex=", 4) == 0) {
        // Legacy format
        hexStart = line + 4;
    }
    
    // Skip whitespace
    while (*hexStart == ' ' || *hexStart == '\t') {
        hexStart++;
    }
    
    // Parse hex to bytes
    uint8_t pktBuf[256];
    size_t pktLen = hexToBytes(hexStart, pktBuf, sizeof(pktBuf));
    
    if (pktLen < 6) {
        if (pktLen > 0) {
            Serial.printf("[BLESerial] Packet too short (%d bytes)\n", (int)pktLen);
        }
        return;
    }
    
    // Validate packet framing (0xAA start, 0xAB end)
    if (pktBuf[0] != 0xAA || pktBuf[pktLen - 1] != 0xAB) {
        Serial.printf("[BLESerial] Invalid framing (start=0x%02X end=0x%02X)\n",
                      pktBuf[0], pktBuf[pktLen - 1]);
        return;
    }
    
    if (delayMs > 0) {
        // Queue packet with delay
        memcpy(pendingPacket, pktBuf, pktLen);
        pendingPacketLen = pktLen;
        pendingDelayMs = delayMs;
        delayStartMs = millis();
        hasPendingPacket = true;
    } else {
        // Send immediately
        sendPacket(pktBuf, pktLen);
    }
}

void BLESerialModule::sendPacket(const uint8_t* data, size_t len) {
    // Feed packet through the normal BLE queue path
    // Using charUUID = 0 to indicate serial source (vs real BLE)
    bleQueue->onNotify(data, len, 0);
    
    packetsReceived++;
    lastPacketMs = millis();
}

size_t BLESerialModule::hexToBytes(const char* hex, uint8_t* out, size_t maxLen) {
    size_t len = 0;
    while (*hex && *(hex + 1) && len < maxLen) {
        char hi = *hex++;
        char lo = *hex++;
        uint8_t val = 0;
        
        if (hi >= '0' && hi <= '9') val = (hi - '0') << 4;
        else if (hi >= 'A' && hi <= 'F') val = (hi - 'A' + 10) << 4;
        else if (hi >= 'a' && hi <= 'f') val = (hi - 'a' + 10) << 4;
        else break;
        
        if (lo >= '0' && lo <= '9') val |= (lo - '0');
        else if (lo >= 'A' && lo <= 'F') val |= (lo - 'A' + 10);
        else if (lo >= 'a' && lo <= 'f') val |= (lo - 'a' + 10);
        else break;
        
        out[len++] = val;
    }
    return len;
}
