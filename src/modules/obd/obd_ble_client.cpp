/// @file obd_ble_client.cpp
/// OBD-owned BLE client implementation. Fully independent of ble_client.cpp.

#ifndef UNIT_TEST

#include "obd_ble_client.h"
#include "obd_runtime_module.h"
#include "obd_scan_policy.h"

#include <cstring>

namespace {

const NimBLEUUID kCxServiceUuid("FFF0");
const NimBLEUUID kCxNotifyUuid("FFF1");
const NimBLEUUID kCxWriteUuid("FFF2");
const NimBLEUUID kDeviceInfoServiceUuid("180A");
const NimBLEUUID kModelNumberUuid("2A24");

bool stringEquals(const std::string& lhs, const char* rhs) {
    return rhs != nullptr && lhs == rhs;
}

}  // namespace

ObdBleClient obdBleClient;

void ObdScanCallback::configure(ObdRuntimeModule* parent, int8_t minRssi) {
    parent_ = parent;
    minRssi_ = minRssi;
}

void ObdScanCallback::onResult(const NimBLEAdvertisedDevice* device) {
    if (!device || !parent_ || !device->haveName()) return;

    const std::string name = device->getName();
    if (!stringEquals(name, obd::DEVICE_NAME_CX)) {
        return;
    }

    const int rssi = device->getRSSI();
    if (rssi < minRssi_) {
        return;
    }

    parent_->onDeviceFound(name.c_str(), device->getAddress().toString().c_str(), rssi,
                            device->getAddress().getType());
    NimBLEDevice::getScan()->stop();
}

void ObdScanCallback::onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) {}

void ObdClientCallback::configure(ObdRuntimeModule* parent) {
    parent_ = parent;
}

void ObdClientCallback::onConnect(NimBLEClient* /*client*/) {}

void ObdClientCallback::onConnectFail(NimBLEClient* /*client*/, int reason) {
    if (parent_) {
        parent_->onBleDisconnect(reason);
    }
}

void ObdClientCallback::onDisconnect(NimBLEClient* /*client*/, int reason) {
    if (parent_) {
        parent_->onBleDisconnect(reason);
    }
}

void ObdBleClient::init(ObdRuntimeModule* parent) {
    if (pClient_ != nullptr) return;

    pClient_ = NimBLEDevice::createClient();
    clientCallback_.configure(parent);
    pClient_->setClientCallbacks(&clientCallback_);
    pClient_->setConnectionParams(12, 12, 0, 400);
    pClient_->setConnectTimeout(obd::CONNECT_TIMEOUT_MS / 1000);
}

bool ObdBleClient::startScan(int8_t minRssi) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) return false;

    scanCallback_.configure(&obdRuntimeModule, minRssi);
    pScan->setScanCallbacks(&scanCallback_);
    pScan->setActiveScan(true);
    pScan->setInterval(100);
    pScan->setWindow(75);
    pScan->setMaxResults(0);
    pScan->setDuplicateFilter(false);

    return pScan->start(obd::SCAN_DURATION_MS, false, false);
}

void ObdBleClient::stopScan() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) {
        pScan->stop();
    }
}

bool ObdBleClient::connect(const char* address, uint8_t addrType, uint32_t timeoutMs, bool preferCachedAttributes) {
    if (!pClient_ || !address || address[0] == '\0') return false;
    if (pClient_->isConnected()) return true;

    NimBLEAddress addr{std::string(address), addrType};
    pClient_->setConnectTimeout(timeoutMs / 1000);
    connectPending_ = true;
    Serial.printf("[OBD] connect addr=%s type=%u timeout=%lus cached=%d\n",
                  address, addrType, timeoutMs / 1000, preferCachedAttributes);
    const bool ok = pClient_->connect(addr, !preferCachedAttributes, true, true);
    if (!ok) {
        Serial.println("[OBD] connect() returned false");
        connectPending_ = false;
    }
    return ok;
}

void ObdBleClient::disconnect() {
    connectPending_ = false;
    if (!pClient_) {
        pTxChar_ = nullptr;
        pRxChar_ = nullptr;
        return;
    }

    if (!pClient_->isConnected()) {
        pClient_->cancelConnect();
    } else {
        pClient_->disconnect();
    }

    pTxChar_ = nullptr;
    pRxChar_ = nullptr;
}

bool ObdBleClient::isConnected() const {
    return pClient_ && pClient_->isConnected();
}

bool ObdBleClient::validateCxModel() const {
    if (!pClient_ || !pClient_->isConnected()) return false;

    NimBLERemoteService* deviceInfo = pClient_->getService(kDeviceInfoServiceUuid);
    if (!deviceInfo) {
        return true;
    }

    NimBLEAttValue modelValue = deviceInfo->getValue(kModelNumberUuid);
    if (modelValue.length() == 0) {
        return true;
    }

    const std::string model = static_cast<std::string>(modelValue);
    return model == obd::DEVICE_NAME_CX;
}

bool ObdBleClient::discoverServices() {
    if (!pClient_ || !pClient_->isConnected()) return false;

    connectPending_ = false;
    if (!validateCxModel()) {
        return false;
    }

    NimBLERemoteService* svc = pClient_->getService(kCxServiceUuid);
    if (!svc) return false;

    pTxChar_ = svc->getCharacteristic(kCxNotifyUuid);
    pRxChar_ = svc->getCharacteristic(kCxWriteUuid);
    if (!pTxChar_ || !pRxChar_) {
        pTxChar_ = nullptr;
        pRxChar_ = nullptr;
        return false;
    }

    if (!pTxChar_->canNotify() || !(pRxChar_->canWrite() || pRxChar_->canWriteNoResponse())) {
        pTxChar_ = nullptr;
        pRxChar_ = nullptr;
        return false;
    }

    return true;
}

bool ObdBleClient::writeCommand(const char* cmd) {
    if (!pRxChar_ || !pClient_ || !pClient_->isConnected() || !cmd) return false;
    return pRxChar_->writeValue(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd), true);
}

bool ObdBleClient::subscribeNotify(void (*callback)(const uint8_t* data, size_t len)) {
    if (!pTxChar_) return false;
    return pTxChar_->subscribe(
        true,
        [callback](NimBLERemoteCharacteristic* /*chr*/, uint8_t* data, size_t length, bool /*isNotify*/) {
            if (callback && data && length > 0) {
                callback(data, length);
            }
        },
        true);
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
