#include "ble_queue_module.h"
#include "perf_metrics.h"
#include "../../../include/config.h"
#include "modules/system/system_event_bus.h"
#ifndef UNIT_TEST
#include "v1_profiles.h"
#include "modules/display/display_preview_module.h"
#include "modules/power/power_module.h"
#endif
#include <algorithm>
#include <cstring>

// Maximum bytes to buffer from BLE RX before dropping
static constexpr size_t RX_BUFFER_MAX = 1024;
static constexpr size_t RX_COMPACT_THRESHOLD = RX_BUFFER_MAX / 2;

static void compactRxBuffer(std::vector<uint8_t>& rxBuffer, size_t& readPos) {
    if (readPos == 0) {
        return;
    }
    if (readPos >= rxBuffer.size()) {
        rxBuffer.clear();
        readPos = 0;
        return;
    }
    const size_t unread = rxBuffer.size() - readPos;
    memmove(rxBuffer.data(), rxBuffer.data() + readPos, unread);
    rxBuffer.resize(unread);
    readPos = 0;
}

static size_t appendRxClamped(std::vector<uint8_t>& rxBuffer,
                              size_t& readPos,
                              const uint8_t* data,
                              size_t length) {
    if (!data || length == 0) {
        return 0;
    }

    if (readPos > 0) {
        const size_t unread = (readPos < rxBuffer.size()) ? (rxBuffer.size() - readPos) : 0;
        if (rxBuffer.size() >= RX_BUFFER_MAX || (unread + length) > RX_BUFFER_MAX) {
            compactRxBuffer(rxBuffer, readPos);
        }
    }

    const size_t unread = (readPos < rxBuffer.size()) ? (rxBuffer.size() - readPos) : 0;
    if (unread >= RX_BUFFER_MAX) {
        return 0;
    }
    const size_t remaining = RX_BUFFER_MAX - unread;
    const size_t toCopy = std::min(remaining, length);
    rxBuffer.insert(rxBuffer.end(), data, data + toCopy);
    return toCopy;
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
                           PowerModule* powerModule,
                           SystemEventBus* eventBus,
                           Config cfg) {
    ble = bleClient;
    parser = parserPtr;
    profiles = profileMgr;
    preview = previewModule;
    power = powerModule;
    bus = eventBus;
    config = cfg;

    queueHandle = xQueueCreate(config.queueDepth, sizeof(BLEDataPacket));
    rxReadPos = 0;
    rxBuffer.reserve(std::max(config.rxBufferCap, RX_BUFFER_MAX));
    backpressureActive = false;
}

void BleQueueModule::onNotify(const uint8_t* data, size_t length, uint16_t charUUID) {
    if (!queueHandle) return;

    if (length > 0 && length <= sizeof(BLEDataPacket::data)) {
        PERF_INC(rxPackets);
        PERF_ADD(rxBytes, length);
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
    }
}

void BleQueueModule::refreshBackpressureState() {
    const size_t unreadBytes = (rxReadPos < rxBuffer.size()) ? (rxBuffer.size() - rxReadPos) : 0;
    const UBaseType_t queueDepth = queueHandle ? uxQueueMessagesWaiting(queueHandle) : 0;
    const size_t queuePressureThreshold = std::max<size_t>(4, config.queueDepth / 4);
    static constexpr size_t RX_BACKPRESSURE_BYTES = 192;
    backpressureActive =
        (unreadBytes >= RX_BACKPRESSURE_BYTES) ||
        (static_cast<size_t>(queueDepth) >= queuePressureThreshold);
}

void BleQueueModule::process() {
    bool previewActive = preview && preview->isRunning();
    UBaseType_t queueDepthBeforeDrain = 0;
    bool parsedEventPending = false;
    uint16_t parsedEventDetail = 0;
    
#ifdef REPLAY_MODE
    processReplayData();
#else
    BLEDataPacket pkt;
    uint32_t latestPktTs = 0;
    queueDepthBeforeDrain = queueHandle ? uxQueueMessagesWaiting(queueHandle) : 0;

    while (queueHandle && xQueueReceive(queueHandle, &pkt, 0) == pdTRUE) {
        appendRxClamped(rxBuffer, rxReadPos, pkt.data, pkt.length);
        latestPktTs = pkt.tsMs;
    }

    if (ble) {
        ble->processProxyQueue();
    }

    if (latestPktTs != 0) {
        lastRxMillis = latestPktTs;
        lastNotifyTsMs = latestPktTs;
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::FirstRx, latestPktTs);
    }
#endif

    if (rxReadPos >= rxBuffer.size()) {
        rxBuffer.clear();
        rxReadPos = 0;
        refreshBackpressureState();
        return;
    }

    size_t availableBytes = rxBuffer.size() - rxReadPos;
    if (availableBytes == 0) {
        refreshBackpressureState();
        return;
    }

    const size_t MIN_HEADER_SIZE = 6;
    const size_t MAX_PACKET_SIZE = 512;
    
    // Adaptive drain budget: keep a low-latency baseline but accelerate when
    // queue/backlog indicates BLE ingest is falling behind.
    static constexpr size_t BASE_PACKETS_PER_CYCLE = 8;
    static constexpr size_t MID_PACKETS_PER_CYCLE = 16;
    static constexpr size_t HIGH_PACKETS_PER_CYCLE = 24;
    static constexpr size_t MAX_PACKETS_PER_CYCLE = 32;
    static constexpr uint32_t BASE_PARSE_BUDGET_US = 2500;
    static constexpr uint32_t BURST_PARSE_BUDGET_US = 7000;
    static constexpr size_t MID_BACKLOG_BYTES = 192;
    static constexpr size_t HIGH_BACKLOG_BYTES = 320;
    static constexpr size_t MAX_BACKLOG_BYTES = 448;

    const size_t queueDepthSnapshot = static_cast<size_t>(queueDepthBeforeDrain);
    const size_t queueHalfThreshold = std::max<size_t>(4, config.queueDepth / 2);
    const size_t queueHighThreshold = std::max<size_t>(6, (config.queueDepth * 3) / 4);

    size_t maxPacketsPerCycle = BASE_PACKETS_PER_CYCLE;
    if (availableBytes >= MID_BACKLOG_BYTES || queueDepthSnapshot >= queueHalfThreshold) {
        maxPacketsPerCycle = MID_PACKETS_PER_CYCLE;
    }
    if (availableBytes >= HIGH_BACKLOG_BYTES || queueDepthSnapshot >= queueHighThreshold) {
        maxPacketsPerCycle = HIGH_PACKETS_PER_CYCLE;
    }
    if (availableBytes >= MAX_BACKLOG_BYTES && queueDepthSnapshot >= queueHighThreshold) {
        maxPacketsPerCycle = MAX_PACKETS_PER_CYCLE;
    }
    if (previewActive && maxPacketsPerCycle > MID_PACKETS_PER_CYCLE) {
        maxPacketsPerCycle = MID_PACKETS_PER_CYCLE;
    }

    const uint32_t parseCycleStartUs = PERF_TIMESTAMP_US();
    const uint32_t parseBudgetUs =
        (maxPacketsPerCycle > BASE_PACKETS_PER_CYCLE) ? BURST_PARSE_BUDGET_US : BASE_PARSE_BUDGET_US;
    size_t packetsProcessedThisCycle = 0;

    while (true) {
        if (packetsProcessedThisCycle >= maxPacketsPerCycle) {
            break;
        }
        if (packetsProcessedThisCycle >= BASE_PACKETS_PER_CYCLE &&
            (PERF_TIMESTAMP_US() - parseCycleStartUs) >= parseBudgetUs) {
            break;
        }
        
        availableBytes = rxBuffer.size() - rxReadPos;
        if (availableBytes == 0) break;

        auto dataBegin = rxBuffer.begin() + rxReadPos;
        auto startIt = (rxBuffer[rxReadPos] == ESP_PACKET_START)
            ? dataBegin
            : std::find(dataBegin, rxBuffer.end(), ESP_PACKET_START);
        if (startIt == rxBuffer.end()) {
            rxBuffer.clear();
            rxReadPos = 0;
            break;
        }
        if (startIt != dataBegin) {
            rxReadPos = static_cast<size_t>(startIt - rxBuffer.begin());
            continue;
        }
        if (availableBytes < MIN_HEADER_SIZE) {
            break;
        }

        uint8_t lenField = rxBuffer[rxReadPos + 4];
        if (lenField == 0) {
            rxReadPos++;
            continue;
        }

        size_t packetSize = 6 + lenField;
        if (packetSize > MAX_PACKET_SIZE) {
            Serial.printf("WARNING: BLE packet too large (%u bytes) - resyncing\n", (unsigned)packetSize);
            rxReadPos++;
            continue;
        }
        if (availableBytes < packetSize) {
            break;
        }
        if (rxBuffer[rxReadPos + packetSize - 1] != ESP_PACKET_END) {
            Serial.println("WARNING: Packet missing end marker - resyncing");
            rxReadPos++;
            continue;
        }

        const uint8_t* packetPtr = rxBuffer.data() + rxReadPos;

        lastRxMillis = millis();

        if (packetSize >= 12 && packetPtr[3] == PACKET_ID_RESP_USER_BYTES && profiles) {
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            ble->onUserBytesReceived(userBytes);
            profiles->setCurrentSettings(userBytes);
            rxReadPos += packetSize;
            packetsProcessedThisCycle++;
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

        rxReadPos += packetSize;
        packetsProcessedThisCycle++;

        if (parseOk) {
            if (power) {
                power->onV1DataReceived();
            }
            // Only cancel preview when V1 has an actual alert (not on every packet)
            // This allows color preview to run while V1 is connected but resting
            if (previewActive && preview && parser->getAlertCount() > 0) {
                preview->cancel();
                previewActive = false;
            }
            // Set flag and timestamp for main loop to drive display pipeline
            // This decouples BLE processing from slow display updates
            hadSuccessfulParse = true;
            lastParsedTsMs = lastNotifyTsMs;
            parsedEventPending = true;
            parsedEventDetail = packetId;
        }
    }

    if (parsedEventPending && bus) {
        SystemEvent event;
        event.type = SystemEventType::BLE_FRAME_PARSED;
        event.tsMs = lastParsedTsMs;
        event.seq = ++parsedEventSeq;
        event.detail = parsedEventDetail;
        bus->publish(event);
    }

    if (rxReadPos >= rxBuffer.size()) {
        rxBuffer.clear();
        rxReadPos = 0;
    } else if (rxReadPos >= RX_COMPACT_THRESHOLD) {
        compactRxBuffer(rxBuffer, rxReadPos);
    }

    refreshBackpressureState();
}

#ifdef REPLAY_MODE
void BleQueueModule::processReplayData() {
    unsigned long now = millis();
    const ReplayPacket& pkt = REPLAY_SEQUENCE[replayIndex];
    if (now - lastReplayTime < pkt.delayMs) {
        return;
    }

    appendRxClamped(rxBuffer, rxReadPos, pkt.data, pkt.length);
    Serial.printf("[REPLAY] Injected packet %d/%d (%d bytes)\n",
                  (int)(replayIndex + 1), (int)REPLAY_SEQUENCE_LENGTH, (int)pkt.length);

    lastReplayTime = now;
    replayIndex = (replayIndex + 1) % REPLAY_SEQUENCE_LENGTH;
}
#endif
