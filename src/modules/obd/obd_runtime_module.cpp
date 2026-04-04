#include "obd_runtime_module.h"

#include "obd_elm327_parser.h"
#include "obd_scan_policy.h"
#include "perf_metrics.h"
#include "../../psram_freertos_alloc.h"

#ifndef UNIT_TEST
#include "ble_client.h"
#include "obd_ble_client.h"

extern "C" {
#include "nimble/nimble/host/include/host/ble_att.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nimble/nimble/include/nimble/ble.h"
}
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace {
bool stringContainsCI(const char* haystack, const char* needle) {
    if (!haystack || !needle || needle[0] == '\0') return false;
    const size_t needleLen = strlen(needle);
    const size_t haystackLen = strlen(haystack);
    if (needleLen > haystackLen) return false;

    for (size_t offset = 0; offset + needleLen <= haystackLen; ++offset) {
        bool matches = true;
        for (size_t i = 0; i < needleLen; ++i) {
            const char lhs = static_cast<char>(toupper(static_cast<unsigned char>(haystack[offset + i])));
            const char rhs = static_cast<char>(toupper(static_cast<unsigned char>(needle[i])));
            if (lhs != rhs) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

void copyString(char* dest, size_t destLen, const char* src) {
    if (!dest || destLen == 0) return;
    dest[0] = '\0';
    if (!src) return;
    strncpy(dest, src, destLen - 1);
    dest[destLen - 1] = '\0';
}

size_t commandDisplayLen(const char* command) {
    if (!command) return 0;
    size_t len = strlen(command);
    while (len > 0 && (command[len - 1] == '\r' || command[len - 1] == '\n')) {
        --len;
    }
    return len;
}

constexpr uint32_t OBD_RSSI_REFRESH_MS = 2000;

#ifndef UNIT_TEST
constexpr UBaseType_t OBD_TRANSPORT_QUEUE_DEPTH = 1;
constexpr uint32_t OBD_TRANSPORT_STACK_SIZE = 8192;
constexpr UBaseType_t OBD_TRANSPORT_PRIORITY = 1;
constexpr TickType_t OBD_TRANSPORT_RECEIVE_TIMEOUT_TICKS = pdMS_TO_TICKS(1000);
constexpr size_t OBD_TRANSPORT_ADDR_BUF_LEN = 18;
constexpr size_t OBD_TRANSPORT_CMD_BUF_LEN = 16;

struct ObdTransportRequest {
    ObdTransportOp op = ObdTransportOp::NONE;
    uint32_t requestId = 0;
    uint32_t timeoutMs = 0;
    uint32_t nowMs = 0;
    char address[OBD_TRANSPORT_ADDR_BUF_LEN] = {};
    uint8_t addrType = 0;
    bool preferCachedAttributes = false;
    char cmd[OBD_TRANSPORT_CMD_BUF_LEN] = {};
    bool withResponse = false;
};

struct ObdTransportContext {
    ObdBleClient* bleClient = nullptr;
    ObdRuntimeModule* runtime = nullptr;
};

struct ObdTransportRuntime {
    QueueHandle_t requestQueue = nullptr;
    QueueHandle_t resultQueue = nullptr;
    TaskHandle_t task = nullptr;
    PsramQueueAllocation requestQueueAllocation = {};
    PsramQueueAllocation resultQueueAllocation = {};
    bool requestQueueInPsram = false;
    bool resultQueueInPsram = false;
    bool taskStackInPsram = false;
    ObdTransportContext context = {};
};

ObdTransportRuntime sObdTransport;

void obdTransportTaskEntry(void* param) {
    auto* context = static_cast<ObdTransportContext*>(param);
    if (!context || !context->bleClient || !context->runtime) {
        vTaskDelete(nullptr);
        return;
    }

    while (true) {
        ObdTransportRequest request{};
        if (!sObdTransport.requestQueue ||
            xQueueReceive(sObdTransport.requestQueue,
                          &request,
                          OBD_TRANSPORT_RECEIVE_TIMEOUT_TICKS) != pdTRUE) {
            continue;
        }

        ObdTransportResult result{};
        result.ready = true;
        result.op = request.op;
        result.requestId = request.requestId;
        result.issuedMs = request.nowMs;

        switch (request.op) {
            case ObdTransportOp::CONNECT:
                result.success = context->bleClient->connect(
                    request.address,
                    request.addrType,
                    request.timeoutMs,
                    request.preferCachedAttributes);
                result.bleError = context->bleClient->getLastBleError();
                break;
            case ObdTransportOp::DISCONNECT:
                context->bleClient->disconnect();
                result.success = true;
                result.bleError = context->bleClient->getLastBleError();
                break;
            case ObdTransportOp::SECURITY_START:
                result.success = context->bleClient->beginSecurity();
                result.bleError = context->bleClient->getLastBleError();
                result.securityError = context->bleClient->getLastSecurityError();
                break;
            case ObdTransportOp::DISCOVER:
                result.success = context->bleClient->discoverServices();
                result.bleError = context->bleClient->getLastBleError();
                break;
            case ObdTransportOp::SUBSCRIBE:
                result.success = context->bleClient->subscribeNotify([](const uint8_t* data, size_t len) {
                    if (sObdTransport.context.runtime) {
                        sObdTransport.context.runtime->onBleData(data, len);
                    }
                });
                result.bleError = context->bleClient->getLastBleError();
                break;
            case ObdTransportOp::WRITE:
                result.success = context->bleClient->writeCommand(request.cmd, request.withResponse);
                result.bleError = context->bleClient->getLastBleError();
                break;
            case ObdTransportOp::RSSI_READ:
                result.success = true;
                result.rssi = context->bleClient->getRssi(request.nowMs);
                result.bleError = context->bleClient->getLastBleError();
                break;
            case ObdTransportOp::NONE:
            default:
                result.success = false;
                break;
        }

        if (sObdTransport.resultQueue) {
            xQueueOverwrite(sObdTransport.resultQueue, &result);
        }
        taskYIELD();
    }
}

bool ensureObdTransportRuntime(ObdBleClient* bleClient, ObdRuntimeModule* runtime) {
    if (!bleClient || !runtime) {
        Serial.println("[OBD] ERROR: transport dependencies not provided");
        return false;
    }

    if (!sObdTransport.requestQueue) {
        sObdTransport.requestQueue = createQueuePreferPsram(
            OBD_TRANSPORT_QUEUE_DEPTH,
            sizeof(ObdTransportRequest),
            sObdTransport.requestQueueAllocation,
            &sObdTransport.requestQueueInPsram);
        if (!sObdTransport.requestQueue) {
            Serial.println("[OBD] ERROR: failed to create transport request queue");
            return false;
        }
    }
    if (!sObdTransport.resultQueue) {
        sObdTransport.resultQueue = createQueuePreferPsram(
            OBD_TRANSPORT_QUEUE_DEPTH,
            sizeof(ObdTransportResult),
            sObdTransport.resultQueueAllocation,
            &sObdTransport.resultQueueInPsram);
        if (!sObdTransport.resultQueue) {
            Serial.println("[OBD] ERROR: failed to create transport result queue");
            return false;
        }
    }
    if (!sObdTransport.task) {
        sObdTransport.context.bleClient = bleClient;
        sObdTransport.context.runtime = runtime;

        const BaseType_t rc = createTaskPinnedToCorePreferPsram(
            obdTransportTaskEntry,
            "ObdTransport",
            OBD_TRANSPORT_STACK_SIZE,
            &sObdTransport.context,
            OBD_TRANSPORT_PRIORITY,
            &sObdTransport.task,
            0,
            &sObdTransport.taskStackInPsram);
        if (rc != pdPASS) {
            Serial.println("[OBD] ERROR: failed to create transport task");
            return false;
        }
    }
    return true;
}
#endif

}  // namespace

ObdRuntimeModule obdRuntimeModule;

void ObdRuntimeModule::resetForBegin() {
    state_ = ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
    bootReadyMs_ = 0;
    stateEntryPending_ = false;
    minRssi_ = obd::DEFAULT_MIN_RSSI;
    rssi_ = 0;
    pendingRssi_ = 0;
    pendingAddrType_ = 0;
    savedAddrType_ = 0;
    connectAddrType_ = 0;
    manualCandidateAddrType_ = 0;
    pendingDeviceFound_ = false;
    scanRequested_ = false;
    manualScanPending_ = false;
    manualScanPreemptProxy_ = false;
    manualCandidateValid_ = false;
    connectTargetFromManualCandidate_ = false;
    preferWarmReconnect_ = false;
    warmInitPreferred_ = false;
    coldInitFallbackUsed_ = false;
    preferWriteWithResponse_ = false;

    savedAddress_[0] = '\0';
    connectAddress_[0] = '\0';
    manualCandidateAddress_[0] = '\0';
    pendingAddress_[0] = '\0';

    connectAttempts_ = 0;
    connectSuccesses_ = 0;
    connectFailures_ = 0;
    pollCount_ = 0;
    pollErrors_ = 0;
    staleSpeedCount_ = 0;
    consecutiveErrors_ = 0;
    backoffCycles_ = 0;
    v1WasConnectedAtEcuIdle_ = false;
    totalBytesReceived_ = 0;
    lastRssiMs_ = 0;
    bufferOverflowCount_ = 0;
    initRetries_ = 0;
    consecutiveSpeedSamples_ = 0;
    securityRepairs_ = 0;
    lastConnectStartMs_ = 0;
    lastConnectSuccessMs_ = 0;
    lastFailureMs_ = 0;
    lastFailure_ = ObdFailureReason::NONE;
    bleDisconnectReason_ = 0;
    repairedBondAddress_[0] = '\0';

    clearBleEventQueue();
    clearBleResponseState();
    bleDisconnected_ = false;
    resetCommandState();
    nextTransportRequestId_ = 0;
    clearTransportRequest();
    readyTransportResult_ = {};
    clearSpeedState();
    resetPollingSchedule(0);
    initIndex_ = 0;

#ifdef UNIT_TEST
    testStartScanResult_ = true;
    testConnectResult_ = true;
    testBleConnected_ = false;
    testDiscoverResult_ = true;
    testSubscribeResult_ = true;
    testWriteResult_ = true;
    testBeginSecurityResult_ = true;
    testSecurityReady_ = true;
    testSecurityEncrypted_ = true;
    testSecurityBonded_ = true;
    testSecurityAuthenticated_ = true;
    testRssi_ = 0;
    testLastBleError_ = 0;
    testLastSecurityError_ = 0;
    testStartScanCalls_ = 0;
    testConnectCalls_ = 0;
    testDiscoverCalls_ = 0;
    testDisconnectCalls_ = 0;
    testWriteCalls_ = 0;
    testBeginSecurityCalls_ = 0;
    testDeleteBondCalls_ = 0;
    testRefreshBondBackupCalls_ = 0;
    testLastCommand_[0] = '\0';
    testLastWriteWithResponse_ = true;
#endif
}

void ObdRuntimeModule::begin(ObdBleClient* bleClient,
                             bool enabled,
                             const char* savedAddress,
                             uint8_t savedAddrType,
                             int8_t minRssi) {
    bleClient_ = bleClient;
    enabled_ = enabled;
    resetForBegin();
    setMinRssi(minRssi);

    setSavedAddressFromBuffer(savedAddress);
    savedAddrType_ = savedAddrType;

#ifndef UNIT_TEST
    if (bleClient_) {
        bleClient_->init(this);
    }
    (void)ensureObdTransportRuntime(bleClient_, this);
#endif

    if (!enabled_) {
        return;
    }

    if (savedAddress_[0] != '\0') {
        state_ = ObdConnectionState::WAIT_BOOT;
#ifndef UNIT_TEST
        Serial.printf("[OBD] begin addr=%s addrType=%u -> WAIT_BOOT\n",
                      savedAddress_, savedAddrType_);
#endif
    }
}

namespace {
const char* obdStateName(ObdConnectionState s) {
    switch (s) {
        case ObdConnectionState::IDLE:          return "IDLE";
        case ObdConnectionState::WAIT_BOOT:     return "WAIT_BOOT";
        case ObdConnectionState::SCANNING:      return "SCANNING";
        case ObdConnectionState::CONNECTING:    return "CONNECTING";
        case ObdConnectionState::SECURING:      return "SECURING";
        case ObdConnectionState::DISCOVERING:   return "DISCOVERING";
        case ObdConnectionState::AT_INIT:       return "AT_INIT";
        case ObdConnectionState::POLLING:       return "POLLING";
        case ObdConnectionState::ERROR_BACKOFF: return "ERROR_BACKOFF";
        case ObdConnectionState::DISCONNECTED:  return "DISCONNECTED";
        case ObdConnectionState::ECU_IDLE:      return "ECU_IDLE";
        default:                                return "?";
    }
}
}  // namespace

void ObdRuntimeModule::transitionTo(ObdConnectionState newState, uint32_t nowMs) {
    if (newState == ObdConnectionState::POLLING) {
        commitManualScanCandidate();
    }
#ifndef UNIT_TEST
    Serial.printf("[OBD] %s -> %s\n", obdStateName(state_), obdStateName(newState));
#endif
    state_ = newState;
    stateEnteredMs_ = nowMs;
    stateEntryPending_ = true;
}

bool ObdRuntimeModule::shouldHoldProxyForAutoObd() const {
    switch (state_) {
        case ObdConnectionState::WAIT_BOOT:
        case ObdConnectionState::CONNECTING:
        case ObdConnectionState::SECURING:
        case ObdConnectionState::DISCOVERING:
        case ObdConnectionState::AT_INIT:
            return savedAddress_[0] != '\0' || connectAddress_[0] != '\0';
        default:
            return false;
    }
}

void ObdRuntimeModule::clearBleEventQueue() {
    taskENTER_CRITICAL(&bleEventQueueMux_);
    bleEventQueueHead_ = 0;
    bleEventQueueCount_ = 0;
    taskEXIT_CRITICAL(&bleEventQueueMux_);
}

bool ObdRuntimeModule::enqueueBleEvent(const BleEvent& event) {
    BleEvent queuedEvent = event;

    taskENTER_CRITICAL(&bleEventQueueMux_);
    if (bleEventQueueCount_ >= BLE_EVENT_QUEUE_DEPTH) {
        const BleEvent& dropped = bleEventQueue_[bleEventQueueHead_];
        if (queuedEvent.type == BleEventType::DATA && dropped.type == BleEventType::DATA) {
            queuedEvent.overflowed = true;
        }
        bleEventQueueHead_ = (bleEventQueueHead_ + 1) % BLE_EVENT_QUEUE_DEPTH;
        bleEventQueueCount_--;
    }

    const size_t tail = (bleEventQueueHead_ + bleEventQueueCount_) % BLE_EVENT_QUEUE_DEPTH;
    bleEventQueue_[tail] = queuedEvent;
    bleEventQueueCount_++;
    taskEXIT_CRITICAL(&bleEventQueueMux_);
    return true;
}

bool ObdRuntimeModule::popBleEvent(BleEvent& event) {
    taskENTER_CRITICAL(&bleEventQueueMux_);
    if (bleEventQueueCount_ == 0) {
        taskEXIT_CRITICAL(&bleEventQueueMux_);
        return false;
    }

    event = bleEventQueue_[bleEventQueueHead_];
    bleEventQueueHead_ = (bleEventQueueHead_ + 1) % BLE_EVENT_QUEUE_DEPTH;
    bleEventQueueCount_--;
    taskEXIT_CRITICAL(&bleEventQueueMux_);
    return true;
}

void ObdRuntimeModule::applyBleEvent(const BleEvent& event) {
    switch (event.type) {
        case BleEventType::DEVICE_FOUND:
            if (state_ != ObdConnectionState::SCANNING ||
                event.address[0] == '\0' ||
                event.rssi < minRssi_) {
                return;
            }
            copyString(pendingAddress_, sizeof(pendingAddress_), event.address);
            pendingRssi_ = event.rssi;
            pendingAddrType_ = event.addrType;
            pendingDeviceFound_ = true;
            return;

        case BleEventType::DISCONNECT:
            bleDisconnected_ = true;
            bleDisconnectReason_ = event.disconnectReason;
            return;

        case BleEventType::DATA:
            if (state_ != ObdConnectionState::AT_INIT &&
                state_ != ObdConnectionState::POLLING) {
                return;
            }
            {
                const size_t remaining = BLE_BUF_LEN - 1 - bleBufLen_;
                const size_t toCopy = std::min(event.dataLen, remaining);
                if (toCopy > 0) {
                    memcpy(bleBuf_ + bleBufLen_, event.data, toCopy);
                    bleBufLen_ += toCopy;
                    bleBuf_[bleBufLen_] = '\0';
                }
                if (toCopy < event.dataLen || event.overflowed) {
                    bleOverflowed_ = true;
                }
                if (event.dataReady) {
                    bleDataReady_ = true;
                }
            }
            return;
    }
}

void ObdRuntimeModule::drainBleEventQueue() {
    BleEvent event;
    while (popBleEvent(event)) {
        applyBleEvent(event);
    }
}

void ObdRuntimeModule::clearBleResponseState() {
    bleBufLen_ = 0;
    bleBuf_[0] = '\0';
    bleDataReady_ = false;
    bleOverflowed_ = false;
}

void ObdRuntimeModule::clearSpeedState() {
    speedMph_ = 0.0f;
    speedSampleTsMs_ = 0;
    speedValid_ = false;
    consecutiveSpeedSamples_ = 0;
}

void ObdRuntimeModule::resetPollingSchedule(uint32_t nowMs) {
    nextSpeedDueMs_ = nowMs;
}

void ObdRuntimeModule::resetInitState(bool preferWarmInit) {
    initIndex_ = 0;
    warmInitPreferred_ = preferWarmInit;
    coldInitFallbackUsed_ = false;
    initRetries_ = 0;
    resetCommandState();
    clearBleResponseState();
}

void ObdRuntimeModule::resetCommandState() {
    activeCommand_ = ActiveObdCommand{};
}

void ObdRuntimeModule::markFailure(ObdFailureReason reason, uint32_t nowMs) {
    lastFailure_ = reason;
    lastFailureMs_ = nowMs;
}

void ObdRuntimeModule::handleConnectFailure(uint32_t nowMs, ObdFailureReason reason) {
    markFailure(reason, nowMs);
    connectFailures_++;
    connectAttempts_++;
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    if (manualScanPending_) {
        connectAttempts_ = 0;
        clearManualScanState();
        transitionTo(ObdConnectionState::IDLE, nowMs);
        return;
    }
    if (connectAttempts_ >= obd::MAX_DIRECT_CONNECT_FAILURES) {
        connectAttempts_ = 0;
        transitionTo(ObdConnectionState::IDLE, nowMs);
        return;
    }
    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
}

void ObdRuntimeModule::handlePollingError(uint32_t nowMs,
                                          bool disconnectBleNow,
                                          ObdFailureReason reason) {
    markFailure(reason, nowMs);
    pollErrors_++;
    consecutiveErrors_++;
    consecutiveSpeedSamples_ = 0;
    clearBleResponseState();
    resetCommandState();
    if (disconnectBleNow) {
        disconnectBle();
    }
    if (consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT &&
        shouldDisconnectAfterPollingError(reason)) {
        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
        return;
    }
    if (consecutiveErrors_ >= obd::MAX_CONSECUTIVE_ERRORS) {
        transitionTo(ObdConnectionState::ERROR_BACKOFF, nowMs);
    }
}

void ObdRuntimeModule::handleCommandFailure(uint32_t nowMs,
                                            ObdFailureReason reason,
                                            bool disconnectBleNow) {
    handlePollingError(nowMs, disconnectBleNow, reason);
}

bool ObdRuntimeModule::shouldDisconnectAfterPollingError(ObdFailureReason reason) {
    switch (reason) {
        case ObdFailureReason::WRITE:
        case ObdFailureReason::BUFFER_OVERFLOW:
            return true;
        case ObdFailureReason::NONE:
        case ObdFailureReason::CONNECT_START:
        case ObdFailureReason::CONNECT_TIMEOUT:
        case ObdFailureReason::DISCOVERY:
        case ObdFailureReason::SUBSCRIBE:
        case ObdFailureReason::INIT_TIMEOUT:
        case ObdFailureReason::INIT_RESPONSE:
        case ObdFailureReason::COMMAND_TIMEOUT:
        case ObdFailureReason::COMMAND_RESPONSE:
        case ObdFailureReason::SECURITY_START:
        case ObdFailureReason::SECURITY_TIMEOUT:
        default:
            return false;
    }
}

void ObdRuntimeModule::setSavedAddressFromBuffer(const char* address) {
    copyString(savedAddress_, sizeof(savedAddress_), address);
}

void ObdRuntimeModule::setConnectTarget(const char* address,
                                        uint8_t addrType,
                                        bool fromManualCandidate) {
    copyString(connectAddress_, sizeof(connectAddress_), address);
    connectAddrType_ = addrType;
    connectTargetFromManualCandidate_ = fromManualCandidate && connectAddress_[0] != '\0';
}

void ObdRuntimeModule::setConnectTargetFromSaved() {
    setConnectTarget(savedAddress_, savedAddrType_, false);
}

void ObdRuntimeModule::clearConnectTarget() {
    connectAddress_[0] = '\0';
    connectAddrType_ = 0;
    connectTargetFromManualCandidate_ = false;
}

void ObdRuntimeModule::clearManualScanState() {
    manualScanPending_ = false;
    manualScanPreemptProxy_ = false;
    manualCandidateValid_ = false;
    manualCandidateAddress_[0] = '\0';
    manualCandidateAddrType_ = 0;
    clearConnectTarget();
}

void ObdRuntimeModule::commitManualScanCandidate() {
    if (!manualScanPending_ || !manualCandidateValid_) {
        return;
    }

    const bool addressChanged =
        strcmp(savedAddress_, manualCandidateAddress_) != 0 || savedAddrType_ != manualCandidateAddrType_;

    setSavedAddressFromBuffer(manualCandidateAddress_);
    savedAddrType_ = manualCandidateAddrType_;
    if (addressChanged) {
        preferWarmReconnect_ = false;
    }
    clearManualScanState();
}

bool ObdRuntimeModule::startBleScan() {
#ifndef UNIT_TEST
    return bleClient_ ? bleClient_->startScan(this, minRssi_) : false;
#else
    testStartScanCalls_++;
    return testStartScanResult_;
#endif
}

bool ObdRuntimeModule::connectBle(uint32_t timeoutMs, bool preferCachedAttributes) {
    const char* const address = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    const uint8_t addrType = connectAddress_[0] != '\0' ? connectAddrType_ : savedAddrType_;
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool connected = bleClient_->connect(address, addrType, timeoutMs, preferCachedAttributes);
    perfRecordObdConnectCallUs(PERF_TIMESTAMP_US() - startUs);
    return connected;
#else
    (void)address;
    (void)addrType;
    (void)timeoutMs;
    (void)preferCachedAttributes;
    testConnectCalls_++;
    return testConnectResult_;
#endif
}

bool ObdRuntimeModule::isBleConnected() const {
#ifndef UNIT_TEST
    return bleClient_->isConnected();
#else
    return testBleConnected_;
#endif
}

bool ObdRuntimeModule::beginBleSecurity() {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool started = bleClient_->beginSecurity();
    perfRecordObdSecurityStartCallUs(PERF_TIMESTAMP_US() - startUs);
    return started;
#else
    testBeginSecurityCalls_++;
    return testBeginSecurityResult_;
#endif
}

bool ObdRuntimeModule::isBleSecurityReady() const {
#ifndef UNIT_TEST
    return bleClient_->isSecurityReady();
#else
    return testSecurityReady_;
#endif
}

bool ObdRuntimeModule::isBleEncrypted() const {
#ifndef UNIT_TEST
    return bleClient_->isEncrypted();
#else
    return testSecurityEncrypted_;
#endif
}

bool ObdRuntimeModule::isBleBonded() const {
#ifndef UNIT_TEST
    return bleClient_->isBonded();
#else
    return testSecurityBonded_;
#endif
}

bool ObdRuntimeModule::isBleAuthenticated() const {
#ifndef UNIT_TEST
    return bleClient_->isAuthenticated();
#else
    return testSecurityAuthenticated_;
#endif
}

int ObdRuntimeModule::getBleLastError() const {
#ifndef UNIT_TEST
    return bleClient_->getLastBleError();
#else
    return testLastBleError_;
#endif
}

int ObdRuntimeModule::getBleSecurityFailure() const {
#ifndef UNIT_TEST
    return bleClient_->getLastSecurityError();
#else
    return testLastSecurityError_;
#endif
}

bool ObdRuntimeModule::discoverBleServices() {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool discovered = bleClient_->discoverServices();
    perfRecordObdDiscoveryCallUs(PERF_TIMESTAMP_US() - startUs);
    return discovered;
#else
    testDiscoverCalls_++;
    return testDiscoverResult_;
#endif
}

bool ObdRuntimeModule::subscribeBleNotifications() {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool subscribed = bleClient_->subscribeNotify([](const uint8_t* data, size_t len) {
        obdRuntimeModule.onBleData(data, len);
    });
    perfRecordObdSubscribeCallUs(PERF_TIMESTAMP_US() - startUs);
    return subscribed;
#else
    return testSubscribeResult_;
#endif
}

bool ObdRuntimeModule::writeBleCommand(const char* cmd, bool withResponse) {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const bool wrote = bleClient_->writeCommand(cmd, withResponse);
    perfRecordObdWriteCallUs(PERF_TIMESTAMP_US() - startUs);
    return wrote;
#else
    testWriteCalls_++;
    copyString(testLastCommand_, sizeof(testLastCommand_), cmd);
    testLastWriteWithResponse_ = withResponse;
    return testWriteResult_;
#endif
}

bool ObdRuntimeModule::deleteBleBond() {
    const char* const address = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    const uint8_t addrType = connectAddress_[0] != '\0' ? connectAddrType_ : savedAddrType_;
#ifndef UNIT_TEST
    return bleClient_->deleteBond(address, addrType);
#else
    (void)address;
    (void)addrType;
    testDeleteBondCalls_++;
    return true;
#endif
}

void ObdRuntimeModule::refreshBleBondBackup() {
#ifndef UNIT_TEST
    ::refreshBleBondBackup();
#else
    testRefreshBondBackupCalls_++;
#endif
}

void ObdRuntimeModule::disconnectBle() {
#ifndef UNIT_TEST
    bleClient_->disconnect();
#else
    testDisconnectCalls_++;
    testBleConnected_ = false;
    pendingTransportRequestId_ = 0;
    pendingTransportIssuedMs_ = 0;
    pendingTransportTimeoutMs_ = 0;
#endif
}

void ObdRuntimeModule::stopBleScan() {
#ifndef UNIT_TEST
    bleClient_->stopScan();
#endif
}

int8_t ObdRuntimeModule::readBleRssi(uint32_t nowMs) {
#ifndef UNIT_TEST
    const uint32_t startUs = PERF_TIMESTAMP_US();
    const int8_t rssi = bleClient_->getRssi(nowMs);
    perfRecordObdRssiCallUs(PERF_TIMESTAMP_US() - startUs);
    return rssi;
#else
    (void)nowMs;
    return testRssi_;
#endif
}

void ObdRuntimeModule::clearTransportRequest() {
    transportRequestActive_ = false;
    pendingTransportOp_ = ObdTransportOp::NONE;
    pendingTransportRequestId_ = 0;
    pendingTransportIssuedMs_ = 0;
    pendingTransportTimeoutMs_ = 0;
    pendingTransportTimedOut_ = false;
}

bool ObdRuntimeModule::beginTransportRequest(ObdTransportOp op,
                                             uint32_t nowMs,
                                             uint32_t timeoutMs,
                                             const char* cmd,
                                             bool withResponse,
                                             bool preferCachedAttributes) {
    if (transportRequestActive_) {
        return false;
    }
    readyTransportResult_ = {};

#ifndef UNIT_TEST
    if (!ensureObdTransportRuntime(bleClient_, this) || !sObdTransport.requestQueue) {
        return false;
    }

    ObdTransportRequest request{};
    request.op = op;
    request.requestId = ++nextTransportRequestId_;
    request.timeoutMs = timeoutMs;
    request.nowMs = nowMs;
    const char* const address = connectAddress_[0] != '\0' ? connectAddress_ : savedAddress_;
    const uint8_t addrType = connectAddress_[0] != '\0' ? connectAddrType_ : savedAddrType_;
    request.addrType = addrType;
    request.preferCachedAttributes = preferCachedAttributes;
    request.withResponse = withResponse;
    copyString(request.address, sizeof(request.address), address);
    copyString(request.cmd, sizeof(request.cmd), cmd);

    if (xQueueSend(sObdTransport.requestQueue, &request, 0) != pdTRUE) {
        return false;
    }

    transportRequestActive_ = true;
    pendingTransportOp_ = op;
    pendingTransportRequestId_ = request.requestId;
    pendingTransportIssuedMs_ = nowMs;
    pendingTransportTimeoutMs_ = timeoutMs;
    pendingTransportTimedOut_ = false;
    return true;
#else
    ObdTransportResult result{};
    result.ready = true;
    result.op = op;
    result.requestId = ++nextTransportRequestId_;
    result.issuedMs = nowMs;
    switch (op) {
        case ObdTransportOp::CONNECT:
            result.success = connectBle(timeoutMs, preferCachedAttributes);
            result.bleError = getBleLastError();
            break;
        case ObdTransportOp::DISCONNECT:
            disconnectBle();
            result.success = true;
            result.bleError = getBleLastError();
            break;
        case ObdTransportOp::SECURITY_START:
            result.success = beginBleSecurity();
            result.bleError = getBleLastError();
            result.securityError = getBleSecurityFailure();
            break;
        case ObdTransportOp::DISCOVER:
            result.success = discoverBleServices();
            result.bleError = getBleLastError();
            break;
        case ObdTransportOp::SUBSCRIBE:
            result.success = subscribeBleNotifications();
            result.bleError = getBleLastError();
            break;
        case ObdTransportOp::WRITE:
            result.success = writeBleCommand(cmd, withResponse);
            result.bleError = getBleLastError();
            break;
        case ObdTransportOp::RSSI_READ:
            result.success = true;
            result.rssi = readBleRssi(nowMs);
            result.bleError = getBleLastError();
            break;
        case ObdTransportOp::NONE:
        default:
            result.success = false;
            break;
    }
    readyTransportResult_ = result;
    return true;
#endif
}

void ObdRuntimeModule::pumpTransportResults() {
#ifndef UNIT_TEST
    if (!sObdTransport.resultQueue) {
        return;
    }

    ObdTransportResult result{};
    while (xQueueReceive(sObdTransport.resultQueue, &result, 0) == pdTRUE) {
        if (!transportRequestActive_ || result.requestId != pendingTransportRequestId_) {
            continue;
        }
        result.timedOut = pendingTransportTimedOut_;
        readyTransportResult_ = result;
        clearTransportRequest();
    }
#endif
}

bool ObdRuntimeModule::pendingTransportTimedOut(uint32_t nowMs) const {
    return transportRequestActive_ &&
           pendingTransportTimeoutMs_ > 0 &&
           static_cast<int32_t>(nowMs - pendingTransportIssuedMs_) >=
               static_cast<int32_t>(pendingTransportTimeoutMs_);
}

bool ObdRuntimeModule::takeTransportResult(ObdTransportOp op, ObdTransportResult& result) {
    if (!readyTransportResult_.ready || readyTransportResult_.op != op) {
        return false;
    }
    result = readyTransportResult_;
    readyTransportResult_ = {};
    return true;
}

void ObdRuntimeModule::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;

    if (!enabled_) {
        stopBleScan();
        disconnectBle();
        clearBleEventQueue();
        scanRequested_ = false;
        clearManualScanState();
        pendingDeviceFound_ = false;
        pendingAddress_[0] = '\0';
        clearBleResponseState();
        resetCommandState();
        clearTransportRequest();
        readyTransportResult_ = {};
        bleDisconnected_ = false;
        clearSpeedState();
        state_ = ObdConnectionState::IDLE;
        stateEnteredMs_ = 0;
        stateEntryPending_ = false;
        return;
    }

    clearSpeedState();
    connectAttempts_ = 0;
    resetPollingSchedule(0);
    clearBleEventQueue();
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    clearManualScanState();
    state_ = (savedAddress_[0] != '\0') ? ObdConnectionState::WAIT_BOOT : ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
    stateEntryPending_ = false;
}

void ObdRuntimeModule::setMinRssi(int8_t minRssi) {
    if (minRssi < -90) minRssi = -90;
    if (minRssi > -40) minRssi = -40;
    minRssi_ = minRssi;
}

bool ObdRuntimeModule::validateAtResponse(const char* command,
                                          const char* response,
                                          size_t len) const {
    if (!command || !response || len == 0) {
        return false;
    }

    if (strncmp(command, "0100", 4) == 0) {
        return validateSimpleResponse(0x41, 0x00, response, len);
    }
    if (strncmp(command, "ATZ", 3) == 0 || strncmp(command, "ATI", 3) == 0) {
        return stringContainsCI(response, "OBDLINK") ||
               stringContainsCI(response, "STN") ||
               stringContainsCI(response, "ELM327");
    }
    return stringContainsCI(response, "OK");
}

bool ObdRuntimeModule::startCommand(ObdCommandKind kind,
                                    ParserKind parser,
                                    const char* tx,
                                    uint8_t expectedService,
                                    uint8_t expectedPid,
                                    uint16_t expectedDid,
                                    uint32_t timeoutMs,
                                    uint8_t retries,
                                    uint32_t nowMs) {
    if (!tx || tx[0] == '\0') {
        return false;
    }

    clearBleResponseState();
    activeCommand_ = ActiveObdCommand{};
    activeCommand_.active = true;
    activeCommand_.kind = kind;
    activeCommand_.parser = parser;
    activeCommand_.expectedService = expectedService;
    activeCommand_.expectedPid = expectedPid;
    activeCommand_.expectedDid = expectedDid;
    activeCommand_.timeoutMs = timeoutMs;
    activeCommand_.retriesRemaining = retries;
    activeCommand_.writeWithResponse = preferWriteWithResponse_;
    activeCommand_.alternateWriteModeTried = false;
    copyString(activeCommand_.tx, sizeof(activeCommand_.tx), tx);

    activeCommand_.sentMs = 0;
    if (!beginTransportRequest(ObdTransportOp::WRITE,
                               nowMs,
                               0,
                               activeCommand_.tx,
                               activeCommand_.writeWithResponse)) {
        resetCommandState();
        return false;
    }
    return true;
}

bool ObdRuntimeModule::retryActiveCommand(uint32_t nowMs) {
    if (!activeCommand_.active || activeCommand_.retriesRemaining == 0) {
        return false;
    }

    activeCommand_.retriesRemaining--;
    initRetries_++;
    clearBleResponseState();
    activeCommand_.sentMs = 0;
    if (!beginTransportRequest(ObdTransportOp::WRITE,
                               nowMs,
                               0,
                               activeCommand_.tx,
                               activeCommand_.writeWithResponse)) {
        return false;
    }
    return true;
}

bool ObdRuntimeModule::retryActiveCommandWithAlternateWriteMode(uint32_t nowMs) {
    if (!activeCommand_.active ||
        activeCommand_.retriesRemaining == 0 ||
        activeCommand_.alternateWriteModeTried) {
        return false;
    }

    activeCommand_.retriesRemaining--;
    activeCommand_.alternateWriteModeTried = true;
    activeCommand_.writeWithResponse = !activeCommand_.writeWithResponse;
    initRetries_++;
    clearBleResponseState();
    activeCommand_.sentMs = 0;
    if (!beginTransportRequest(ObdTransportOp::WRITE,
                               nowMs,
                               0,
                               activeCommand_.tx,
                               activeCommand_.writeWithResponse)) {
        return false;
    }
    return true;
}

void ObdRuntimeModule::completeActiveCommand() {
    resetCommandState();
}

bool ObdRuntimeModule::validateSimpleResponse(uint8_t expectedService,
                                              uint8_t expectedPid,
                                              const char* response,
                                              size_t len) const {
    Elm327ParseResult result = parseElm327Response(response, len);
    return result.valid && result.service == expectedService && result.pid == expectedPid;
}

void ObdRuntimeModule::handleAtInitResponse(uint32_t nowMs) {
    if (!activeCommand_.active) {
        return;
    }
    const bool valid = !bleOverflowed_ &&
                       validateAtResponse(activeCommand_.tx, bleBuf_, bleBufLen_);
    clearBleResponseState();

    if (valid) {
        preferWriteWithResponse_ = activeCommand_.writeWithResponse;
        completeActiveCommand();
        initIndex_++;
        return;
    }

    // 0100 (supported PIDs) fails when the vehicle isn't running. Main
    // explicitly treats this as non-fatal: "vehicle might just be off".
    // Skip and proceed to polling so AT commands still work for voltage, etc.
    if (activeCommand_.kind == ObdCommandKind::SANITY) {
#ifndef UNIT_TEST
        Serial.println("[OBD] sanity 0100 failed (vehicle may be off) - skipping");
#endif
        completeActiveCommand();
        initIndex_++;
        return;
    }

    if (retryActiveCommand(nowMs)) {
        return;
    }

    if (warmInitPreferred_ && !coldInitFallbackUsed_) {
        coldInitFallbackUsed_ = true;
        resetInitState(false);
        return;
    }

    disconnectBle();
    handleConnectFailure(nowMs, ObdFailureReason::INIT_RESPONSE);
}

bool ObdRuntimeModule::handleSpeedResponse(uint32_t nowMs) {
    Elm327ParseResult result = parseElm327Response(bleBuf_, bleBufLen_);
    if (!result.valid || result.service != 0x41 || result.pid != 0x0D) {
        return false;
    }

    const float kmh = decodeSpeedKmh(result);
    if (kmh < 0.0f) {
        return false;
    }

    speedMph_ = kmhToMph(kmh);
    speedSampleTsMs_ = nowMs;
    speedValid_ = true;
    consecutiveSpeedSamples_++;
    consecutiveErrors_ = 0;
    backoffCycles_ = 0;
    pollCount_++;
    nextSpeedDueMs_ = nowMs + obd::POLL_INTERVAL_MS;
    return true;
}

void ObdRuntimeModule::handlePollingResponse(uint32_t nowMs) {
    if (!activeCommand_.active) {
        return;
    }

    const ObdCommandKind kind = activeCommand_.kind;
    bool handled = false;

    if (bleOverflowed_) {
        bufferOverflowCount_++;
        handled = false;
    } else {
        switch (kind) {
            case ObdCommandKind::SPEED:
                handled = handleSpeedResponse(nowMs);
                break;
            default:
                handled = false;
                break;
        }
    }

    totalBytesReceived_ += bleBufLen_;
    clearBleResponseState();
    const bool completedWriteWithResponse = activeCommand_.writeWithResponse;
    completeActiveCommand();

    if (handled) {
        preferWriteWithResponse_ = completedWriteWithResponse;
        return;
    }

    if (bufferOverflowCount_ >= obd::BUFFER_OVERFLOWS_BEFORE_DISCONNECT) {
        handlePollingError(nowMs, false, ObdFailureReason::BUFFER_OVERFLOW);
    } else {
        handlePollingError(nowMs,
                           false,
                           bleOverflowed_ ? ObdFailureReason::BUFFER_OVERFLOW
                                          : ObdFailureReason::COMMAND_RESPONSE);
    }
}

bool ObdRuntimeModule::isSpeedFresh(uint32_t nowMs) const {
    return speedValid_ && speedSampleTsMs_ != 0 &&
           (nowMs - speedSampleTsMs_) <= obd::SPEED_MAX_AGE_MS;
}

bool ObdRuntimeModule::speedDue(uint32_t nowMs) const {
    return nextSpeedDueMs_ == 0 || static_cast<int32_t>(nowMs - nextSpeedDueMs_) >= 0;
}

bool ObdRuntimeModule::startSpeedCommand(uint32_t nowMs) {
    if (!startCommand(ObdCommandKind::SPEED,
                      ParserKind::SIMPLE,
                      obd::SPEED_POLL_CMD,
                      0x41,
                      0x0D,
                      0x0000,
                      obd::POLL_TIMEOUT_MS,
                      obd::POLL_COMMAND_RETRIES,
                      nowMs)) {
        handlePollingError(nowMs, false, ObdFailureReason::WRITE);
        return false;
    }
    nextSpeedDueMs_ = nowMs + obd::POLL_INTERVAL_MS;
    return true;
}

bool ObdRuntimeModule::sendNextPollingCommand(uint32_t nowMs) {
    return speedDue(nowMs) ? startSpeedCommand(nowMs) : false;
}

void ObdRuntimeModule::updateSecuring(uint32_t nowMs) {
    ObdTransportResult transportResult{};

    if (bleDisconnected_) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] lost connection during securing (ble reason=%d %s)\n",
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        bleDisconnected_ = false;
        if (autoHealBondIfAllowed(nowMs, "securing_disconnect")) {
            return;
        }
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_TIMEOUT);
        return;
    }

    if ((nowMs - stateEnteredMs_) < obd::POST_CONNECT_SETTLE_MS) {
        return;
    }

    if (isBleSecurityReady() || isBleEncrypted()) {
        transitionTo(ObdConnectionState::DISCOVERING, nowMs);
        return;
    }

    if (takeTransportResult(ObdTransportOp::SECURITY_START, transportResult) &&
        (!transportResult.success || transportResult.timedOut)) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] secureConnection start failed rc=%d (%s)\n",
                      transportResult.securityError,
                      bleReasonName(transportResult.securityError));
#endif
        if (autoHealBondIfAllowed(nowMs, "securing_start")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_START);
        return;
    }

    if (!transportRequestActive_ &&
        !readyTransportResult_.ready &&
        !beginTransportRequest(ObdTransportOp::SECURITY_START, nowMs, obd::SECURITY_TIMEOUT_MS)) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] secureConnection request queue failed rc=%d (%s)\n",
                      getBleSecurityFailure(),
                      bleReasonName(getBleSecurityFailure()));
#endif
        if (autoHealBondIfAllowed(nowMs, "securing_start")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_START);
        return;
    }

    if ((nowMs - stateEnteredMs_) >= (obd::POST_CONNECT_SETTLE_MS + obd::SECURITY_TIMEOUT_MS)) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] securing timed out bleError=%d (%s) securityError=%d (%s)\n",
                      getBleLastError(),
                      bleReasonName(getBleLastError()),
                      getBleSecurityFailure(),
                      bleReasonName(getBleSecurityFailure()));
#endif
        if (autoHealBondIfAllowed(nowMs, "securing_timeout")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::SECURITY_TIMEOUT);
    }
}

void ObdRuntimeModule::updateAtInit(uint32_t nowMs) {
    ObdTransportResult transportResult{};

    if (bleDisconnected_) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] lost connection during AT init (ble reason=%d %s)\n",
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        bleDisconnected_ = false;
        if (manualScanPending_) {
            clearManualScanState();
            transitionTo(ObdConnectionState::IDLE, nowMs);
            return;
        }
        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
        return;
    }

    if (activeCommand_.active) {
        if (activeCommand_.sentMs == 0) {
            if (!takeTransportResult(ObdTransportOp::WRITE, transportResult)) {
                return;
            }
            if (!transportResult.success || transportResult.timedOut) {
                const int cmdLen = static_cast<int>(commandDisplayLen(activeCommand_.tx));
#ifndef UNIT_TEST
                Serial.printf("[OBD] AT init write failed cmd=%.*s writeMode=%s rc=%d (%s) timedOut=%d\n",
                              cmdLen,
                              activeCommand_.tx,
                              activeCommand_.writeWithResponse ? "with_response" : "no_response",
                              transportResult.bleError,
                              bleReasonName(transportResult.bleError),
                              transportResult.timedOut ? 1 : 0);
#endif
                if (initIndex_ == 0 && autoHealBondIfAllowed(nowMs, "at_init_write")) {
                    return;
                }
                disconnectBle();
                handleConnectFailure(nowMs,
                                     transportResult.timedOut
                                         ? ObdFailureReason::INIT_TIMEOUT
                                         : ObdFailureReason::WRITE);
                return;
            }
            activeCommand_.sentMs = transportResult.issuedMs;
            return;
        }
        if (bleDataReady_) {
            handleAtInitResponse(nowMs);
            return;
        }
        if (nowMs - activeCommand_.sentMs >= activeCommand_.timeoutMs) {
            const int cmdLen = static_cast<int>(commandDisplayLen(activeCommand_.tx));
#ifndef UNIT_TEST
            Serial.printf("[OBD] AT init response timed out cmd=%.*s writeMode=%s rxBytes=%u raw=[%.*s] securityReady=%d enc=%d bond=%d auth=%d lastBleError=%d (%s) disconnectReason=%d (%s)\n",
                          cmdLen,
                          activeCommand_.tx,
                          activeCommand_.writeWithResponse ? "with_response" : "no_response",
                          static_cast<unsigned>(bleBufLen_),
                          static_cast<int>(bleBufLen_),
                          bleBuf_,
                          isBleSecurityReady(),
                          isBleEncrypted(),
                          isBleBonded(),
                          isBleAuthenticated(),
                          getBleLastError(),
                          bleReasonName(getBleLastError()),
                          bleDisconnectReason_,
                          bleReasonName(bleDisconnectReason_));
#endif
            // 0100 (sanity) times out when vehicle isn't running - non-fatal.
            if (activeCommand_.kind == ObdCommandKind::SANITY) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] sanity 0100 timed out rxBytes=%u (vehicle may be off) - skipping\n",
                              static_cast<unsigned>(bleBufLen_));
#endif
                clearBleResponseState();
                completeActiveCommand();
                initIndex_++;
                return;
            }
            if (bleBufLen_ == 0 && retryActiveCommandWithAlternateWriteMode(nowMs)) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] AT init retrying cmd=%.*s with alternate write mode=%s after empty timeout\n",
                              cmdLen,
                              activeCommand_.tx,
                              activeCommand_.writeWithResponse ? "with_response" : "no_response");
#endif
                return;
            }
            if (retryActiveCommand(nowMs)) {
                return;
            }
            if (warmInitPreferred_ && !coldInitFallbackUsed_) {
                coldInitFallbackUsed_ = true;
                resetInitState(false);
                return;
            }
            disconnectBle();
            handleConnectFailure(nowMs, ObdFailureReason::INIT_TIMEOUT);
        }
        return;
    }

    if ((nowMs - stateEnteredMs_) < obd::POST_SUBSCRIBE_SETTLE_MS) {
        return;
    }

    const char* const* commands = warmInitPreferred_ ? obd::WARM_INIT_COMMANDS : obd::COLD_INIT_COMMANDS;
    const size_t commandCount = warmInitPreferred_ ? obd::WARM_INIT_COMMAND_COUNT : obd::COLD_INIT_COMMAND_COUNT;
    if (initIndex_ >= commandCount) {
        consecutiveErrors_ = 0;
        resetPollingSchedule(nowMs);
        clearBleResponseState();
        transitionTo(ObdConnectionState::POLLING, nowMs);
        return;
    }

    const char* command = commands[initIndex_];
    const bool isSanity = strncmp(command, "0100", 4) == 0;
    if (!startCommand(isSanity ? ObdCommandKind::SANITY : ObdCommandKind::AT_INIT,
                      isSanity ? ParserKind::SIMPLE : ParserKind::AT_TEXT,
                      command,
                      isSanity ? 0x41 : 0x00,
                      isSanity ? 0x00 : 0x00,
                      0x0000,
                      obd::AT_INIT_RESPONSE_TIMEOUT_MS,
                      obd::AT_INIT_RETRIES,
                      nowMs)) {
#ifndef UNIT_TEST
        const int cmdLen = static_cast<int>(commandDisplayLen(command));
        Serial.printf("[OBD] AT init write failed cmd=%.*s writeMode=%s rc=%d (%s) securityReady=%d enc=%d bond=%d auth=%d bleReason=%d (%s)\n",
                      cmdLen,
                      command,
                      preferWriteWithResponse_ ? "with_response" : "no_response",
                      getBleLastError(),
                      bleReasonName(getBleLastError()),
                      isBleSecurityReady(),
                      isBleEncrypted(),
                      isBleBonded(),
                      isBleAuthenticated(),
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        if (initIndex_ == 0 && autoHealBondIfAllowed(nowMs, "at_init_write")) {
            return;
        }
        disconnectBle();
        handleConnectFailure(nowMs, ObdFailureReason::WRITE);
    }
}

void ObdRuntimeModule::updatePolling(uint32_t nowMs) {
    ObdTransportResult transportResult{};

    if (bleDisconnected_) {
#ifndef UNIT_TEST
        Serial.printf("[OBD] lost connection during polling (ble reason=%d %s)\n",
                      bleDisconnectReason_,
                      bleReasonName(bleDisconnectReason_));
#endif
        bleDisconnected_ = false;
        clearSpeedState();
        clearBleResponseState();
        resetCommandState();
        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
        return;
    }

    if (activeCommand_.active) {
        if (activeCommand_.sentMs == 0) {
            if (!takeTransportResult(ObdTransportOp::WRITE, transportResult)) {
                return;
            }
            if (!transportResult.success || transportResult.timedOut) {
                clearBleResponseState();
                completeActiveCommand();
                handlePollingError(nowMs,
                                   false,
                                   transportResult.timedOut
                                       ? ObdFailureReason::COMMAND_TIMEOUT
                                       : ObdFailureReason::WRITE);
                return;
            }
            activeCommand_.sentMs = transportResult.issuedMs;
            return;
        }
        if (bleDataReady_) {
            handlePollingResponse(nowMs);
            if (state_ != ObdConnectionState::POLLING) {
                return;
            }
        } else if (nowMs - activeCommand_.sentMs >= activeCommand_.timeoutMs) {
            // CX sends "SEARCHING..." while probing OBD protocols (up to ~10s).
            // Extend timeout so we don't retry mid-search and collide.
            if (activeCommand_.kind == ObdCommandKind::SPEED &&
                bleBufLen_ >= 9 &&
                strstr(bleBuf_, "SEARCHING") != nullptr &&
                (nowMs - activeCommand_.sentMs) < obd::SEARCH_EXTENDED_TIMEOUT_MS) {
                return;
            }
            if (activeCommand_.kind == ObdCommandKind::SPEED &&
                bleBufLen_ == 0 &&
                retryActiveCommandWithAlternateWriteMode(nowMs)) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] speed timeout retrying with alternate write mode=%s\n",
                              activeCommand_.writeWithResponse ? "with_response" : "no_response");
#endif
                return;
            }
#ifndef UNIT_TEST
            if (activeCommand_.kind == ObdCommandKind::SPEED && bleBufLen_ > 0) {
                Serial.printf("[OBD] speed timeout rxBytes=%u raw=[%.*s]\n",
                              static_cast<unsigned>(bleBufLen_),
                              static_cast<int>(bleBufLen_),
                              bleBuf_);
            }
#endif
            clearBleResponseState();
            completeActiveCommand();
            handlePollingError(nowMs, false, ObdFailureReason::COMMAND_TIMEOUT);
            return;
        }
    }

    if (!activeCommand_.active) {
        sendNextPollingCommand(nowMs);
    }

    if (takeTransportResult(ObdTransportOp::RSSI_READ, transportResult)) {
        rssi_ = transportResult.rssi;
        lastRssiMs_ = nowMs;
    } else if (!transportRequestActive_ &&
               !readyTransportResult_.ready &&
               !(activeCommand_.active && activeCommand_.sentMs == 0) &&
               static_cast<int32_t>(nowMs - lastRssiMs_) >= static_cast<int32_t>(OBD_RSSI_REFRESH_MS)) {
        if (beginTransportRequest(ObdTransportOp::RSSI_READ, nowMs, 0)) {
            lastRssiMs_ = nowMs;
        }
    }

    if (speedValid_ && !isSpeedFresh(nowMs)) {
        speedValid_ = false;
        staleSpeedCount_++;
    }
}

void ObdRuntimeModule::update(uint32_t nowMs, const ObdBleContext& bootReadyContext) {
    if (!enabled_) return;

    drainBleEventQueue();

    if (pendingTransportTimedOut(nowMs)) {
        pendingTransportTimedOut_ = true;
    }
    pumpTransportResults();

    const bool bootReady = bootReadyContext.bootReady;
    const bool v1Connected = bootReadyContext.v1Connected;
    const bool bleScanIdle = bootReadyContext.bleScanIdle;
    const bool v1ConnectBurstSettling = bootReadyContext.v1ConnectBurstSettling;
    const bool proxyAdvertising = bootReadyContext.proxyAdvertising;
    const bool proxyClientConnected = bootReadyContext.proxyClientConnected;

    if (bootReady && bootReadyMs_ == 0) {
        bootReadyMs_ = nowMs == 0 ? 1 : nowMs;
    }

    const bool justEntered = stateEntryPending_;
    stateEntryPending_ = false;

    switch (state_) {
        case ObdConnectionState::IDLE:
            if (manualScanPreemptProxy_ && (proxyAdvertising || proxyClientConnected)) {
                break;
            }
            if (scanRequested_ && bleScanIdle) {
                if (startBleScan()) {
                    scanRequested_ = false;
                    transitionTo(ObdConnectionState::SCANNING, nowMs);
                }
            }
            break;

        case ObdConnectionState::WAIT_BOOT: {
            if (!bootReady) {
                break;
            }

            const uint32_t elapsed = nowMs - bootReadyMs_;
            if (proxyClientConnected) {
                break;
            }

            if (v1Connected && v1ConnectBurstSettling) {
                break;
            }

            if (v1Connected || elapsed >= obd::POST_BOOT_DWELL_MS) {
                setConnectTargetFromSaved();
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            }
            break;
        }

        case ObdConnectionState::SCANNING: {
            if (pendingDeviceFound_) {
                pendingDeviceFound_ = false;
                rssi_ = pendingRssi_;
                connectAttempts_ = 0;
                preferWarmReconnect_ = false;
                if (manualScanPending_) {
                    copyString(manualCandidateAddress_, sizeof(manualCandidateAddress_), pendingAddress_);
                    manualCandidateAddrType_ = pendingAddrType_;
                    manualCandidateValid_ = true;
                    setConnectTarget(manualCandidateAddress_, manualCandidateAddrType_, true);
                    manualScanPreemptProxy_ = false;
                } else {
                    setSavedAddressFromBuffer(pendingAddress_);
                    savedAddrType_ = pendingAddrType_;
                    setConnectTargetFromSaved();
                }
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
                break;
            }
            if ((nowMs - stateEnteredMs_) >= obd::SCAN_DURATION_MS) {
                if (manualScanPending_) {
                    clearManualScanState();
                }
                transitionTo(ObdConnectionState::IDLE, nowMs);
            }
            break;
        }

        case ObdConnectionState::CONNECTING:
            if (bleDisconnected_) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] connect failed (ble reason=%d %s)\n",
                              bleDisconnectReason_,
                              bleReasonName(bleDisconnectReason_));
#endif
                bleDisconnected_ = false;
                handleConnectFailure(nowMs, ObdFailureReason::CONNECT_START);
                break;
            }
            {
                ObdTransportResult transportResult{};
                if (takeTransportResult(ObdTransportOp::CONNECT, transportResult)) {
                    if (!transportResult.success || transportResult.timedOut) {
                        handleConnectFailure(nowMs,
                                             transportResult.timedOut
                                                 ? ObdFailureReason::CONNECT_TIMEOUT
                                                 : ObdFailureReason::CONNECT_START);
                        break;
                    }
                }
            }
            if (justEntered) {
                bleDisconnectReason_ = 0;
                lastConnectStartMs_ = nowMs;
                const bool preferCachedAttributes = preferWarmReconnect_ && savedAddress_[0] != '\0';
                if (!beginTransportRequest(ObdTransportOp::CONNECT,
                                           nowMs,
                                           obd::CONNECT_TIMEOUT_MS,
                                           nullptr,
                                           false,
                                           preferCachedAttributes)) {
                    handleConnectFailure(nowMs, ObdFailureReason::CONNECT_START);
                    break;
                }
            }
            if (isBleConnected()) {
                connectAttempts_ = 0;
                connectSuccesses_++;
                lastConnectSuccessMs_ = nowMs;
                if (pendingTransportOp_ == ObdTransportOp::CONNECT ||
                    readyTransportResult_.op == ObdTransportOp::CONNECT) {
                    clearTransportRequest();
                    readyTransportResult_ = {};
                }
                transitionTo(ObdConnectionState::DISCOVERING, nowMs);
                break;
            }
            if ((nowMs - stateEnteredMs_) >= obd::CONNECT_TIMEOUT_MS) {
                clearTransportRequest();
                readyTransportResult_ = {};
                disconnectBle();
                handleConnectFailure(nowMs, ObdFailureReason::CONNECT_TIMEOUT);
                break;
            }
            break;

        case ObdConnectionState::SECURING:
            updateSecuring(nowMs);
            break;

        case ObdConnectionState::DISCOVERING:
            if (bleDisconnected_) {
#ifndef UNIT_TEST
                Serial.printf("[OBD] lost connection during discovery (ble reason=%d %s)\n",
                              bleDisconnectReason_,
                              bleReasonName(bleDisconnectReason_));
#endif
                bleDisconnected_ = false;
                if (manualScanPending_) {
                    clearManualScanState();
                    transitionTo(ObdConnectionState::IDLE, nowMs);
                    break;
                }
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }
            // DA14531 BLE 4.2 needs time after connect before GATT ops
            if ((nowMs - stateEnteredMs_) < obd::POST_CONNECT_SETTLE_MS) {
                break;
            }
            {
                ObdTransportResult transportResult{};
                if (takeTransportResult(ObdTransportOp::DISCOVER, transportResult)) {
                    if (!transportResult.success || transportResult.timedOut) {
                        disconnectBle();
                        handleConnectFailure(nowMs, ObdFailureReason::DISCOVERY);
                        break;
                    }
                    if (bleDisconnected_) {
#ifndef UNIT_TEST
                        Serial.printf("[OBD] lost connection after discovery (ble reason=%d %s)\n",
                                      bleDisconnectReason_,
                                      bleReasonName(bleDisconnectReason_));
#endif
                        bleDisconnected_ = false;
                        if (manualScanPending_) {
                            clearManualScanState();
                            transitionTo(ObdConnectionState::IDLE, nowMs);
                            break;
                        }
                        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                        break;
                    }
                    if (!beginTransportRequest(ObdTransportOp::SUBSCRIBE, nowMs, obd::CONNECT_TIMEOUT_MS)) {
                        disconnectBle();
                        handleConnectFailure(nowMs, ObdFailureReason::SUBSCRIBE);
                        break;
                    }
                    break;
                }
                if (takeTransportResult(ObdTransportOp::SUBSCRIBE, transportResult)) {
                    if (!transportResult.success || transportResult.timedOut) {
                        disconnectBle();
                        handleConnectFailure(nowMs, ObdFailureReason::SUBSCRIBE);
                        break;
                    }
                    resetInitState(preferWarmReconnect_);
                    preferWarmReconnect_ = true;
                    transitionTo(ObdConnectionState::AT_INIT, nowMs);
                    break;
                }
                if (!transportRequestActive_ && !readyTransportResult_.ready &&
                    !beginTransportRequest(ObdTransportOp::DISCOVER, nowMs, obd::CONNECT_TIMEOUT_MS)) {
                    disconnectBle();
                    handleConnectFailure(nowMs, ObdFailureReason::DISCOVERY);
                    break;
                }
            }
            break;

        case ObdConnectionState::AT_INIT:
            updateAtInit(nowMs);
            break;

        case ObdConnectionState::POLLING:
            updatePolling(nowMs);
            break;

        case ObdConnectionState::ERROR_BACKOFF:
            if ((nowMs - stateEnteredMs_) >= obd::ERROR_PAUSE_MS) {
                if (shouldDisconnectAfterPollingError(lastFailure_) &&
                    consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT) {
                    disconnectBle();
                    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                } else {
                    backoffCycles_++;
                    if (!shouldDisconnectAfterPollingError(lastFailure_)) {
                        consecutiveErrors_ = 0;
                    }
                    if (backoffCycles_ >= obd::ECU_IDLE_BACKOFF_THRESHOLD) {
                        v1WasConnectedAtEcuIdle_ = v1Connected;
                        disconnectBle();
                        clearSpeedState();
                        transitionTo(ObdConnectionState::ECU_IDLE, nowMs);
                    } else {
                        transitionTo(ObdConnectionState::POLLING, nowMs);
                    }
                }
            }
            break;

        case ObdConnectionState::DISCONNECTED:
            if (justEntered) {
                clearBleResponseState();
                resetCommandState();
                disconnectBle();
            }
            if (scanRequested_ && bleScanIdle) {
                if (startBleScan()) {
                    scanRequested_ = false;
                    transitionTo(ObdConnectionState::SCANNING, nowMs);
                    break;
                }
            }
            if ((nowMs - stateEnteredMs_) >= obd::RECONNECT_BACKOFF_MS) {
                if (proxyClientConnected) {
                    break;
                }
                if (savedAddress_[0] != '\0') {
                    setConnectTargetFromSaved();
                    transitionTo(ObdConnectionState::CONNECTING, nowMs);
                } else {
                    transitionTo(ObdConnectionState::IDLE, nowMs);
                }
            }
            break;

        case ObdConnectionState::ECU_IDLE:
            if (justEntered) {
                clearBleResponseState();
                resetCommandState();
            }

            // Resume path 1: V1 reconnected (was disconnected when we entered ECU_IDLE)
            if (!v1WasConnectedAtEcuIdle_ && v1Connected) {
                backoffCycles_ = 0;
                transitionTo(ObdConnectionState::WAIT_BOOT, nowMs);
                break;
            }

            // Resume path 2: slow probe — try reconnecting to OBDLink periodically
            if ((nowMs - stateEnteredMs_) >= obd::ECU_IDLE_PROBE_INTERVAL_MS) {
                if (proxyClientConnected) {
                    // Don't compete with an active proxy session; just reset timer
                    stateEnteredMs_ = nowMs;
                    break;
                }
                if (savedAddress_[0] != '\0') {
                    setConnectTargetFromSaved();
                    preferWarmReconnect_ = true;
                    transitionTo(ObdConnectionState::CONNECTING, nowMs);
                }
            }
            break;
    }
}

ObdBleArbitrationRequest ObdRuntimeModule::getBleArbitrationRequest() const {
    if (manualScanPreemptProxy_) {
        return ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN;
    }
    return shouldHoldProxyForAutoObd()
               ? ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD
               : ObdBleArbitrationRequest::NONE;
}

ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t nowMs) const {
#ifndef UNIT_TEST
    // Guard against calls before begin() — bleClient_ is still nullptr and
    // the helper methods (isBleConnected, etc.) would dereference it.
    if (!bleClient_) return ObdRuntimeStatus{};
#endif

    ObdRuntimeStatus status;
    status.enabled = enabled_;
    status.state = state_;
    status.connected = isBleConnected() ||
                       state_ == ObdConnectionState::SECURING ||
                       state_ == ObdConnectionState::DISCOVERING ||
                       state_ == ObdConnectionState::AT_INIT ||
                       state_ == ObdConnectionState::POLLING ||
                       state_ == ObdConnectionState::ERROR_BACKOFF;
    status.securityReady = isBleSecurityReady();
    status.encrypted = isBleEncrypted();
    status.bonded = isBleBonded();
    status.speedValid = isSpeedFresh(nowMs);
    status.speedMph = speedMph_;
    status.speedSampleTsMs = speedSampleTsMs_;
    status.speedAgeMs = status.speedValid ? (nowMs - speedSampleTsMs_) : UINT32_MAX;
    status.rssi = rssi_;
    status.connectAttempts = connectAttempts_;
    status.connectSuccesses = connectSuccesses_;
    status.connectFailures = connectFailures_;
    status.securityRepairs = securityRepairs_;
    status.scanInProgress = (state_ == ObdConnectionState::SCANNING);
    status.manualScanPending = manualScanPending_;
    status.savedAddressValid = savedAddress_[0] != '\0';
    status.initRetries = initRetries_;
    status.pollCount = pollCount_;
    status.pollErrors = pollErrors_;
    status.staleSpeedCount = staleSpeedCount_;
    status.consecutiveErrors = consecutiveErrors_;
    status.totalBytesReceived = totalBytesReceived_;
    status.bufferOverflows = bufferOverflowCount_;
    status.lastConnectStartMs = lastConnectStartMs_;
    status.lastConnectSuccessMs = lastConnectSuccessMs_;
    status.lastFailureMs = lastFailureMs_;
    status.lastBleError = getBleLastError();
    status.lastSecurityError = getBleSecurityFailure();
    status.lastFailure = lastFailure_;
    status.commandInFlight = activeCommand_.active ? activeCommand_.kind : ObdCommandKind::NONE;
    return status;
}

bool ObdRuntimeModule::getFreshSpeed(uint32_t nowMs,
                                     float& speedMphOut,
                                     uint32_t& tsMsOut) const {
    if (!isSpeedFresh(nowMs)) return false;
    speedMphOut = speedMph_;
    tsMsOut = speedSampleTsMs_;
    return true;
}

bool ObdRuntimeModule::startScan() {
    if (!enabled_ || state_ == ObdConnectionState::SCANNING || scanRequested_) return false;
    scanRequested_ = true;
    return true;
}

bool ObdRuntimeModule::requestManualPairScan(uint32_t nowMs) {
    if (!enabled_ || isBleConnected() || manualScanPending_ || scanRequested_ ||
        state_ == ObdConnectionState::SCANNING) {
        return false;
    }

    stopBleScan();
    disconnectBle();
    clearBleEventQueue();
    clearBleResponseState();
    resetCommandState();
    clearTransportRequest();
    readyTransportResult_ = {};
    bleDisconnected_ = false;
    pendingDeviceFound_ = false;
    pendingAddress_[0] = '\0';
    connectAttempts_ = 0;
    clearManualScanState();
    manualScanPending_ = true;
    manualScanPreemptProxy_ = true;
    scanRequested_ = true;
    state_ = ObdConnectionState::IDLE;
    stateEnteredMs_ = nowMs;
    stateEntryPending_ = false;
    return true;
}

void ObdRuntimeModule::forgetDevice() {
    stopBleScan();
    disconnectBle();
    clearBleEventQueue();
    setSavedAddressFromBuffer("");
    pendingAddress_[0] = '\0';
    pendingDeviceFound_ = false;
    scanRequested_ = false;
    clearManualScanState();
    connectAttempts_ = 0;
    clearSpeedState();
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    state_ = ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
    stateEntryPending_ = false;
}

void ObdRuntimeModule::onDeviceFound(const char* name, const char* address, int rssi, uint8_t addrType) {
    if (!address || address[0] == '\0') {
        return;
    }
    if (name && strcmp(name, obd::DEVICE_NAME_CX) != 0) {
        return;
    }

    BleEvent event{};
    event.type = BleEventType::DEVICE_FOUND;
    event.rssi = static_cast<int8_t>(rssi);
    event.addrType = addrType;
    copyString(event.address, sizeof(event.address), address);
    enqueueBleEvent(event);
}

void ObdRuntimeModule::onBleDisconnect(int reason) {
    BleEvent event{};
    event.type = BleEventType::DISCONNECT;
    event.disconnectReason = reason;
    enqueueBleEvent(event);
}

void ObdRuntimeModule::onBleData(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    BleEvent event{};
    event.type = BleEventType::DATA;
    event.dataLen = std::min(len, BLE_BUF_LEN - 1);
    if (event.dataLen > 0) {
        memcpy(event.data, data, event.dataLen);
    }
    event.overflowed = event.dataLen < len;
    event.dataReady = memchr(data, '>', len) != nullptr;
    enqueueBleEvent(event);
}

const char* ObdRuntimeModule::bleReasonName(int reason) {
    switch (reason) {
        case 0:
            return "none";
        case 520:
            return "supervision_timeout";
        case 534:
            return "local_host_terminated";
#ifndef UNIT_TEST
        case BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING):
            return "pinkey_missing";
        case BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL):
            return "auth_fail";
        case BLE_HS_HCI_ERR(BLE_ERR_NO_PAIRING):
            return "no_pairing";
#endif
        default:
            return "unknown";
    }
}

bool ObdRuntimeModule::isSecurityBleError(int error) {
#ifndef UNIT_TEST
    switch (error) {
        case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN):
        case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHOR):
        case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC):
        case BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_KEY_SZ):
        case BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING):
        case BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL):
        case BLE_HS_HCI_ERR(BLE_ERR_NO_PAIRING):
        case BLE_HS_HCI_ERR(BLE_ERR_INSUFFICIENT_SEC):
            return true;
        default:
            return false;
    }
#else
    return error != 0;
#endif
}

bool ObdRuntimeModule::canAutoHealBond() const {
    return !connectTargetFromManualCandidate_ &&
           savedAddress_[0] != '\0' &&
           strcmp(repairedBondAddress_, savedAddress_) != 0;
}

bool ObdRuntimeModule::autoHealBondIfAllowed(uint32_t nowMs, const char* context) {
    if (!canAutoHealBond()) {
        return false;
    }

    if (!deleteBleBond()) {
        return false;
    }

#ifndef UNIT_TEST
    Serial.printf("[OBD] auto-heal bond during %s addr=%s lastBleError=%d (%s) lastSecurityError=%d (%s)\n",
                  context ? context : "unknown",
                  savedAddress_,
                  getBleLastError(),
                  bleReasonName(getBleLastError()),
                  getBleSecurityFailure(),
                  bleReasonName(getBleSecurityFailure()));
#endif

    disconnectBle();
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
    preferWarmReconnect_ = false;
    warmInitPreferred_ = false;
    copyString(repairedBondAddress_, sizeof(repairedBondAddress_), savedAddress_);
    securityRepairs_++;
    refreshBleBondBackup();
    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
    return true;
}

#ifdef UNIT_TEST
void ObdRuntimeModule::injectSpeedForTest(float speedMph, uint32_t timestampMs) {
    speedMph_ = speedMph;
    speedSampleTsMs_ = timestampMs;
    speedValid_ = true;
    consecutiveErrors_ = 0;
    backoffCycles_ = 0;
}

void ObdRuntimeModule::forceStateForTest(ObdConnectionState state, uint32_t enteredMs) {
    state_ = state;
    stateEnteredMs_ = enteredMs;
    stateEntryPending_ = false;
    clearBleEventQueue();
    clearBleResponseState();
    resetCommandState();
    bleDisconnected_ = false;
}

void ObdRuntimeModule::transitionToPollingForTest(uint32_t nowMs) {
    transitionTo(ObdConnectionState::POLLING, nowMs);
}

ObdCommandKind ObdRuntimeModule::getActiveCommandKindForTest() const {
    return activeCommand_.active ? activeCommand_.kind : ObdCommandKind::NONE;
}
#endif
