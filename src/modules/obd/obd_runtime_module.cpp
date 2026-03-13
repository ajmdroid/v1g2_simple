#include "obd_runtime_module.h"
#include "obd_elm327_parser.h"
#include "obd_scan_policy.h"

#ifndef UNIT_TEST
#include "obd_ble_client.h"
#endif

#include <cstring>

ObdRuntimeModule obdRuntimeModule;

void ObdRuntimeModule::begin(bool enabled, const char* savedAddress, int8_t minRssi) {
    enabled_ = enabled;
    minRssi_ = minRssi;

    savedAddress_[0] = '\0';
    if (savedAddress != nullptr && savedAddress[0] != '\0') {
        strncpy(savedAddress_, savedAddress, ADDR_BUF_LEN - 1);
        savedAddress_[ADDR_BUF_LEN - 1] = '\0';
    }

    pendingAddress_[0] = '\0';
    pendingDeviceFound_ = false;
    scanRequested_ = false;
    connectAttempts_ = 0;
    pollCount_ = 0;
    pollErrors_ = 0;
    consecutiveErrors_ = 0;
    totalBytesReceived_ = 0;
    lastPollMs_ = 0;
    pollResponsePending_ = false;
    pollBackoffApplied_ = false;
    lastRssiMs_ = 0;
    speedValid_ = false;
    speedMph_ = 0.0f;
    speedSampleTsMs_ = 0;
    rssi_ = 0;
    pendingRssi_ = 0;
    bootReadyMs_ = 0;
    atInitIndex_ = 0;
    atInitSentMs_ = 0;
    stateEntryPending_ = false;
    clearBleResponseState();
    bleDisconnected_ = false;

#ifdef UNIT_TEST
    testStartScanResult_ = true;
    testConnectResult_ = true;
    testBleConnected_ = false;
    testDiscoverResult_ = true;
    testSubscribeResult_ = true;
    testWriteResult_ = true;
    testRssi_ = 0;
    testStartScanCalls_ = 0;
    testConnectCalls_ = 0;
    testDiscoverCalls_ = 0;
    testDisconnectCalls_ = 0;
    testWriteCalls_ = 0;
    testLastCommand_[0] = '\0';
#else
    obdBleClient.init(this);
#endif

    if (!enabled_) {
        state_ = ObdConnectionState::IDLE;
        stateEnteredMs_ = 0;
        return;
    }

    state_ = (savedAddress_[0] != '\0') ? ObdConnectionState::WAIT_BOOT : ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
}

void ObdRuntimeModule::transitionTo(ObdConnectionState newState, uint32_t nowMs) {
    state_ = newState;
    stateEnteredMs_ = nowMs;
    stateEntryPending_ = true;
}

void ObdRuntimeModule::clearBleResponseState() {
    bleBufLen_ = 0;
    bleBuf_[0] = '\0';
    bleDataReady_ = false;
    pollResponsePending_ = false;
}

void ObdRuntimeModule::setSavedAddressFromBuffer(const char* address) {
    savedAddress_[0] = '\0';
    if (address != nullptr && address[0] != '\0') {
        strncpy(savedAddress_, address, ADDR_BUF_LEN - 1);
        savedAddress_[ADDR_BUF_LEN - 1] = '\0';
    }
}

void ObdRuntimeModule::handleConnectFailure(uint32_t nowMs) {
    clearBleResponseState();
    bleDisconnected_ = false;
    pollBackoffApplied_ = false;
    atInitIndex_ = 0;
    atInitSentMs_ = 0;
    connectAttempts_++;
    if (connectAttempts_ >= obd::MAX_DIRECT_CONNECT_FAILURES) {
        savedAddress_[0] = '\0';
        connectAttempts_ = 0;
        transitionTo(ObdConnectionState::IDLE, nowMs);
        return;
    }
    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
}

void ObdRuntimeModule::handlePollingError(uint32_t nowMs, bool disconnectBleNow) {
    pollErrors_++;
    consecutiveErrors_++;
    clearBleResponseState();
    if (consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT) {
        if (disconnectBleNow) {
            disconnectBle();
        }
        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
    }
}

bool ObdRuntimeModule::startBleScan() {
#ifndef UNIT_TEST
    return obdBleClient.startScan(minRssi_);
#else
    testStartScanCalls_++;
    return testStartScanResult_;
#endif
}

bool ObdRuntimeModule::connectBle(uint32_t timeoutMs) {
#ifndef UNIT_TEST
    return obdBleClient.connect(savedAddress_, timeoutMs);
#else
    (void)timeoutMs;
    testConnectCalls_++;
    return testConnectResult_;
#endif
}

bool ObdRuntimeModule::isBleConnected() const {
#ifndef UNIT_TEST
    return obdBleClient.isConnected();
#else
    return testBleConnected_;
#endif
}

bool ObdRuntimeModule::discoverBleServices() {
#ifndef UNIT_TEST
    return obdBleClient.discoverServices();
#else
    testDiscoverCalls_++;
    return testDiscoverResult_;
#endif
}

bool ObdRuntimeModule::subscribeBleNotifications() {
#ifndef UNIT_TEST
    return obdBleClient.subscribeNotify([](const uint8_t* data, size_t len) {
        obdRuntimeModule.onBleData(data, len);
    });
#else
    return testSubscribeResult_;
#endif
}

bool ObdRuntimeModule::writeBleCommand(const char* cmd) {
#ifndef UNIT_TEST
    return obdBleClient.writeCommand(cmd);
#else
    testWriteCalls_++;
    if (cmd != nullptr) {
        strncpy(testLastCommand_, cmd, sizeof(testLastCommand_) - 1);
        testLastCommand_[sizeof(testLastCommand_) - 1] = '\0';
    } else {
        testLastCommand_[0] = '\0';
    }
    return testWriteResult_;
#endif
}

void ObdRuntimeModule::disconnectBle() {
#ifndef UNIT_TEST
    obdBleClient.disconnect();
#else
    testDisconnectCalls_++;
    testBleConnected_ = false;
#endif
}

void ObdRuntimeModule::stopBleScan() {
#ifndef UNIT_TEST
    obdBleClient.stopScan();
#endif
}

int8_t ObdRuntimeModule::readBleRssi(uint32_t nowMs) {
#ifndef UNIT_TEST
    return obdBleClient.getRssi(nowMs);
#else
    (void)nowMs;
    return testRssi_;
#endif
}

void ObdRuntimeModule::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;

    if (!enabled_) {
        stopBleScan();
        disconnectBle();
        scanRequested_ = false;
        pendingDeviceFound_ = false;
        pendingAddress_[0] = '\0';
        clearBleResponseState();
        bleDisconnected_ = false;
        pollBackoffApplied_ = false;
        speedValid_ = false;
        transitionTo(ObdConnectionState::IDLE, 0);
        stateEntryPending_ = false;
        return;
    }

    speedValid_ = false;
    connectAttempts_ = 0;
    pollBackoffApplied_ = false;
    clearBleResponseState();
    bleDisconnected_ = false;
    state_ = (savedAddress_[0] != '\0') ? ObdConnectionState::WAIT_BOOT : ObdConnectionState::IDLE;
    stateEnteredMs_ = 0;
    stateEntryPending_ = false;
}

void ObdRuntimeModule::setMinRssi(int8_t minRssi) {
    if (minRssi < -90) minRssi = -90;
    if (minRssi > -40) minRssi = -40;
    minRssi_ = minRssi;
}

void ObdRuntimeModule::update(uint32_t nowMs, bool bootReady, bool v1Connected, bool bleScanIdle) {
    if (!enabled_) return;

    if (bootReady && bootReadyMs_ == 0) {
        bootReadyMs_ = nowMs;
        if (bootReadyMs_ == 0) bootReadyMs_ = 1;
    }

    const bool justEntered = stateEntryPending_;
    stateEntryPending_ = false;

    switch (state_) {
        case ObdConnectionState::IDLE: {
            if (scanRequested_ && bleScanIdle) {
                if (!startBleScan()) break;
                scanRequested_ = false;
                transitionTo(ObdConnectionState::SCANNING, nowMs);
            }
            break;
        }

        case ObdConnectionState::WAIT_BOOT: {
            if (!bootReady) break;

            const uint32_t elapsed = nowMs - bootReadyMs_;
            if (v1Connected || elapsed >= obd::POST_BOOT_DWELL_MS) {
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            }
            break;
        }

        case ObdConnectionState::SCANNING: {
            const uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::SCAN_DURATION_MS) {
                transitionTo(ObdConnectionState::IDLE, nowMs);
                break;
            }
            if (pendingDeviceFound_) {
                pendingDeviceFound_ = false;
                setSavedAddressFromBuffer(pendingAddress_);
                rssi_ = pendingRssi_;
                connectAttempts_ = 0;
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            }
            break;
        }

        case ObdConnectionState::CONNECTING: {
            if (bleDisconnected_) {
                handleConnectFailure(nowMs);
                break;
            }

            if (justEntered && !connectBle(obd::CONNECT_TIMEOUT_MS)) {
                handleConnectFailure(nowMs);
                break;
            }

            if (isBleConnected()) {
                connectAttempts_ = 0;
                transitionTo(ObdConnectionState::DISCOVERING, nowMs);
                break;
            }

            const uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::CONNECT_TIMEOUT_MS) {
                disconnectBle();
                handleConnectFailure(nowMs);
            }
            break;
        }

        case ObdConnectionState::DISCOVERING: {
            if (bleDisconnected_) {
                bleDisconnected_ = false;
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }

            if (justEntered && discoverBleServices()) {
                if (!subscribeBleNotifications()) {
                    disconnectBle();
                    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                    break;
                }
                atInitIndex_ = 0;
                atInitSentMs_ = 0;
                transitionTo(ObdConnectionState::AT_INIT, nowMs);
                break;
            }

            const uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::CONNECT_TIMEOUT_MS) {
                connectAttempts_++;
                disconnectBle();
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
            }
            break;
        }

        case ObdConnectionState::AT_INIT: {
            if (bleDisconnected_) {
                bleDisconnected_ = false;
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }

            if (atInitIndex_ < obd::AT_INIT_COMMAND_COUNT) {
                if (atInitSentMs_ == 0) {
                    if (writeBleCommand(obd::AT_INIT_COMMANDS[atInitIndex_])) {
                        atInitSentMs_ = nowMs;
                    } else {
                        disconnectBle();
                        transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                        break;
                    }
                } else if (bleDataReady_) {
                    clearBleResponseState();
                    atInitIndex_++;
                    atInitSentMs_ = 0;
                } else if (nowMs - atInitSentMs_ >= obd::AT_INIT_RESPONSE_TIMEOUT_MS) {
                    atInitIndex_++;
                    atInitSentMs_ = 0;
                }
            }

            if (atInitIndex_ >= obd::AT_INIT_COMMAND_COUNT) {
                consecutiveErrors_ = 0;
                pollBackoffApplied_ = false;
                lastPollMs_ = 0;
                clearBleResponseState();
                transitionTo(ObdConnectionState::POLLING, nowMs);
                break;
            }

            const uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::AT_INIT_RESPONSE_TIMEOUT_MS * obd::AT_INIT_COMMAND_COUNT) {
                consecutiveErrors_++;
                disconnectBle();
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
            }
            break;
        }

        case ObdConnectionState::POLLING: {
            if (bleDisconnected_) {
                bleDisconnected_ = false;
                speedValid_ = false;
                clearBleResponseState();
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }

            if (consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT) {
                disconnectBle();
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }

            if (consecutiveErrors_ >= obd::MAX_CONSECUTIVE_ERRORS && !pollBackoffApplied_) {
                pollBackoffApplied_ = true;
                transitionTo(ObdConnectionState::ERROR_BACKOFF, nowMs);
                break;
            }

            if (bleDataReady_) {
                bleDataReady_ = false;
                totalBytesReceived_ += bleBufLen_;
                Elm327ParseResult result = parseElm327Response(bleBuf_, bleBufLen_);
                bleBufLen_ = 0;
                bleBuf_[0] = '\0';
                pollResponsePending_ = false;

                if (result.valid) {
                    const float kmh = decodeSpeedKmh(result);
                    if (kmh >= 0.0f) {
                        speedMph_ = kmhToMph(kmh);
                        speedSampleTsMs_ = nowMs;
                        speedValid_ = true;
                        consecutiveErrors_ = 0;
                        pollBackoffApplied_ = false;
                    }
                    pollCount_++;
                } else if (result.noData || result.error || !result.busInit) {
                    handlePollingError(nowMs, true);
                    break;
                }
            }

            if (pollResponsePending_ && (nowMs - lastPollMs_) >= obd::POLL_TIMEOUT_MS) {
                handlePollingError(nowMs, true);
                break;
            }

            if (!pollResponsePending_ &&
                (lastPollMs_ == 0 || (nowMs - lastPollMs_) >= obd::POLL_INTERVAL_MS)) {
                clearBleResponseState();
                if (writeBleCommand(obd::SPEED_POLL_CMD)) {
                    lastPollMs_ = nowMs;
                    pollResponsePending_ = true;
                } else {
                    handlePollingError(nowMs, true);
                    break;
                }
            }

            rssi_ = readBleRssi(nowMs);

            if (speedValid_) {
                const uint32_t age = nowMs - speedSampleTsMs_;
                if (age > obd::SPEED_MAX_AGE_MS) {
                    speedValid_ = false;
                }
            }
            break;
        }

        case ObdConnectionState::ERROR_BACKOFF: {
            if (bleDisconnected_) {
                bleDisconnected_ = false;
                speedValid_ = false;
                clearBleResponseState();
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }

            const uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::ERROR_PAUSE_MS) {
                if (consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT) {
                    disconnectBle();
                    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                } else {
                    transitionTo(ObdConnectionState::POLLING, nowMs);
                }
            }
            break;
        }

        case ObdConnectionState::DISCONNECTED: {
            speedValid_ = false;
            if (justEntered) {
                clearBleResponseState();
                disconnectBle();
            }
            const uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::RECONNECT_BACKOFF_MS) {
                if (savedAddress_[0] != '\0') {
                    transitionTo(ObdConnectionState::CONNECTING, nowMs);
                } else {
                    transitionTo(ObdConnectionState::IDLE, nowMs);
                }
            }
            break;
        }
    }
}

ObdRuntimeStatus ObdRuntimeModule::snapshot(uint32_t nowMs) const {
    ObdRuntimeStatus status;
    status.enabled = enabled_;
    status.state = state_;
    status.connected = (state_ == ObdConnectionState::DISCOVERING ||
                        state_ == ObdConnectionState::AT_INIT ||
                        state_ == ObdConnectionState::POLLING ||
                        state_ == ObdConnectionState::ERROR_BACKOFF);
    status.speedValid = speedValid_;
    status.speedMph = speedMph_;
    status.speedSampleTsMs = speedSampleTsMs_;
    status.speedAgeMs = speedValid_ ? (nowMs - speedSampleTsMs_) : UINT32_MAX;
    status.rssi = rssi_;
    status.connectAttempts = connectAttempts_;
    status.scanInProgress = (state_ == ObdConnectionState::SCANNING);
    status.savedAddressValid = (savedAddress_[0] != '\0');
    status.pollCount = pollCount_;
    status.pollErrors = pollErrors_;
    status.consecutiveErrors = consecutiveErrors_;
    status.totalBytesReceived = totalBytesReceived_;
    return status;
}

bool ObdRuntimeModule::getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const {
    if (!speedValid_) return false;
    const uint32_t age = nowMs - speedSampleTsMs_;
    if (age > obd::SPEED_MAX_AGE_MS) return false;
    speedMphOut = speedMph_;
    tsMsOut = speedSampleTsMs_;
    return true;
}

void ObdRuntimeModule::startScan() {
    if (!enabled_) return;
    if (state_ == ObdConnectionState::SCANNING) return;
    scanRequested_ = true;
}

void ObdRuntimeModule::forgetDevice() {
    stopBleScan();
    disconnectBle();
    setSavedAddressFromBuffer("");
    pendingAddress_[0] = '\0';
    pendingDeviceFound_ = false;
    scanRequested_ = false;
    connectAttempts_ = 0;
    pollBackoffApplied_ = false;
    speedValid_ = false;
    clearBleResponseState();
    bleDisconnected_ = false;
    if (state_ != ObdConnectionState::IDLE) {
        transitionTo(ObdConnectionState::IDLE, 0);
        stateEntryPending_ = false;
    }
}

void ObdRuntimeModule::onDeviceFound(const char* name, const char* address, int rssi) {
    (void)name;
    if (address == nullptr || address[0] == '\0') return;
    if (rssi < minRssi_) return;
    if (state_ != ObdConnectionState::SCANNING) return;

    strncpy(pendingAddress_, address, ADDR_BUF_LEN - 1);
    pendingAddress_[ADDR_BUF_LEN - 1] = '\0';
    pendingRssi_ = static_cast<int8_t>(rssi);
    pendingDeviceFound_ = true;
}

void ObdRuntimeModule::onBleDisconnect() {
    bleDisconnected_ = true;
}

void ObdRuntimeModule::onBleData(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    const size_t space = BLE_BUF_LEN - 1 - bleBufLen_;
    const size_t toCopy = (len < space) ? len : space;
    memcpy(bleBuf_ + bleBufLen_, data, toCopy);
    bleBufLen_ += toCopy;
    bleBuf_[bleBufLen_] = '\0';

    if (memchr(bleBuf_, '>', bleBufLen_) != nullptr) {
        bleDataReady_ = true;
    }
}

#ifdef UNIT_TEST
void ObdRuntimeModule::injectSpeedForTest(float speedMph, uint32_t timestampMs) {
    speedMph_ = speedMph;
    speedSampleTsMs_ = timestampMs;
    speedValid_ = true;
    consecutiveErrors_ = 0;
    pollBackoffApplied_ = false;
}

void ObdRuntimeModule::forceStateForTest(ObdConnectionState state, uint32_t enteredMs) {
    state_ = state;
    stateEnteredMs_ = enteredMs;
    stateEntryPending_ = false;
    clearBleResponseState();
    bleDisconnected_ = false;
}
#endif
