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
    lastRssiMs_ = 0;
    speedValid_ = false;
    speedMph_ = 0.0f;
    speedSampleTsMs_ = 0;
    rssi_ = 0;
    pendingRssi_ = 0;
    bootReadyMs_ = 0;
    atInitIndex_ = 0;
    atInitSentMs_ = 0;
    bleBufLen_ = 0;
    bleDataReady_ = false;
    bleDisconnected_ = false;

#ifndef UNIT_TEST
    obdBleClient.init(this);
#endif

    if (!enabled_) {
        state_ = ObdConnectionState::IDLE;
        stateEnteredMs_ = 0;
        return;
    }

    if (savedAddress_[0] != '\0') {
        state_ = ObdConnectionState::WAIT_BOOT;
    } else {
        state_ = ObdConnectionState::IDLE;
    }
    stateEnteredMs_ = 0;
}

void ObdRuntimeModule::transitionTo(ObdConnectionState newState, uint32_t nowMs) {
    state_ = newState;
    stateEnteredMs_ = nowMs;
}

void ObdRuntimeModule::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    if (!enabled_) {
        transitionTo(ObdConnectionState::IDLE, 0);
        speedValid_ = false;
    }
}

void ObdRuntimeModule::update(uint32_t nowMs, bool bootReady, bool v1Connected, bool bleScanIdle) {
    if (!enabled_) return;

    // Track when boot became ready
    if (bootReady && bootReadyMs_ == 0) {
        bootReadyMs_ = nowMs;
        if (bootReadyMs_ == 0) bootReadyMs_ = 1;
    }

    switch (state_) {
        case ObdConnectionState::IDLE: {
            // Waiting for web UI scan trigger
            if (scanRequested_ && bleScanIdle) {
                scanRequested_ = false;
#ifndef UNIT_TEST
                if (!obdBleClient.startScan(minRssi_)) break;
#endif
                transitionTo(ObdConnectionState::SCANNING, nowMs);
            }
            break;
        }

        case ObdConnectionState::WAIT_BOOT: {
            if (!bootReady) break;

            // Wait for V1 to connect or post-boot dwell
            uint32_t elapsed = nowMs - bootReadyMs_;
            if (v1Connected || elapsed >= obd::POST_BOOT_DWELL_MS) {
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            }
            break;
        }

        case ObdConnectionState::SCANNING: {
            // Scan is in progress (driven externally by BLE scan callback).
            // Check for timeout.
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::SCAN_DURATION_MS) {
                // Scan timed out with no device found
                transitionTo(ObdConnectionState::IDLE, nowMs);
                break;
            }
            // Check for device found during scan
            if (pendingDeviceFound_) {
                pendingDeviceFound_ = false;
                strncpy(savedAddress_, pendingAddress_, ADDR_BUF_LEN - 1);
                savedAddress_[ADDR_BUF_LEN - 1] = '\0';
                connectAttempts_ = 0;
                transitionTo(ObdConnectionState::CONNECTING, nowMs);
            }
            break;
        }

        case ObdConnectionState::CONNECTING: {
            // Handle async disconnect signal
            if (bleDisconnected_) {
                bleDisconnected_ = false;
                connectAttempts_++;
                if (connectAttempts_ >= obd::MAX_DIRECT_CONNECT_FAILURES) {
                    savedAddress_[0] = '\0';
                    connectAttempts_ = 0;
                    transitionTo(ObdConnectionState::IDLE, nowMs);
                } else {
                    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                }
                break;
            }
#ifndef UNIT_TEST
            // Initiate connection on state entry
            if (nowMs == stateEnteredMs_) {
                obdBleClient.connect(savedAddress_, obd::CONNECT_TIMEOUT_MS);
            }
            // Check if connection succeeded
            if (obdBleClient.isConnected()) {
                connectAttempts_ = 0;
                transitionTo(ObdConnectionState::DISCOVERING, nowMs);
                break;
            }
#endif
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::CONNECT_TIMEOUT_MS) {
                connectAttempts_++;
#ifndef UNIT_TEST
                obdBleClient.disconnect();
#endif
                if (connectAttempts_ >= obd::MAX_DIRECT_CONNECT_FAILURES) {
                    savedAddress_[0] = '\0';
                    connectAttempts_ = 0;
                    transitionTo(ObdConnectionState::IDLE, nowMs);
                } else {
                    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                }
            }
            break;
        }

        case ObdConnectionState::DISCOVERING: {
            if (bleDisconnected_) {
                bleDisconnected_ = false;
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }
#ifndef UNIT_TEST
            if (nowMs == stateEnteredMs_) {
                if (obdBleClient.discoverServices()) {
                    // Subscribe to notifications for response data
                    obdBleClient.subscribeNotify([](const uint8_t* data, size_t len) {
                        obdRuntimeModule.onBleData(data, len);
                    });
                    atInitIndex_ = 0;
                    atInitSentMs_ = 0;
                    transitionTo(ObdConnectionState::AT_INIT, nowMs);
                    break;
                }
            }
#endif
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::CONNECT_TIMEOUT_MS) {
                connectAttempts_++;
#ifndef UNIT_TEST
                obdBleClient.disconnect();
#endif
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
#ifndef UNIT_TEST
            // Send AT init commands one at a time, waiting for response
            if (atInitIndex_ < obd::AT_INIT_COMMAND_COUNT) {
                if (atInitSentMs_ == 0) {
                    // Send next command
                    obdBleClient.writeCommand(obd::AT_INIT_COMMANDS[atInitIndex_]);
                    atInitSentMs_ = nowMs;
                } else if (bleDataReady_) {
                    // Got response, move to next command
                    bleDataReady_ = false;
                    bleBufLen_ = 0;
                    atInitIndex_++;
                    atInitSentMs_ = 0;
                } else if (nowMs - atInitSentMs_ >= obd::AT_INIT_RESPONSE_TIMEOUT_MS) {
                    // Timeout on this command — skip and try next
                    atInitIndex_++;
                    atInitSentMs_ = 0;
                }
            }
            if (atInitIndex_ >= obd::AT_INIT_COMMAND_COUNT) {
                consecutiveErrors_ = 0;
                lastPollMs_ = 0;
                transitionTo(ObdConnectionState::POLLING, nowMs);
                break;
            }
#endif
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::AT_INIT_RESPONSE_TIMEOUT_MS * obd::AT_INIT_COMMAND_COUNT) {
                consecutiveErrors_++;
#ifndef UNIT_TEST
                obdBleClient.disconnect();
#endif
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
            }
            break;
        }

        case ObdConnectionState::POLLING: {
            if (bleDisconnected_) {
                bleDisconnected_ = false;
                speedValid_ = false;
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                break;
            }

            // Check for error backoff threshold.
            if (consecutiveErrors_ >= obd::MAX_CONSECUTIVE_ERRORS) {
                transitionTo(ObdConnectionState::ERROR_BACKOFF, nowMs);
                break;
            }

#ifndef UNIT_TEST
            // Process incoming BLE data
            if (bleDataReady_) {
                bleDataReady_ = false;
                totalBytesReceived_ += bleBufLen_;
                Elm327ParseResult result = parseElm327Response(bleBuf_, bleBufLen_);
                bleBufLen_ = 0;

                if (result.valid) {
                    float kmh = decodeSpeedKmh(result);
                    if (kmh >= 0.0f) {
                        speedMph_ = kmhToMph(kmh);
                        speedSampleTsMs_ = nowMs;
                        speedValid_ = true;
                        consecutiveErrors_ = 0;
                    }
                    pollCount_++;
                } else if (result.noData) {
                    pollErrors_++;
                    consecutiveErrors_++;
                } else if (result.error) {
                    pollErrors_++;
                    consecutiveErrors_++;
                } else if (!result.busInit) {
                    pollErrors_++;
                    consecutiveErrors_++;
                }
            }

            // Send poll command at interval
            if (lastPollMs_ == 0 || (nowMs - lastPollMs_) >= obd::POLL_INTERVAL_MS) {
                obdBleClient.writeCommand(obd::SPEED_POLL_CMD);
                lastPollMs_ = nowMs;
            }

            // Update cached RSSI
            rssi_ = obdBleClient.getRssi(nowMs);
#endif

            // Speed staleness check
            if (speedValid_) {
                uint32_t age = nowMs - speedSampleTsMs_;
                if (age > obd::SPEED_MAX_AGE_MS) {
                    speedValid_ = false;
                }
            }
            break;
        }

        case ObdConnectionState::ERROR_BACKOFF: {
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::ERROR_PAUSE_MS) {
                consecutiveErrors_ = 0;
                if (consecutiveErrors_ >= obd::ERRORS_BEFORE_DISCONNECT) {
                    transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
                } else {
                    transitionTo(ObdConnectionState::POLLING, nowMs);
                }
            }
            break;
        }

        case ObdConnectionState::DISCONNECTED: {
            speedValid_ = false;
#ifndef UNIT_TEST
            // Ensure BLE client is disconnected
            if (nowMs == stateEnteredMs_) {
                obdBleClient.disconnect();
            }
#endif
            uint32_t elapsed = nowMs - stateEnteredMs_;
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
    status.connected = (state_ == ObdConnectionState::POLLING ||
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
    uint32_t age = nowMs - speedSampleTsMs_;
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
    savedAddress_[0] = '\0';
    connectAttempts_ = 0;
    speedValid_ = false;
    if (state_ != ObdConnectionState::IDLE) {
        transitionTo(ObdConnectionState::IDLE, 0);
    }
}

void ObdRuntimeModule::onDeviceFound(const char* name, const char* address, int rssi) {
    (void)name;
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
    // Append to buffer (handle fragmented BLE responses)
    size_t space = BLE_BUF_LEN - 1 - bleBufLen_;
    size_t toCopy = (len < space) ? len : space;
    memcpy(bleBuf_ + bleBufLen_, data, toCopy);
    bleBufLen_ += toCopy;
    bleBuf_[bleBufLen_] = '\0';

    // Check for ELM327 prompt ">" indicating complete response
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
}
#endif
