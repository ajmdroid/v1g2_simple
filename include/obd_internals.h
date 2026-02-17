// Shared helpers for OBD handler translation-unit split.
// Included by obd_handler.cpp, obd_protocol.cpp, obd_connection.cpp,
// and obd_persistence.cpp.

#pragma once

#include "obd_handler.h"

#include <NimBLEDevice.h>
#include <atomic>

// ---------------------------------------------------------------------------
// Static globals shared across OBD translation units
// ---------------------------------------------------------------------------

/// Singleton pointer (set in OBDHandler::begin).
extern OBDHandler* s_obdInstance;

/// Passkey injected by OBDSecurityCallbacks::onPassKeyEntry.
extern std::atomic<uint32_t> s_activePinCode;

/// Set by the NimBLE disconnect callback; consumed by runStateMachine().
extern std::atomic<bool> s_obdDisconnectPending;

/// SD backup path for remembered-device JSON.
extern const char* const OBD_SD_BACKUP_PATH;

// ---------------------------------------------------------------------------
// Free-function helpers
// ---------------------------------------------------------------------------

/// Returns true when all six octets of a NimBLE address are zero.
inline bool isAllZeroAddress(const NimBLEAddress& address) {
    const uint8_t* raw = address.getVal();
    if (!raw) {
        return true;
    }
    for (size_t i = 0; i < 6; i++) {
        if (raw[i] != 0) {
            return false;
        }
    }
    return true;
}

/// Returns true for empty strings, NimBLE null addresses, and all-zero addresses.
inline bool isNullAddress(const String& address) {
    if (address.length() == 0) return true;
    const NimBLEAddress parsed(std::string(address.c_str()), BLE_ADDR_PUBLIC);
    return parsed.isNull() || isAllZeroAddress(parsed);
}

// ---------------------------------------------------------------------------
// ObdLock – RAII SemaphoreTake/Give with optional timeout
// ---------------------------------------------------------------------------

class ObdLock {
public:
    explicit ObdLock(SemaphoreHandle_t mutex, TickType_t timeout = 0)
        : mutex_(mutex), locked_(false) {
        if (mutex_) {
            locked_ = (xSemaphoreTake(mutex_, timeout) == pdTRUE);
        }
    }

    ~ObdLock() {
        if (mutex_ && locked_) {
            xSemaphoreGive(mutex_);
        }
    }

    bool ok() const { return locked_; }

private:
    SemaphoreHandle_t mutex_;
    bool locked_;
};

// ---------------------------------------------------------------------------
// OBDSecurityCallbacks – NimBLE client callbacks for pairing/disconnect
// ---------------------------------------------------------------------------

class OBDSecurityCallbacks : public NimBLEClientCallbacks {
public:
    void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
        NimBLEDevice::injectPassKey(connInfo, s_activePinCode.load(std::memory_order_relaxed));
    }

    void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t passkey) override {
        (void)passkey;
        NimBLEDevice::injectConfirmPasskey(connInfo, true);
    }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        Serial.printf("[OBD] BLE disconnected (reason=%d)\n", reason);
        s_obdDisconnectPending.store(true, std::memory_order_release);
    }

    bool onConnParamsUpdateRequest(NimBLEClient* pClient,
                                   const ble_gap_upd_params* params) override {
        if (!params) return true;

        Serial.printf("[OBD] Accepting conn param update request: int=%u-%u lat=%u to=%u\n",
                      (unsigned)params->itvl_min, (unsigned)params->itvl_max,
                      (unsigned)params->latency, (unsigned)params->supervision_timeout);
        return true;
    }
};

extern OBDSecurityCallbacks obdSecurityCallbacks;

// ---------------------------------------------------------------------------
// parseDevicesJson – shared between persistence load and SD backup restore
// ---------------------------------------------------------------------------
size_t parseDevicesJson(const String& blob,
                        std::vector<OBDRememberedDevice>& out,
                        size_t maxDevices);
