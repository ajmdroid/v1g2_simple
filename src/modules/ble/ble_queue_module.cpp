#include "ble_queue_module.h"
#include "perf_metrics.h"
#include "../../../include/config.h"
#include "debug_logger.h"
#include <algorithm>

// External debug logger instance
extern DebugLogger debugLogger;

// Helper to log raw BLE packet as hex for replay capture
// Format: [BLE:PKT] ts=12345 len=19 hex=AA040A430C...
static void logPacketHex(uint32_t timestamp, const uint8_t* data, size_t length) {
    if (!debugLogger.isEnabledFor(DebugLogCategory::Ble)) return;
    if (length == 0 || length > 128) return;  // Sanity check
    
    // Build hex string (2 chars per byte + null)
    char hexBuf[257];  // 128 bytes * 2 + null
    for (size_t i = 0; i < length && i < 128; i++) {
        snprintf(&hexBuf[i * 2], 3, "%02X", data[i]);
    }
    hexBuf[length * 2] = '\0';
    
    debugLogger.logf(DebugLogCategory::Ble, "[BLE:PKT] ts=%lu len=%u hex=%s",
                     (unsigned long)timestamp, (unsigned)length, hexBuf);
}

// Helper to parse hex string to bytes
static size_t hexToBytes(const char* hex, uint8_t* out, size_t maxLen) {
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

#ifdef REPLAY_MODE
// Sample replay data for UI testing
static const uint8_t REPLAY_PACKET_KA_ALERT[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,
    0x04, 0x01, 0x05, 0x00,
    0x00, 0xD0, 0x2F, 0x01,
    0x00, 0x00, 0x00, 0x01,
    0xE8, 0xAB
};

static const uint8_t REPLAY_PACKET_DISPLAY_MUTED[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,
    0x04, 0x01, 0x03, 0x01,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x8A, 0xAB
};

static const uint8_t REPLAY_PACKET_DISPLAY_X[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,
    0x01, 0x01, 0x04, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x7E, 0xAB
};

static const uint8_t REPLAY_PACKET_K_ALERT[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,
    0x02, 0x02, 0x00, 0x03,
    0x00, 0x6C, 0xBE, 0x03,
    0x00, 0x00, 0x00, 0x02,
    0xD9, 0xAB
};

static const uint8_t REPLAY_PACKET_LASER[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,
    0x08, 0x01, 0x08, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01,
    0xA8, 0xAB
};

static const uint8_t REPLAY_PACKET_CLEAR[] = {
    0xAA, 0x04, 0x0A, 0x31, 0x08,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x73, 0xAB
};

struct ReplayPacket {
    const uint8_t* data;
    size_t length;
    unsigned long delayMs;
};

static const ReplayPacket REPLAY_SEQUENCE[] = {
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 2000},
    {REPLAY_PACKET_KA_ALERT, sizeof(REPLAY_PACKET_KA_ALERT), 100},
    {REPLAY_PACKET_DISPLAY_MUTED, sizeof(REPLAY_PACKET_DISPLAY_MUTED), 1000},
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 1500},
    {REPLAY_PACKET_DISPLAY_X, sizeof(REPLAY_PACKET_DISPLAY_X), 100},
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 2000},
    {REPLAY_PACKET_K_ALERT, sizeof(REPLAY_PACKET_K_ALERT), 100},
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 1500},
    {REPLAY_PACKET_LASER, sizeof(REPLAY_PACKET_LASER), 100},
    {REPLAY_PACKET_CLEAR, sizeof(REPLAY_PACKET_CLEAR), 3000},
};

static const size_t REPLAY_SEQUENCE_LENGTH = sizeof(REPLAY_SEQUENCE) / sizeof(REPLAY_SEQUENCE[0]);
#endif

void BleQueueModule::begin(V1BLEClient* bleClient,
                           PacketParser* parserPtr,
                           V1ProfileManager* profileMgr,
                           DisplayPreviewModule* previewModule,
                           DisplayPipelineModule* displayPipelineModule,
                           PowerModule* powerModule,
                           Config cfg) {
    ble = bleClient;
    parser = parserPtr;
    profiles = profileMgr;
    preview = previewModule;
    displayPipeline = displayPipelineModule;
    power = powerModule;
    config = cfg;

    queueHandle = xQueueCreate(config.queueDepth, sizeof(BLEDataPacket));
    rxBuffer.reserve(config.rxBufferCap);
}

void BleQueueModule::onNotify(const uint8_t* data, size_t length, uint16_t charUUID) {
    if (!queueHandle) return;

    if (length > 0 && length <= sizeof(BLEDataPacket::data)) {
        PERF_INC(rxPackets);
        BLEDataPacket pkt;
        memcpy(pkt.data, data, length);
        pkt.length = length;
        pkt.charUUID = charUUID;
        pkt.tsMs = millis();

        BaseType_t result = xQueueSend(queueHandle, &pkt, 0);
        if (result != pdTRUE) {
            PERF_INC(queueDrops);
            BLEDataPacket dropped;
            xQueueReceive(queueHandle, &dropped, 0);
            xQueueSend(queueHandle, &pkt, 0);
        }
        UBaseType_t depth = uxQueueMessagesWaiting(queueHandle);
        PERF_MAX(queueHighWater, depth);
    } else if (length > sizeof(BLEDataPacket::data)) {
        PERF_INC(oversizeDrops);
        Serial.printf("[BLE] WARNING: Dropped oversize packet (%d bytes > %d max)\n",
                      (int)length, (int)sizeof(BLEDataPacket::data));
    }
}

void BleQueueModule::process() {
    bool previewActive = preview && preview->isRunning();
    
#ifdef REPLAY_MODE
    processReplayData();
#elif defined(SERIAL_REPLAY_MODE)
    // Serial replay mode - read packets from USB serial
    processSerialReplay();
#else
    BLEDataPacket pkt;
    uint32_t latestPktTs = 0;

    constexpr size_t RX_BUFFER_MAX = 512;
    if (rxBuffer.capacity() < RX_BUFFER_MAX) {
        rxBuffer.reserve(RX_BUFFER_MAX);
    }

    while (queueHandle && xQueueReceive(queueHandle, &pkt, 0) == pdTRUE) {
        if (rxBuffer.size() < RX_BUFFER_MAX) {
            rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
        }
        latestPktTs = pkt.tsMs;
    }

    if (ble) {
        ble->processProxyQueue();
    }

    if (latestPktTs != 0) {
        lastRxMillis = latestPktTs;
    }
#endif

    if (rxBuffer.empty()) {
        return;
    }

    if (rxBuffer.size() > 256) {
        if (rxBuffer.size() > config.rxTrimKeep) {
            rxBuffer.erase(rxBuffer.begin(), rxBuffer.end() - config.rxTrimKeep);
        }
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
            Serial.printf("WARNING: BLE packet too large (%u bytes) - resyncing\n", (unsigned)packetSize);
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }
        if (rxBuffer.size() < packetSize) {
            break;
        }
        if (rxBuffer[packetSize - 1] != ESP_PACKET_END) {
            Serial.println("WARNING: Packet missing end marker - resyncing");
            rxBuffer.erase(rxBuffer.begin());
            continue;
        }

        const uint8_t* packetPtr = rxBuffer.data();

        lastRxMillis = millis();

        // Log raw packet hex for replay capture (gated by BLE debug category)
        logPacketHex(lastRxMillis, packetPtr, packetSize);

        if (packetSize >= 12 && packetPtr[3] == PACKET_ID_RESP_USER_BYTES && profiles) {
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            ble->onUserBytesReceived(userBytes);
            profiles->setCurrentSettings(userBytes);
            rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);
            continue;
        }

        uint8_t packetId = packetPtr[3];
        bool parseOk = parser->parse(packetPtr, packetSize);

        if (packetId == PACKET_ID_DISPLAY_DATA || packetId == PACKET_ID_ALERT_DATA) {
            if (parseOk) {
                PERF_INC(parseSuccesses);
            } else {
                PERF_INC(parseFailures);
            }
        }

        rxBuffer.erase(rxBuffer.begin(), rxBuffer.begin() + packetSize);

        if (parseOk) {
            if (power) {
                power->onV1DataReceived();
            }
            // Cancel preview immediately when live V1 data arrives
            if (previewActive && preview) {
                preview->cancel();
                previewActive = false;
            }
            if (displayPipeline) {
                displayPipeline->handleParsed(millis());
            }
        }
    }
}

#ifdef REPLAY_MODE
void BleQueueModule::processReplayData() {
    unsigned long now = millis();
    const ReplayPacket& pkt = REPLAY_SEQUENCE[replayIndex];
    if (now - lastReplayTime < pkt.delayMs) {
        return;
    }

    rxBuffer.insert(rxBuffer.end(), pkt.data, pkt.data + pkt.length);
    Serial.printf("[REPLAY] Injected packet %d/%d (%d bytes)\n",
                  (int)(replayIndex + 1), (int)REPLAY_SEQUENCE_LENGTH, (int)pkt.length);

    lastReplayTime = now;
    replayIndex = (replayIndex + 1) % REPLAY_SEQUENCE_LENGTH;
}
#endif

#ifdef SERIAL_REPLAY_MODE
void BleQueueModule::processSerialReplay() {
    // Read lines from Serial in format: "hex=AABBCCDD..." or just raw hex "AABBCCDD"
    // This allows the replay_ble.py tool to inject packets
    static char lineBuf[600];  // Max 256 bytes = 512 hex chars + prefix
    static size_t linePos = 0;
    
    while (Serial.available()) {
        char c = Serial.read();
        
        if (c == '\n' || c == '\r') {
            if (linePos > 0) {
                lineBuf[linePos] = '\0';
                
                // Find hex data - look for "hex=" prefix or treat whole line as hex
                const char* hexStart = lineBuf;
                if (strstr(lineBuf, "hex=") != nullptr) {
                    hexStart = strstr(lineBuf, "hex=") + 4;
                }
                
                // Skip any whitespace
                while (*hexStart == ' ' || *hexStart == '\t') hexStart++;
                
                // Parse hex to bytes
                uint8_t pktBuf[256];
                size_t pktLen = hexToBytes(hexStart, pktBuf, sizeof(pktBuf));
                
                if (pktLen >= 6 && pktBuf[0] == 0xAA && pktBuf[pktLen - 1] == 0xAB) {
                    // Valid packet framing
                    rxBuffer.insert(rxBuffer.end(), pktBuf, pktBuf + pktLen);
                    Serial.printf("[SERIAL_REPLAY] Injected %d bytes\n", (int)pktLen);
                    lastRxMillis = millis();
                } else if (pktLen > 0) {
                    Serial.printf("[SERIAL_REPLAY] Invalid packet (%d bytes, start=0x%02X)\n", 
                                  (int)pktLen, pktLen > 0 ? pktBuf[0] : 0);
                }
                
                linePos = 0;
            }
        } else if (linePos < sizeof(lineBuf) - 1) {
            lineBuf[linePos++] = c;
        }
    }
}
#endif
