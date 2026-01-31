// BLE Serial Module - Receives BLE packets over USB Serial for replay/testing
// This module runs alongside the real BLE module and feeds packets through
// the same processing pipeline.

#pragma once

#include <Arduino.h>

// Forward declarations
class BleQueueModule;

class BLESerialModule {
public:
    void begin(BleQueueModule* queue);
    
    // Call from main loop - reads serial, paces packets, feeds to queue
    void process();
    
    // Enable/disable (controlled from web UI /dev page)
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled; }
    
    // Stats for debugging
    unsigned long getPacketsReceived() const { return packetsReceived; }
    unsigned long getLastPacketMs() const { return lastPacketMs; }

private:
    BleQueueModule* bleQueue = nullptr;
    bool enabled = false;
    
    // Line buffer for reading serial
    static constexpr size_t LINE_BUF_SIZE = 600;  // 256 bytes = 512 hex + overhead
    char lineBuf[LINE_BUF_SIZE];
    size_t linePos = 0;
    
    // Timing control for paced replay
    unsigned long pendingDelayMs = 0;      // Delay before sending pending packet
    unsigned long delayStartMs = 0;        // When we started waiting
    uint8_t pendingPacket[256];            // Buffered packet waiting to be sent
    size_t pendingPacketLen = 0;           // Length of pending packet
    bool hasPendingPacket = false;         // Whether we have a packet waiting
    
    // Stats
    unsigned long packetsReceived = 0;
    unsigned long lastPacketMs = 0;
    
    // Parse a line and either send immediately or queue with delay
    void processLine(const char* line);
    
    // Parse hex string to bytes
    static size_t hexToBytes(const char* hex, uint8_t* out, size_t maxLen);
    
    // Send packet to BLE queue
    void sendPacket(const uint8_t* data, size_t len);
};

// Global instance
extern BLESerialModule bleSerialModule;
