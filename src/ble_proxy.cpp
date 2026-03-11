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

// ==================== BLE Proxy Server Functions ====================

void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    // NOTE: BLE callback - keep fast
    if (BLE_CALLBACK_LOGS) {
        BLE_LOGF("[BLE] App connected (handle: %d)\n", connInfo.getConnHandle());
    }
    
    // Request connection parameters - use Android-compatible range
    // Min 15ms (12), Max 45ms (36), Latency 0, Timeout 4s (400)
    // Some devices (Motorola G series) reject very tight intervals
    uint16_t connHandle = connInfo.getConnHandle();
    pServer->updateConnParams(connHandle, 12, 36, 0, 400);
    
    if (bleClient) {
        bleClient->setProxyClientConnected(true);
        bleClient->proxyAdvertisingWindowStartMs = 0;
        bleClient->proxyAdvertisingRetryAtMs = 0;
        perfRecordProxyAdvertisingTransition(
            false,
            static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopAppConnected),
            millis());
    }
}

void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    // NOTE: BLE callback - keep fast
    if (BLE_CALLBACK_LOGS) {
        BLE_LOGF("[BLE] App disconnected (reason: %d)\n", reason);
    }
    if (bleClient) {
        bleClient->proxyClientConnected = false;
        bleClient->proxyAdvertisingWindowStartMs = 0;
        // Resume advertising if V1 is still connected and WiFi-priority suppression is off.
        if (bleClient->connected && !bleClient->wifiPriorityMode) {
            bleClient->startProxyAdvertising(
                static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartAppDisconnect));
        }
    }
}

void V1BLEClient::deferLastV1Address(const char* addr) {
    if (!addr || addr[0] == '\0') {
        return;
    }
    portENTER_CRITICAL(&pendingAddrMux);
    snprintf(pendingLastV1Address, sizeof(pendingLastV1Address), "%s", addr);
    pendingLastV1AddressValid = true;
    portEXIT_CRITICAL(&pendingAddrMux);
}

void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    // Forward commands from app to V1
    // NOTE: This is a BLE callback - avoid blocking operations (Serial, delays, long locks)
    if (!pCharacteristic || !bleClient) {
        return;
    }
    
    if (!bleClient->connected) {
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
        if (!bleClient->proxyCmdPending) {
            bleClient->proxyCmdPending = true;
            bleClient->proxyCmdId = packetId;
            bleClient->proxyCmdLen = static_cast<uint8_t>(std::min<size_t>(rawLen, sizeof(bleClient->proxyCmdBuf)));
            memcpy(bleClient->proxyCmdBuf, cmdBuf, bleClient->proxyCmdLen);
        }
        portEXIT_CRITICAL(&proxyCmdMux);
    }
    // Proxy command logging disabled - app uses standard mute (0x34/0x35)
    // Uncomment to debug: snprintf(logBuf, ...) with packet ID at cmdBuf[3]
    
    // Enqueue for main-loop processing to avoid BLE callback blocking
    bleClient->enqueuePhoneCommand(cmdBuf, rawLen, sourceChar);
}

bool V1BLEClient::allocateProxyQueues() {
    if (proxyQueue && phone2v1Queue) {
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

    proxyQueue = allocateBuffer(PROXY_QUEUE_SIZE, "proxyQueue", proxyQueueAllocatedInPsram);
    phone2v1Queue = allocateBuffer(PHONE_CMD_QUEUE_SIZE, "phone2v1Queue", phoneQueueAllocatedInPsram);

    if (!proxyQueue || !phone2v1Queue) {
        Serial.println("[BLE] ERROR: Proxy queue allocation failed; disabling proxy");
        releaseProxyQueues();
        proxyEnabled = false;
        return false;
    }

    proxyQueuesInPsram = proxyQueueAllocatedInPsram && phoneQueueAllocatedInPsram;
    proxyQueueHead = 0;
    proxyQueueTail = 0;
    proxyQueueCount = 0;
    phone2v1QueueHead = 0;
    phone2v1QueueTail = 0;
    phone2v1QueueCount = 0;
    phoneCmdDropsOverflow = 0;
    phoneCmdDropsInvalid = 0;
    phoneCmdDropsBleFail = 0;
    phoneCmdDropsLockBusy = 0;
    proxyMetrics.reset();

    Serial.printf("[BLE] Proxy queues allocated (proxy=%s phone=%s)\n",
                  proxyQueueAllocatedInPsram ? "PSRAM" : "INTERNAL",
                  phoneQueueAllocatedInPsram ? "PSRAM" : "INTERNAL");
    return true;
}

void V1BLEClient::releaseProxyQueues() {
    if (proxyQueue) {
        heap_caps_free(proxyQueue);
        proxyQueue = nullptr;
    }
    if (phone2v1Queue) {
        heap_caps_free(phone2v1Queue);
        phone2v1Queue = nullptr;
    }

    proxyQueuesInPsram = false;
    proxyQueueHead = 0;
    proxyQueueTail = 0;
    proxyQueueCount = 0;
    phone2v1QueueHead = 0;
    phone2v1QueueTail = 0;
    phone2v1QueueCount = 0;
}

bool V1BLEClient::initProxyServer(const char* deviceName) {
    // Proxy server init (name logged in initBLE summary)
    if (!allocateProxyQueues()) {
        return false;
    }
    
    // Kenny's exact order:
    // 1. Create server (no callbacks yet)
    pServer = NimBLEDevice::createServer();
    
    // 2. Create service
    pProxyService = pServer->createService(V1_SERVICE_UUID);
    
    // 3. Create ALL 6 characteristics that the V1 exposes (apps expect all of them)
    // Characteristic UUIDs from V1:
    // 92A0B2CE - Display data SHORT (notify) - primary alert data
    // 92A0B4E0 - V1 out LONG (notify)
    // 92A0B6D4 - Client write SHORT (writeNR)
    // 92A0B8D2 - Client write LONG (writeNR)
    // 92A0BCE0 - Notify characteristic
    // 92A0BAD4 - Write with response
    
    // Primary notify characteristic - display/alert data
    pProxyNotifyChar = pProxyService->createCharacteristic(
        "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // V1 out LONG - notify (stores responses like voltage)
    pProxyNotifyLongChar = pProxyService->createCharacteristic(
        "92A0B4E0-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // Client write SHORT - primary command input
    pProxyWriteChar = pProxyService->createCharacteristic(
        "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );
    
    // Client write LONG
    NimBLECharacteristic* pWriteLong = pProxyService->createCharacteristic(
        "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE_NR
    );
    
    // Additional notify characteristic
    [[maybe_unused]] NimBLECharacteristic* pNotifyAlt = pProxyService->createCharacteristic(
        "92A0BCE0-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // Alternate write with response
    NimBLECharacteristic* pWriteAlt = pProxyService->createCharacteristic(
        "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E",
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    
    // 4. Set characteristic callbacks - all write chars forward to V1
    pProxyWriteCallbacks.reset(new ProxyWriteCallbacks(this));
    pProxyWriteChar->setCallbacks(pProxyWriteCallbacks.get());
    pWriteLong->setCallbacks(pProxyWriteCallbacks.get());
    pWriteAlt->setCallbacks(pProxyWriteCallbacks.get());
    
    // 5. Start service
    pProxyService->start();
    
    // 6. Set server callbacks AFTER service start (Kenny's order - critical!)
    pProxyServerCallbacks.reset(new ProxyServerCallbacks(this));
    pServer->setCallbacks(pProxyServerCallbacks.get());
    
    // Configure advertising data with improved Android compatibility
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    NimBLEAdvertisementData advData;
    NimBLEAdvertisementData scanRespData;
    
    // CRITICAL: Set flags to indicate BLE-only device (0x06 = LE General Discoverable + BR/EDR Not Supported)
    // Without this flag, some Android devices (Motorola G series) may cache the device as "DUAL" (type 3)
    // and attempt BR/EDR connections which fail with GATT_ERROR 133
    advData.setFlags(0x06);
    
    // Include service UUID in advertising data (required for app discovery)
    advData.setCompleteServices(pProxyService->getUUID());
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
    return proxyEnabled && proxyServerInitialized && 
           NimBLEDevice::getAdvertising()->isAdvertising();
}

bool V1BLEClient::forceProxyAdvertising(bool enable, uint8_t reasonCode) {
    if (!proxyEnabled || !proxyServerInitialized || !pServer) {
        return false;
    }

    const uint8_t startReason = reasonCode == 0
                                    ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StartDirect)
                                    : reasonCode;
    const uint8_t stopReason = reasonCode == 0
                                   ? static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::StopOther)
                                   : reasonCode;

    if (enable) {
        if (!connected) {
            return false;
        }
        // Explicit debug/test control refreshes the no-client window so
        // transition-drive flaps do not inherit a stale boot-time deadline.
        proxyNoClientTimeoutLatched = false;
        proxyNoClientDeadlineMs = millis() + PROXY_NO_CLIENT_TIMEOUT_MS;
        startProxyAdvertising(startReason, true);
        return isProxyAdvertising();
    }

    proxyAdvertisingStartMs = 0;
    proxyAdvertisingStartReasonCode =
        static_cast<uint8_t>(PerfProxyAdvertisingTransitionReason::Unknown);
    proxyAdvertisingWindowStartMs = 0;
    proxyAdvertisingRetryAtMs = 0;

    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv && pAdv->isAdvertising()) {
        NimBLEDevice::stopAdvertising();
    }
    perfRecordProxyAdvertisingTransition(false, stopReason, millis());
    return true;
}

void V1BLEClient::startProxyAdvertising(uint8_t reasonCode, bool ignoreWifiPriority) {
    if (!proxyServerInitialized || !pServer) {
        Serial.println("Cannot start advertising - proxy server not initialized");
        return;
    }

    if (wifiPriorityMode && !ignoreWifiPriority) {
        return;
    }

    if (proxyNoClientTimeoutLatched) {
        return;
    }

    if (!proxyClientConnectedOnceThisBoot && proxyNoClientDeadlineMs == 0) {
        proxyNoClientDeadlineMs = millis() + PROXY_NO_CLIENT_TIMEOUT_MS;
        Serial.printf("[BLE] Proxy no-client timeout armed (%lus)\n",
                      static_cast<unsigned long>(PROXY_NO_CLIENT_TIMEOUT_MS / 1000));
    }
    
    // Don't restart if client already connected
    if (pServer->getConnectedCount() > 0) {
        Serial.println("Proxy client already connected, not restarting advertising");
        proxyAdvertisingWindowStartMs = 0;
        proxyAdvertisingRetryAtMs = 0;
        return;
    }
    
    // Start advertising if not already (simple approach, no task needed)
    if (!NimBLEDevice::getAdvertising()->isAdvertising()) {
        if (NimBLEDevice::startAdvertising()) {
            proxyAdvertisingWindowStartMs = millis();
            perfRecordProxyAdvertisingTransition(true, reasonCode, millis());
            Serial.println("Proxy advertising started");
        }
    } else {
        if (proxyAdvertisingWindowStartMs == 0) {
            proxyAdvertisingWindowStartMs = millis();
        }
        perfRecordProxyAdvertisingTransition(true, reasonCode, millis());
        Serial.println("Proxy already advertising");
    }
}

void V1BLEClient::forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!proxyEnabled || !proxyClientConnected) {
        return;
    }
    
    // Validate packet size
    if (length == 0 || length > PROXY_PACKET_MAX) {
        return;
    }
    
    // Protect queue operations from concurrent access (BLE callback vs main loop)
    if (bleNotifyMutex && xSemaphoreTake(bleNotifyMutex, 0) != pdTRUE) {
        // Queue busy – drop to avoid blocking in callback path
        proxyMetrics.dropCount++;
        return;
    }
    
    // Queue packet for async send (non-blocking)
    // Use simple ring buffer with drop-oldest backpressure
    if (proxyQueueCount >= PROXY_QUEUE_SIZE) {
        // Queue full - drop oldest packet
        proxyQueueTail = (proxyQueueTail + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount--;
        proxyMetrics.dropCount++;
    }
    
    // Add packet to queue
    ProxyPacket& pkt = proxyQueue[proxyQueueHead];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    pkt.tsMs = millis();
    proxyQueueHead = (proxyQueueHead + 1) % PROXY_QUEUE_SIZE;
    proxyQueueCount++;
    
    // Track high water mark
    if (proxyQueueCount > proxyMetrics.queueHighWater) {
        proxyMetrics.queueHighWater = proxyQueueCount;
    }
    PERF_MAX(proxyQueueHighWater, proxyQueueCount);

    if (bleNotifyMutex) {
        xSemaphoreGive(bleNotifyMutex);
    }
}

// PERFORMANCE: Immediate proxy forwarding - zero latency path
// Called directly from BLE callback context - no queue, no delay
// Uses non-blocking mutex to avoid deadlock while preventing concurrent notifies
void V1BLEClient::forwardToProxyImmediate(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!proxyEnabled || !proxyClientConnected) {
        return;
    }
    
    // Validate packet size
    if (length == 0 || length > PROXY_PACKET_MAX) {
        return;
    }
    
    // Route to correct proxy characteristic based on source
    // B2CE (0xB2CE) -> proxy B2CE (short display data)
    // B4E0 (0xB4E0) -> proxy B4E0 (long alert/response data - voltage, etc)
    NimBLECharacteristic* targetChar = nullptr;
    
    if (sourceCharUUID == 0xB4E0 && pProxyNotifyLongChar) {
        targetChar = pProxyNotifyLongChar;
    } else if (pProxyNotifyChar) {
        targetChar = pProxyNotifyChar;
    }
    
    if (!targetChar) {
        return;
    }
    
    // Try non-blocking mutex acquire to avoid concurrent notifies
    // If mutex is held (processProxyQueue running), enqueue instead
    if (xSemaphoreTake(bleNotifyMutex, 0) == pdTRUE) {
        // Got mutex - safe to notify immediately
        if (targetChar->notify(data, length)) {
            proxyMetrics.sendCount++;
        } else {
            proxyMetrics.errorCount++;
            // Notify failed (stack busy) - enqueue for retry
            // Release mutex first to avoid recursive lock in forwardToProxy
            xSemaphoreGive(bleNotifyMutex);
            forwardToProxy(data, length, sourceCharUUID);
            return;
        }
        xSemaphoreGive(bleNotifyMutex);
    } else {
        // Mutex held by processProxyQueue - enqueue to avoid race
        forwardToProxy(data, length, sourceCharUUID);
    }
}

int V1BLEClient::processProxyQueue() {
    if (!proxyEnabled || !proxyClientConnected || proxyQueueCount == 0) {
        return 0;
    }
    
    // HOT PATH: try-lock only, skip if busy (another iteration will process)
    SemaphoreGuard lock(bleNotifyMutex, 0);
    if (!lock.locked()) {
        return 0;  // Skip this cycle, try again next loop (counter incremented in SemaphoreGuard)
    }
    
    int sent = 0;
    
    // Process all queued packets (typically 1-2)
    while (proxyQueueCount > 0) {
        ProxyPacket& pkt = proxyQueue[proxyQueueTail];
        uint32_t nowMs = millis();
        if (pkt.tsMs != 0 && nowMs >= pkt.tsMs) {
            perfRecordNotifyToProxyMs(nowMs - pkt.tsMs);
        }

        NimBLECharacteristic* targetChar = nullptr;
        if (pkt.charUUID == 0xB4E0 && pProxyNotifyLongChar) {
            targetChar = pProxyNotifyLongChar;
        } else if (pProxyNotifyChar) {
            targetChar = pProxyNotifyChar;
        }

        if (targetChar) {
            if (targetChar->notify(pkt.data, pkt.length)) {
                proxyMetrics.sendCount++;
                sent++;
            } else {
                proxyMetrics.errorCount++;
            }
        }
        
        proxyQueueTail = (proxyQueueTail + 1) % PROXY_QUEUE_SIZE;
        proxyQueueCount--;
    }
    
    return sent;
}

bool V1BLEClient::enqueuePhoneCommand(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!data || length == 0 || length > 32) {
        phoneCmdDropsInvalid++;
        PERF_INC(phoneCmdDropsInvalid);
        return false;
    }

    if (!phoneCmdMutex || xSemaphoreTake(phoneCmdMutex, 0) != pdTRUE) {
        phoneCmdDropsLockBusy++;
        PERF_INC(phoneCmdDropsLockBusy);
        return false;
    }

    if (phone2v1QueueCount >= PHONE_CMD_QUEUE_SIZE) {
        phone2v1QueueTail = (phone2v1QueueTail + 1) % PHONE_CMD_QUEUE_SIZE;
        phone2v1QueueCount--;
        phoneCmdDropsOverflow++;
        PERF_INC(phoneCmdDropsOverflow);
    }

    ProxyPacket& pkt = phone2v1Queue[phone2v1QueueHead];
    memcpy(pkt.data, data, length);
    pkt.length = length;
    pkt.charUUID = sourceCharUUID;
    phone2v1QueueHead = (phone2v1QueueHead + 1) % PHONE_CMD_QUEUE_SIZE;
    phone2v1QueueCount++;
    PERF_MAX(phoneCmdQueueHighWater, phone2v1QueueCount);

    xSemaphoreGive(phoneCmdMutex);
    return true;
}

int V1BLEClient::processPhoneCommandQueue() {
    if (!connected) {
        return 0;
    }

    // Static pending packet: holds command when pacing/lock says "not yet"
    // Ensures no command loss from dequeue-before-send pattern
    static ProxyPacket pendingPkt;
    static uint16_t pendingCharUUID = 0;
    static bool hasPending = false;

    // Clear stale state from previous connection session
    if (phoneCmdPendingClear) {
        phoneCmdPendingClear = false;
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
    } else if (phoneCmdMutex && xSemaphoreTake(phoneCmdMutex, 0) == pdTRUE) {
        // Dequeue one packet under lock
        if (phone2v1QueueCount > 0) {
            ProxyPacket& pkt = phone2v1Queue[phone2v1QueueTail];
            configASSERT(pkt.length <= sizeof(pkt.data));  // Belt-and-suspenders: validated at enqueue
            memcpy(pktCopy.data, pkt.data, pkt.length);
            pktCopy.length = pkt.length;
            charUUID = pkt.charUUID;
            phone2v1QueueTail = (phone2v1QueueTail + 1) % PHONE_CMD_QUEUE_SIZE;
            phone2v1QueueCount--;
            hasPacket = true;
        }
        xSemaphoreGive(phoneCmdMutex);
    }

    if (!hasPacket) {
        return 0;
    }

    // Send outside queue lock - BLE write can take time
    // HOT PATH: try-lock only, skip if busy
    SendResult result = SendResult::FAILED;
    if (bleNotifyMutex) {
        SemaphoreGuard lock(bleNotifyMutex, 0);
        if (!lock.locked()) {
            // Mutex busy - store in pending for next iteration (NOT_YET semantics)
            // (counter incremented in SemaphoreGuard)
            memcpy(pendingPkt.data, pktCopy.data, pktCopy.length);
            pendingPkt.length = pktCopy.length;
            pendingCharUUID = charUUID;
            hasPending = true;
            return 0;
        }
        if (charUUID == 0xB8D2 && pCommandCharLong) {
            // Long characteristic write - same transient failure semantics as sendCommand
            if (pCommandCharLong->writeValue(pktCopy.data, pktCopy.length, false)) {
                result = SendResult::SENT;
            } else {
                PERF_INC(cmdBleBusy);
                result = SendResult::NOT_YET;  // Transient - retry
            }
        } else {
            result = sendCommandWithResult(pktCopy.data, pktCopy.length);
        }
    } else {
        if (charUUID == 0xB8D2 && pCommandCharLong) {
            if (pCommandCharLong->writeValue(pktCopy.data, pktCopy.length, false)) {
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
            phoneCmdDropsBleFail++;  // Count as BLE failure (not connected, char null)
            PERF_INC(phoneCmdDropsBleFail);
            return 0;
    }
}
