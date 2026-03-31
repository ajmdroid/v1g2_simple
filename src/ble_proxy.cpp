/**
 * BLE Proxy Server Implementation
 * Handles BLE server mode for proxying V1 data to companion apps
 *
 * Extracted from ble_client.cpp — proxy server callbacks, forwarding,
 * phone command queue.
 */

#include "ble_client.h"
#include "../include/ble_internals.h"
#include "../include/config.h"
#include "perf_metrics.h"
#include <esp_heap_caps.h>
#include <algorithm>
#include <cstring>

// ============================================================================
// BLE Proxy Server Functions
// ============================================================================

void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo) {
    // NOTE: BLE callback - keep fast
    if (BLE_CALLBACK_LOGS) {
        BLE_LOGF("[BLE] App connected_ (handle: %d)\n", connInfo.getConnHandle());
    }

    if (!bleClient) {
        return;
    }

    ProxyCallbackEvent event{};
    event.type = ProxyCallbackEventType::APP_CONNECTED;
    event.connHandle = connInfo.getConnHandle();
    bleClient->enqueueProxyCallbackEvent(event);
}

void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer* pServer_, NimBLEConnInfo& connInfo, int reason) {
    // NOTE: BLE callback - keep fast
    if (BLE_CALLBACK_LOGS) {
        BLE_LOGF("[BLE] App disconnected (reason: %d)\n", reason);
    }
    if (!bleClient) {
        return;
    }

    ProxyCallbackEvent event{};
    event.type = ProxyCallbackEventType::APP_DISCONNECTED;
    event.reason = reason;
    bleClient->enqueueProxyCallbackEvent(event);
}

void V1BLEClient::deferLastV1Address(const char* addr) {
    if (!addr || addr[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&pendingAddrMux);
    snprintf(pendingLastV1Address_, sizeof(pendingLastV1Address_), "%s", addr);
    pendingLastV1AddressValid_ = true;
    portEXIT_CRITICAL(&pendingAddrMux);
}

uint32_t V1BLEClient::getPhoneCmdDropsOverflow() const {
    return perfPhoneCmdDropMetricsSnapshot().overflow;
}

uint32_t V1BLEClient::getPhoneCmdDropsInvalid() const {
    return perfPhoneCmdDropMetricsSnapshot().invalid;
}

uint32_t V1BLEClient::getPhoneCmdDropsBleFail() const {
    return perfPhoneCmdDropMetricsSnapshot().bleFail;
}

uint32_t V1BLEClient::getPhoneCmdDropsLockBusy() const {
    return perfPhoneCmdDropMetricsSnapshot().lockBusy;
}

bool V1BLEClient::enqueueProxyCallbackEvent(const ProxyCallbackEvent& event) {
    taskENTER_CRITICAL(&proxyCallbackEventMux_);
    if (proxyCallbackEventQueueCount_ >= PROXY_CALLBACK_EVENT_QUEUE_DEPTH) {
        proxyCallbackEventQueueHead_ =
            (proxyCallbackEventQueueHead_ + 1) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
        proxyCallbackEventQueueCount_--;
    }

    const size_t tail =
        (proxyCallbackEventQueueHead_ + proxyCallbackEventQueueCount_) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
    proxyCallbackEventQueue_[tail] = event;
    proxyCallbackEventQueueCount_++;
    taskEXIT_CRITICAL(&proxyCallbackEventMux_);
    return true;
}

bool V1BLEClient::popProxyCallbackEvent(ProxyCallbackEvent& event) {
    taskENTER_CRITICAL(&proxyCallbackEventMux_);
    if (proxyCallbackEventQueueCount_ == 0) {
        taskEXIT_CRITICAL(&proxyCallbackEventMux_);
        return false;
    }

    event = proxyCallbackEventQueue_[proxyCallbackEventQueueHead_];
    proxyCallbackEventQueueHead_ =
        (proxyCallbackEventQueueHead_ + 1) % PROXY_CALLBACK_EVENT_QUEUE_DEPTH;
    proxyCallbackEventQueueCount_--;
    taskEXIT_CRITICAL(&proxyCallbackEventMux_);
    return true;
}

void V1BLEClient::clearProxyAdvertisingSchedule() {
    proxyAdvertisingStartMs_ = 0;
    proxyAdvertisingStartReasonCode_ =
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
}

void V1BLEClient::clearProxyAdvertisingWindowState() {
    proxyAdvertisingWindowStartMs_ = 0;
    proxyAdvertisingRetryAtMs_ = 0;
}

void V1BLEClient::stopProxyAdvertisingFromMainLoop(uint8_t reasonCode) {
    clearProxyAdvertisingSchedule();
    clearProxyAdvertisingWindowState();

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv && pAdv->isAdvertising()) {
        NimBLEDevice::stopAdvertising();
        perfRecordProxyAdvertisingTransition(false, reasonCode, millis());
    }
}

void V1BLEClient::handleProxyCallbackEvent(const ProxyCallbackEvent& event) {
    switch (event.type) {
        case ProxyCallbackEventType::APP_CONNECTED:
            if (pServer_ && pServer_->getConnectedCount() > 0) {
                // Request Android-compatible connection parameters from the main loop.
                pServer_->updateConnParams(event.connHandle, 12, 36, 0, 400);
            }
            setProxyClientConnected(true);
            proxySuppressedForObdHold_ = false;
            proxyDisconnectRequestedForObdPreempt_ = false;
            proxySuppressedResumeReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
            clearProxyAdvertisingWindowState();
            perfRecordProxyAdvertisingTransition(
                false,
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopAppConnected),
                millis());
            return;

        case ProxyCallbackEventType::APP_DISCONNECTED:
            proxyClientConnected_ = false;
            proxyDisconnectRequestedForObdPreempt_ = false;
            clearProxyAdvertisingWindowState();
            if (connected_.load(std::memory_order_relaxed) && !wifiPriorityMode_) {
                if (obdBleArbitrationRequest_ == ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD ||
                    obdBleArbitrationRequest_ == ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN) {
                    proxySuppressedForObdHold_ = true;
                    proxySuppressedResumeReasonCode_ =
                        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartAppDisconnect);
                    clearProxyAdvertisingSchedule();
                    return;
                }
                proxyAdvertisingStartMs_ = millis();
                proxyAdvertisingStartReasonCode_ =
                    static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartAppDisconnect);
            }
            return;

        case ProxyCallbackEventType::V1_DISCONNECTED:
            proxyClientConnected_ = false;
            proxySuppressedForObdHold_ = false;
            proxyDisconnectRequestedForObdPreempt_ = false;
            proxySuppressedResumeReasonCode_ =
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
            stopProxyAdvertisingFromMainLoop(
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopV1Disconnect));
            return;
    }
}

void V1BLEClient::drainProxyCallbackEvents() {
    ProxyCallbackEvent event{};
    while (popProxyCallbackEvent(event)) {
        handleProxyCallbackEvent(event);
    }
}

void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    // Forward commands from app to V1
    // NOTE: This is a BLE callback - avoid blocking operations (Serial, delays, long locks)
    if (!pCharacteristic || !bleClient) {
        return;
    }

    if (!bleClient->connected_.load(std::memory_order_relaxed)) {
        return;
    }

    // Get the raw data pointer and length directly
    NimBLEAttValue attrValue = pCharacteristic->getValue();
    const uint8_t* rawData = attrValue.data();
    size_t rawLen = attrValue.size();

    if (rawLen == 0 || !rawData || rawLen > 32) {
        return;
    }

    // Copy to a safe buffer and forward to V1
    uint16_t sourceChar = shortUuid(pCharacteristic->getUUID());
    uint8_t cmdBuf[32];
    memcpy(cmdBuf, rawData, rawLen);

    // Defer proxy command logging to main loop (avoid Serial in BLE callback)
    uint8_t packetId = (rawLen >= 4 && cmdBuf[0] == ESP_PACKET_START) ? cmdBuf[3] : 0;
    if (packetId != 0) {
        portENTER_CRITICAL(&proxyCmdMux);
        if (!bleClient->proxyCmdPending_) {
            bleClient->proxyCmdPending_ = true;
            bleClient->proxyCmdId_ = packetId;
            bleClient->proxyCmdLen_ = static_cast<uint8_t>(std::min<size_t>(rawLen, sizeof(bleClient->proxyCmdBuf_)));
            memcpy(bleClient->proxyCmdBuf_, cmdBuf, bleClient->proxyCmdLen_);
        }
        portEXIT_CRITICAL(&proxyCmdMux);
    }
    // Proxy command logging disabled - app uses standard mute (0x34/0x35)
    // Uncomment to debug: snprintf(logBuf, ...) with packet ID at cmdBuf[3]

    // Enqueue for main-loop processing to avoid BLE callback blocking
    bleClient->enqueuePhoneCommand(cmdBuf, rawLen, sourceChar);
}

bool V1BLEClient::allocateProxyQueues() {
    if (proxyQueue_ && phone2v1Queue_) {
        return true;
    }

    releaseProxyQueues();

    bool proxyQueueAllocatedInPsram = false;
    bool phoneQueueAllocatedInPsram = false;

    auto allocateBuffer = [&](size_t count, const char* label, bool& allocatedInPsram) -> ProxyPacket* {
        const size_t bytes = sizeof(ProxyPacket) * count;
        allocatedInPsram = false;

        ProxyPacket* buffer = static_cast<ProxyPacket*>(
            heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM));
        if (buffer) {
            allocatedInPsram = true;
            memset(buffer, 0, bytes);
            return buffer;
        }

        Serial.printf("[BLE] WARN: %s PSRAM allocation failed (%lu bytes), falling back to internal SRAM\n",
                      label,
                      static_cast<unsigned long>(bytes));
        buffer = static_cast<ProxyPacket*>(
            heap_caps_malloc(bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL));
        if (buffer) {
            memset(buffer, 0, bytes);
        }
        return buffer;
    };

    proxyQueue_ = allocateBuffer(PROXY_QUEUE_SIZE, "proxyQueue_", proxyQueueAllocatedInPsram);
    phone2v1Queue_ = allocateBuffer(PHONE_CMD_QUEUE_SIZE, "phone2v1Queue_", phoneQueueAllocatedInPsram);

    if (!proxyQueue_ || !phone2v1Queue_) {
        Serial.println("[BLE] ERROR: Proxy queue allocation failed; disabling proxy");
        releaseProxyQueues();
        proxyEnabled_ = false;
        return false;
    }

    proxyQueuesInPsram_ = proxyQueueAllocatedInPsram && phoneQueueAllocatedInPsram;
    proxyQueueHead_ = 0;
    proxyQueueTail_ = 0;
    proxyQueueCount_ = 0;
    phone2v1QueueHead_ = 0;
    phone2v1QueueTail_ = 0;
    phone2v1QueueCount_ = 0;
    proxyMetrics_.reset();

    Serial.printf("[BLE] Proxy queues allocated (proxy=%s phone=%s)\n",
                  proxyQueueAllocatedInPsram ? "PSRAM" : "INTERNAL",
                  phoneQueueAllocatedInPsram ? "PSRAM" : "INTERNAL");
    return true;
}

void V1BLEClient::releaseProxyQueues() {
    if (proxyQueue_) {
        heap_caps_free(proxyQueue_);
        proxyQueue_ = nullptr;
    }
    if (phone2v1Queue_) {
        heap_caps_free(phone2v1Queue_);
        phone2v1Queue_ = nullptr;
    }

    proxyQueuesInPsram_ = false;
    proxyQueueHead_ = 0;
    proxyQueueTail_ = 0;
    proxyQueueCount_ = 0;
    phone2v1QueueHead_ = 0;
    phone2v1QueueTail_ = 0;
    phone2v1QueueCount_ = 0;
}

bool V1BLEClient::initProxyServer(const char* deviceName) {
    // Proxy server init (name logged in initBLE summary)
    if (!allocateProxyQueues()) {
        return false;
    }

    // Kenny's exact order:
    // 1. Create server (no callbacks yet)
    pServer_ = NimBLEDevice::createServer();

    // 2. Create service
    pProxyService_ = pServer_->createService(V1_SERVICE_UUID);

    // 3. Create ALL 6 characteristics that the V1 exposes (apps expect all of them)
    // Characteristic UUIDs from V1:
    // 92A0B2CE - Display data SHORT (notify) - primary alert data
    // 92A0B4E0 - V1 out LONG (notify)
    // 92A0B6D4 - Client write SHORT (writeNR)
    // 92A0B8D2 - Client write LONG (writeNR)
    // 92A0BCE0 - Notify characteristic
    // 92A0BAD4 - Write with response

    // Primary notify characteristic - display/alert data
    pProxyNotifyChar_ = pProxyService_->createCharacteristic(
        "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // V1 out LONG - notify (stores responses like voltage)
    pProxyNotifyLongChar_ = pProxyService_->createCharacteristic(
        "92A0B4E0-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Client write SHORT - primary command input
    pProxyWriteChar_ = pProxyService_->createCharacteristic(
        "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );

    // Client write LONG
    NimBLECharacteristic* pWriteLong = pProxyService_->createCharacteristic(
        "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );

    // Additional notify characteristic
    [[maybe_unused]] NimBLECharacteristic* pNotifyAlt = pProxyService_->createCharacteristic(
        "92A0BCE0-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Alternate write with response
    NimBLECharacteristic* pWriteAlt = pProxyService_->createCharacteristic(
        "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );

    // 4. Set characteristic callbacks - all write chars forward to V1
    pProxyWriteCallbacks_.reset(new ProxyWriteCallbacks(this));
    pProxyWriteChar_->setCallbacks(pProxyWriteCallbacks_.get());
    pWriteLong->setCallbacks(pProxyWriteCallbacks_.get());
    pWriteAlt->setCallbacks(pProxyWriteCallbacks_.get());

    // 5. Start service
    pProxyService_->start();

    // 6. Set server callbacks AFTER service start (Kenny's order - critical!)
    pProxyServerCallbacks_.reset(new ProxyServerCallbacks(this));
    pServer_->setCallbacks(pProxyServerCallbacks_.get());

    // Configure advertising data with improved Android compatibility
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    NimBLEAdvertisementData scanRespData;

    // CRITICAL: Set flags to indicate BLE-only device (0x06 = LE General Discoverable + BR/EDR Not Supported)
    // Without this flag, some Android devices (Motorola G series) may cache the device as "DUAL" (type 3)
    // and attempt BR/EDR connections which fail with GATT_ERROR 133
    advData.setFlags(0x06);

    // Include service UUID in advertising data (required for app discovery)
    advData.setCompleteServices(pProxyService_->getUUID());
    advData.setAppearance(0x0C80);  // Generic tag appearance

    // Put name in both adv data AND scan response for broader compatibility
    // Some Android devices (especially older Motorola) only read one or the other
    advData.setName(deviceName);
    scanRespData.setName(deviceName);

    pAdvertising->setAdvertisementData(advData);
    pAdvertising->setScanResponseData(scanRespData);

    // Advertising interval: 50-100ms is optimal for Android discovery
    // Some devices (Motorola G series) have trouble with faster intervals
    pAdvertising->setMinInterval(0x50);   // 50ms in 0.625ms units = ~50ms
    pAdvertising->setMaxInterval(0xA0);   // 100ms in 0.625ms units = ~100ms

    // Start/stop advertising to initialize the stack cleanly before scanning
    pAdvertising->start();
    delay(25);
    NimBLEDevice::stopAdvertising();
    delay(25);

    return true;
}

bool V1BLEClient::isProxyAdvertising() const {
    return proxyEnabled_ && proxyServerInitialized_ &&
           NimBLEDevice::getAdvertising()->isAdvertising();
}

bool V1BLEClient::forceProxyAdvertising(bool enable, uint8_t reasonCode) {
    if (!proxyEnabled_ || !proxyServerInitialized_ || !pServer_) {
        return false;
    }

    const uint8_t startReason = reasonCode == 0
                                    ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartDirect)
                                    : reasonCode;
    const uint8_t stopReason = reasonCode == 0
                                   ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopOther)
                                   : reasonCode;

    if (enable) {
        if (!connected_.load(std::memory_order_relaxed)) {
            return false;
        }
        // Explicit debug/test control refreshes the no-client window so
        // transition-drive flaps do not inherit a stale boot-time deadline.
        proxyNoClientTimeoutLatched_ = false;
        proxyNoClientDeadlineMs_ = millis() + PROXY_NO_CLIENT_TIMEOUT_MS;
        startProxyAdvertising(startReason, true);
        return isProxyAdvertising();
    }

    stopProxyAdvertisingFromMainLoop(stopReason);
    return true;
}

void V1BLEClient::startProxyAdvertising(uint8_t reasonCode, bool ignoreWifiPriority) {
    if (!proxyServerInitialized_ || !pServer_) {
        Serial.println("Cannot start advertising - proxy server not initialized");
        return;
    }

    if (wifiPriorityMode_ && !ignoreWifiPriority) {
        return;
    }

    if (proxyNoClientTimeoutLatched_) {
        return;
    }

    if (!proxyClientConnectedOnceThisBoot_ && proxyNoClientDeadlineMs_ == 0) {
        proxyNoClientDeadlineMs_ = millis() + PROXY_NO_CLIENT_TIMEOUT_MS;
        Serial.printf("[BLE] Proxy no-client timeout armed (%lus)\n",
                      static_cast<unsigned long>(PROXY_NO_CLIENT_TIMEOUT_MS / 1000));
    }

    const uint32_t startUs = micros();

    // Don't restart if client already connected_
    if (pServer_->getConnectedCount() > 0) {
        Serial.println("Proxy client already connected_, not restarting advertising");
        clearProxyAdvertisingWindowState();
        perfRecordBleProxyStartUs(micros() - startUs);
        return;
    }

    // Start advertising if not already (simple approach, no task needed)
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        if (NimBLEDevice::startAdvertising()) {
            proxyAdvertisingWindowStartMs_ = millis();
            perfRecordProxyAdvertisingTransition(true, reasonCode, millis());
            Serial.println("Proxy advertising started");
        }
    } else {
        if (proxyAdvertisingWindowStartMs_ == 0) {
            proxyAdvertisingWindowStartMs_ = millis();
        }
        perfRecordProxyAdvertisingTransition(true, reasonCode, millis());
        Serial.println("Proxy already advertising");
    }
    perfRecordBleProxyStartUs(micros() - startUs);
}

void V1BLEClient::forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!proxyEnabled_ || !proxyClientConnected_) {
        return;
    }

    // Validate packet size
    if (length == 0 || length > PROXY_PACKET_MAX) {
        return;
    }

    // Protect queue operations from concurrent access (BLE callback vs main loop)
    if (bleNotifyMutex_ && xSemaphoreTake(bleNotifyMutex_, 0) != pdTRUE) {
        // Queue busy – drop to avoid blocking in callback path
        proxyMetrics_.dropCount++;
        return;
    }

    // Queue packet for async send (non-blocking)
    // Use simple ring buffer with drop-oldest backpressure
    if (proxyQueueCount_ >= PROXY_QUEUE_SIZE) {
        // Queue full - drop oldest packet
        proxyQueueTail_ = (proxyQueueTail_ + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount_--;
        proxyMetrics_.dropCount++;
    }

    // Add packet to queue
    ProxyPacket& pkt = proxyQueue_[proxyQueueHead_];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    pkt.tsMs = millis();
    proxyQueueHead_ = (proxyQueueHead_ + 1) % PROXY_QUEUE_SIZE;
    proxyQueueCount_++;

    // Track high water mark
    if (proxyQueueCount_ > proxyMetrics_.queueHighWater) {
        proxyMetrics_.queueHighWater = proxyQueueCount_;
    }
    PERF_MAX(proxyQueueHighWater, proxyQueueCount_);

    if (bleNotifyMutex_) {
        xSemaphoreGive(bleNotifyMutex_);
    }
}

int V1BLEClient::processProxyQueue() {
    if (!proxyEnabled_ || !proxyClientConnected_ || proxyQueueCount_ == 0) {
        return 0;
    }

    // HOT PATH: try-lock only, skip if busy (another iteration will process)
    SemaphoreGuard lock(bleNotifyMutex_, 0);
    if (!lock.locked()) {
        return 0;  // Skip this cycle, try again next loop (counter incremented in SemaphoreGuard)
    }

    int sent = 0;

    // Process all queued packets (typically 1-2)
    while (proxyQueueCount_ > 0) {
        ProxyPacket& pkt = proxyQueue_[proxyQueueTail_];
        uint32_t nowMs = millis();
        if (pkt.tsMs != 0 && nowMs >= pkt.tsMs) {
            perfRecordNotifyToProxyMs(nowMs - pkt.tsMs);
        }

        NimBLECharacteristic* targetChar = nullptr;
        if (pkt.charUUID == V1_SHORT_UUID_DISPLAY_LONG && pProxyNotifyLongChar_) {
            targetChar = pProxyNotifyLongChar_;
        } else if (pProxyNotifyChar_) {
            targetChar = pProxyNotifyChar_;
        }

        if (targetChar) {
            if (targetChar->notify(pkt.data, pkt.length)) {
                proxyMetrics_.sendCount++;
                sent++;
            } else {
                proxyMetrics_.errorCount++;
            }
        }

        proxyQueueTail_ = (proxyQueueTail_ + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount_--;
    }

    return sent;
}

bool V1BLEClient::enqueuePhoneCommand(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!data || length == 0 || length > 32) {
        PERF_INC(phoneCmdDropsInvalid);
        return false;
    }

    if (!phoneCmdMutex_ || xSemaphoreTake(phoneCmdMutex_, 0) != pdTRUE) {
        PERF_INC(phoneCmdDropsLockBusy);
        return false;
    }

    if (phone2v1QueueCount_ >= PHONE_CMD_QUEUE_SIZE) {
        // Preserve the established queue policy: evict oldest, keep newest.
        phone2v1QueueTail_ = (phone2v1QueueTail_ + 1) % PHONE_CMD_QUEUE_SIZE;
        phone2v1QueueCount_--;
        PERF_INC(phoneCmdDropsOverflow);
    }

    ProxyPacket& pkt = phone2v1Queue_[phone2v1QueueHead_];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    phone2v1QueueHead_ = (phone2v1QueueHead_ + 1) % PHONE_CMD_QUEUE_SIZE;
    phone2v1QueueCount_++;
    PERF_MAX(phoneCmdQueueHighWater, phone2v1QueueCount_);

    xSemaphoreGive(phoneCmdMutex_);
    return true;
}

int V1BLEClient::processPhoneCommandQueue() {
    if (!connected_.load(std::memory_order_relaxed)) {
        return 0;
    }

    // Static pending packet: holds command when pacing/lock says "not yet"
    // Ensures no command loss from dequeue-before-send pattern
    static ProxyPacket pendingPkt;
    static uint16_t pendingCharUUID = 0;
    static bool hasPending = false;

    // Clear stale state from previous connection session
    if (phoneCmdPendingClear_) {
        phoneCmdPendingClear_ = false;
        hasPending = false;
    }

    ProxyPacket pktCopy;
    uint16_t charUUID = 0;
    bool hasPacket = false;

    // Try pending packet first (from previous pacing/lock deferral)
    if (hasPending) {
        configASSERT(pendingPkt.length <= sizeof(pendingPkt.data));  // Belt-and-suspenders: validated at enqueue
        memcpy(pktCopy.data, pendingPkt.data, pendingPkt.length);
        pktCopy.length = pendingPkt.length;
        charUUID = pendingCharUUID;
        hasPacket = true;
    } else if (phoneCmdMutex_ && xSemaphoreTake(phoneCmdMutex_, 0) == pdTRUE) {
        // Dequeue one packet under lock
        if (phone2v1QueueCount_ > 0) {
            ProxyPacket& pkt = phone2v1Queue_[phone2v1QueueTail_];
            configASSERT(pkt.length <= sizeof(pkt.data));  // Belt-and-suspenders: validated at enqueue
            memcpy(pktCopy.data, pkt.data, pkt.length);
            pktCopy.length = pkt.length;
            charUUID = pkt.charUUID;
            phone2v1QueueTail_ = (phone2v1QueueTail_ + 1) % PHONE_CMD_QUEUE_SIZE;
            phone2v1QueueCount_--;
            hasPacket = true;
        }
        xSemaphoreGive(phoneCmdMutex_);
    }

    if (!hasPacket) {
        return 0;
    }

    // Send outside queue lock - BLE write can take time
    // HOT PATH: try-lock only, skip if busy
    SendResult result = SendResult::FAILED;
    if (bleNotifyMutex_) {
        SemaphoreGuard lock(bleNotifyMutex_, 0);
        if (!lock.locked()) {
            // Mutex busy - store in pending for next iteration (NOT_YET semantics)
            // (counter incremented in SemaphoreGuard)
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        }
        if (charUUID == V1_SHORT_UUID_COMMAND_LONG && pCommandCharLong_) {
            // Long characteristic write - same transient failure semantics as sendCommand
            if (pCommandCharLong_->writeValue(pktCopy.data, pktCopy.length, false)) {
                result = SendResult::SENT;
            } else {
                PERF_INC(cmdBleBusy);
                result = SendResult::NOT_YET;  // Transient - retry
            }
        } else {
            result = sendCommandWithResult(pktCopy.data, pktCopy.length);
        }
    } else {
        if (charUUID == V1_SHORT_UUID_COMMAND_LONG && pCommandCharLong_) {
            if (pCommandCharLong_->writeValue(pktCopy.data, pktCopy.length, false)) {
                result = SendResult::SENT;
            } else {
                PERF_INC(cmdBleBusy);
                result = SendResult::NOT_YET;  // Transient - retry
            }
        } else {
            result = sendCommandWithResult(pktCopy.data, pktCopy.length);
        }
    }

    switch (result) {
        case SendResult::SENT:
            // Successfully sent - clear pending state
            hasPending = false;
            return 1;
        case SendResult::NOT_YET:
            // Pacing: store in pending for next iteration
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        case SendResult::FAILED:
        default:
            // Hard failure: drop packet, clear pending, count error
            hasPending = false;
            PERF_INC(phoneCmdDropsBleFail);
            return 0;
    }
}
