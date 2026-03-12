#include "obd_runtime_module.h"
#include "obd_elm327_parser.h"
#include "obd_scan_policy.h"

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
            // In real BLE integration, this initiates pOBDClient->connect().
            // For testability, the state machine transitions are driven by
            // external callbacks. In the native test environment, we simulate
            // connection success/failure via test helpers.
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::CONNECT_TIMEOUT_MS) {
                connectAttempts_++;
                if (connectAttempts_ >= obd::MAX_DIRECT_CONNECT_FAILURES) {
                    // Clear saved address — user must re-scan from web UI
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
            // Service/characteristic discovery in progress.
            // Timeout handled same as CONNECTING.
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::CONNECT_TIMEOUT_MS) {
                connectAttempts_++;
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
            }
            break;
        }

        case ObdConnectionState::AT_INIT: {
            // AT command init sequence in progress.
            uint32_t elapsed = nowMs - stateEnteredMs_;
            if (elapsed >= obd::AT_INIT_RESPONSE_TIMEOUT_MS * obd::AT_INIT_COMMAND_COUNT) {
                consecutiveErrors_++;
                transitionTo(ObdConnectionState::DISCONNECTED, nowMs);
            }
            break;
        }

        case ObdConnectionState::POLLING: {
            // Steady-state speed polling.
            // Check for error backoff threshold.
            if (consecutiveErrors_ >= obd::MAX_CONSECUTIVE_ERRORS) {
                transitionTo(ObdConnectionState::ERROR_BACKOFF, nowMs);
                break;
            }

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

#ifdef UNIT_TEST
void ObdRuntimeModule::injectSpeedForTest(float speedMph, uint32_t timestampMs) {
    speedMph_ = speedMph;
    speedSampleTsMs_ = timestampMs;
    speedValid_ = true;
    consecutiveErrors_ = 0;
}
#endif
