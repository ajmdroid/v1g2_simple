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
    ble_ = bleClient;
    parser_ = parserPtr;
    profiles_ = profileMgr;
    preview_ = previewModule;
    power_ = powerModule;
    bus_ = eventBus;
    config_ = cfg;

    queueHandle_ = xQueueCreate(config_.queueDepth, sizeof(BLEDataPacket));
    rxReadPos_ = 0;
    rxBuffer_.reserve(std::max(config_.rxBufferCap, RX_BUFFER_MAX));
    backpressureActive_ = false;
}

void BleQueueModule::onNotify(const uint8_t* data, size_t length, uint16_t charUUID) {
    if (!queueHandle_) return;

    if (length > 0 && length <= sizeof(BLEDataPacket::data)) {
        PERF_INC(rxPackets);
        PERF_ADD(rxBytes, length);
        BLEDataPacket pkt;
        memcpy(pkt.data, data, length);
        pkt.length = length;
        pkt.charUUID = charUUID;
        pkt.tsMs = millis();

        BaseType_t result = xQueueSend(queueHandle_, &pkt, 0);
        if (result != pdTRUE) {
            PERF_INC(queueDrops);
            BLEDataPacket dropped;
            xQueueReceive(queueHandle_, &dropped, 0);
            xQueueSend(queueHandle_, &pkt, 0);
        }
        UBaseType_t depth = uxQueueMessagesWaiting(queueHandle_);
        PERF_MAX(queueHighWater, depth);
    } else if (length > sizeof(BLEDataPacket::data)) {
        PERF_INC(oversizeDrops);
    }
}

void BleQueueModule::refreshBackpressureState() {
    const size_t unreadBytes = (rxReadPos_ < rxBuffer_.size()) ? (rxBuffer_.size() - rxReadPos_) : 0;
    const UBaseType_t queueDepth = queueHandle_ ? uxQueueMessagesWaiting(queueHandle_) : 0;
    const size_t queuePressureThreshold = std::max<size_t>(4, config_.queueDepth / 4);
    static constexpr size_t RX_BACKPRESSURE_BYTES = 192;
    backpressureActive_ =
        (unreadBytes >= RX_BACKPRESSURE_BYTES) ||
        (static_cast<size_t>(queueDepth) >= queuePressureThreshold);
}

void BleQueueModule::process() {
    bool previewActive = preview_ && preview_->isRunning();
    UBaseType_t queueDepthBeforeDrain = 0;
    bool parsedEventPending = false;
    uint16_t parsedEventDetail = 0;

#ifdef REPLAY_MODE
    processReplayData();
#else
    BLEDataPacket pkt;
    uint32_t latestPktTs = 0;
    queueDepthBeforeDrain = queueHandle_ ? uxQueueMessagesWaiting(queueHandle_) : 0;

    while (queueHandle_ && xQueueReceive(queueHandle_, &pkt, 0) == pdTRUE) {
        appendRxClamped(rxBuffer_, rxReadPos_, pkt.data, pkt.length);
        latestPktTs = pkt.tsMs;
    }

    if (ble_) {
        ble_->processProxyQueue();
    }

    if (latestPktTs != 0) {
        lastRxMillis_ = latestPktTs;
        lastNotifyTsMs_ = latestPktTs;
        perfRecordBleTimelineEvent(PerfBleTimelineEvent::FirstRx, latestPktTs);
    }
#endif

    if (rxReadPos_ >= rxBuffer_.size()) {
        rxBuffer_.clear();
        rxReadPos_ = 0;
        refreshBackpressureState();
        return;
    }

    size_t availableBytes = rxBuffer_.size() - rxReadPos_;
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
    const size_t queueHalfThreshold = std::max<size_t>(4, config_.queueDepth / 2);
    const size_t queueHighThreshold = std::max<size_t>(6, (config_.queueDepth * 3) / 4);

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

        availableBytes = rxBuffer_.size() - rxReadPos_;
        if (availableBytes == 0) break;

        const uint8_t* dataBegin = rxBuffer_.data() + rxReadPos_;
        const uint8_t* startPtr = (rxBuffer_[rxReadPos_] == ESP_PACKET_START)
            ? dataBegin
            : static_cast<const uint8_t*>(memchr(dataBegin, ESP_PACKET_START, availableBytes));
        if (startPtr == nullptr) {
            rxBuffer_.clear();
            rxReadPos_ = 0;
            break;
        }
        if (startPtr != dataBegin) {
            rxReadPos_ = static_cast<size_t>(startPtr - rxBuffer_.data());
            continue;
        }
        if (availableBytes < MIN_HEADER_SIZE) {
            break;
        }

        uint8_t lenField = rxBuffer_[rxReadPos_ + 4];
        if (lenField == 0) {
            PERF_INC(parseResyncs);
            rxReadPos_++;
            continue;
        }

        size_t packetSize = 6 + lenField;
        if (packetSize > MAX_PACKET_SIZE) {
            Serial.printf("[BLE] WARN: BLE packet too large (%u bytes) - resyncing\n", (unsigned)packetSize);
            PERF_INC(parseResyncs);
            rxReadPos_++;
            continue;
        }
        if (availableBytes < packetSize) {
            break;
        }
        if (rxBuffer_[rxReadPos_ + packetSize - 1] != ESP_PACKET_END) {
            Serial.println("[BLE] WARN: Packet missing end marker - resyncing");
            PERF_INC(parseResyncs);
            rxReadPos_++;
            continue;
        }

        const uint8_t* packetPtr = rxBuffer_.data() + rxReadPos_;

        lastRxMillis_ = millis();

        if (packetSize >= 12 && packetPtr[3] == PACKET_ID_RESP_USER_BYTES && profiles_) {
            uint8_t userBytes[6];
            memcpy(userBytes, &packetPtr[5], 6);
            ble_->onUserBytesReceived(userBytes);
            profiles_->setCurrentSettings(userBytes);
            rxReadPos_ += packetSize;
            packetsProcessedThisCycle++;
            continue;
        }

        uint8_t packetId = packetPtr[3];
        bool parseOk = parser_->parse(packetPtr, packetSize);

        if (packetId == PACKET_ID_DISPLAY_DATA || packetId == PACKET_ID_ALERT_DATA) {
            if (parseOk) {
                PERF_INC(parseSuccesses);
            } else {
                PERF_INC(parseFailures);
            }
        }

        rxReadPos_ += packetSize;
        packetsProcessedThisCycle++;

        if (parseOk) {
            if (power_) {
                power_->onV1DataReceived();
            }
            // Only cancel preview when V1 has an actual alert (not on every packet)
            // This allows color preview to run while V1 is connected but resting
            if (previewActive && preview_ && parser_->getAlertCount() > 0) {
                preview_->cancel();
                previewActive = false;
            }
            // Set flag and timestamp for main loop to drive display pipeline
            // This decouples BLE processing from slow display updates
            hadSuccessfulParse_ = true;
            lastParsedTsMs_ = lastNotifyTsMs_;
            parsedEventPending = true;
            parsedEventDetail = packetId;
        }
    }

    if (parsedEventPending && bus_) {
        SystemEvent event;
        event.type = SystemEventType::BLE_FRAME_PARSED;
        event.tsMs = lastParsedTsMs_;
        event.seq = ++parsedEventSeq_;
        event.detail = parsedEventDetail;
        bus_->publish(event);
    }

    if (rxReadPos_ >= rxBuffer_.size()) {
        rxBuffer_.clear();
        rxReadPos_ = 0;
    } else if (rxReadPos_ >= RX_COMPACT_THRESHOLD) {
        compactRxBuffer(rxBuffer_, rxReadPos_);
    }

    refreshBackpressureState();
}

#ifdef REPLAY_MODE
void BleQueueModule::processReplayData() {
    unsigned long now = millis();
    const ReplayPacket& pkt = REPLAY_SEQUENCE[replayIndex_];
    if (now - lastReplayTime_ < pkt.delayMs) {
        return;
    }

    appendRxClamped(rxBuffer_, rxReadPos_, pkt.data, pkt.length);
    Serial.printf("[REPLAY] Injected packet %d/%d (%d bytes)\n",
                  (int)(replayIndex_ + 1), (int)REPLAY_SEQUENCE_LENGTH, (int)pkt.length);

    lastReplayTime_ = now;
    replayIndex_ = (replayIndex_ + 1) % REPLAY_SEQUENCE_LENGTH;
}
#endif
