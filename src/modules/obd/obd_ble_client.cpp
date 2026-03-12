/// @file obd_ble_client.cpp
/// OBD-owned BLE client implementation. Fully independent of ble_client.cpp.

#ifndef UNIT_TEST

#include "obd_ble_client.h"
#include "obd_runtime_module.h"
#include "obd_scan_policy.h"

#include <cstring>

ObdBleClient obdBleClient;

// ── ObdScanCallback ───────────────────────────────────────────────

void ObdScanCallback::configure(ObdRuntimeModule* parent, int8_t minRssi) {
    parent_ = parent;
    minRssi_ = minRssi;
}

void ObdScanCallback::onResult(const NimBLEAdvertisedDevice* device) {
    if (!device || !parent_) return;

    // Must have a name
    if (!device->haveName()) return;

    // Match name prefix "OBDLink"
    const std::string name = device->getName();
    if (name.size() < 7 || strncmp(name.c_str(), "OBDLink", 7) != 0) return;

    int rssi = device->getRSSI();

    // Signal the runtime module (ISR-safe: sets flag + copies address)
    parent_->onDeviceFound(
        name.c_str(),
        device->getAddress().toString().c_str(),
        rssi);

    // Stop scan early — we found our device
    NimBLEDevice::getScan()->stop();
}

void ObdScanCallback::onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) {
    // No action needed — runtime module handles scan timeout via elapsed time
}

// ── ObdClientCallback ─────────────────────────────────────────────

void ObdClientCallback::configure(ObdRuntimeModule* parent) {
    parent_ = parent;
}

void ObdClientCallback::onDisconnect(NimBLEClient* /*client*/, int /*reason*/) {
    if (parent_) {
        parent_->onBleDisconnect();
    }
}

// ── ObdBleClient ──────────────────────────────────────────────────

void ObdBleClient::init(ObdRuntimeModule* parent) {
    if (pClient_ != nullptr) return;  // Already initialized

    pClient_ = NimBLEDevice::createClient();
    clientCallback_.configure(parent);
    pClient_->setClientCallbacks(&clientCallback_);
    pClient_->setConnectionParams(12, 12, 0, 400);  // 15ms interval, 400 supervision TO
    pClient_->setConnectTimeout(obd::CONNECT_TIMEOUT_MS / 1000);  // seconds
}

bool ObdBleClient::startScan(int8_t minRssi) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) return false;  // V1 or another scan active

    scanCallback_.configure(&obdRuntimeModule, minRssi);
    pScan->setScanCallbacks(&scanCallback_);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(75);
    pScan->setMaxResults(0);
    pScan->setDuplicateFilter(true);

    return pScan->start(obd::SCAN_DURATION_MS / 1000, false, false);
}

void ObdBleClient::stopScan() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
    }
}

bool ObdBleClient::connect(const char* address, uint32_t timeoutMs) {
    if (!pClient_ || !address || address[0] == '\0') return false;
    if (pClient_->isConnected()) return true;

    NimBLEAddress addr{std::string(address), BLE_ADDR_PUBLIC};
    pClient_->setConnectTimeout(timeoutMs / 1000);
    return pClient_->connect(addr);
}

void ObdBleClient::disconnect() {
    if (pClient_ && pClient_->isConnected()) {
        pClient_->disconnect();
    }
    pTxChar_ = nullptr;
    pRxChar_ = nullptr;
}

bool ObdBleClient::isConnected() const {
    return pClient_ && pClient_->isConnected();
}

bool ObdBleClient::discoverServices() {
    if (!pClient_ || !pClient_->isConnected()) return false;

    // OBDLink CX uses SPP-over-GATT. Common service UUIDs: FFF0, FFE0
    // Try to discover all services and find TX/RX characteristics
    const auto& services = pClient_->getServices(true);
    if (services.empty()) return false;

    pTxChar_ = nullptr;
    pRxChar_ = nullptr;

    for (auto* svc : services) {
        const auto& chars = svc->getCharacteristics(true);
        for (auto* chr : chars) {
            if (chr->canNotify()) {
                pTxChar_ = chr;
            }
            if (chr->canWrite() || chr->canWriteNoResponse()) {
                pRxChar_ = chr;
            }
        }
        if (pTxChar_ && pRxChar_) break;
    }

    return (pTxChar_ != nullptr && pRxChar_ != nullptr);
}

bool ObdBleClient::writeCommand(const char* cmd) {
    if (!pRxChar_ || !pClient_ || !pClient_->isConnected()) return false;
    return pRxChar_->writeValue(reinterpret_cast<const uint8_t*>(cmd),
                                strlen(cmd), false);
}

bool ObdBleClient::subscribeNotify(void (*callback)(const uint8_t* data, size_t len)) {
    if (!pTxChar_) return false;
    return pTxChar_->subscribe(true, [callback](NimBLERemoteCharacteristic* /*chr*/,
                                                 uint8_t* data, size_t length, bool /*isNotify*/) {
        if (callback && data && length > 0) {
            callback(data, length);
        }
    });
}

int8_t ObdBleClient::getRssi(uint32_t nowMs) {
    if (!pClient_ || !pClient_->isConnected()) return 0;

    if (nowMs - lastRssiQueryMs_ >= RSSI_QUERY_INTERVAL_MS) {
        lastRssiQueryMs_ = nowMs;
        cachedRssi_ = static_cast<int8_t>(pClient_->getRssi());
    }
    return cachedRssi_;
}

#endif  // UNIT_TEST
