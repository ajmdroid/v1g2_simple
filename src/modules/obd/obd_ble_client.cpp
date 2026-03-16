/// @file obd_ble_client.cpp
/// OBD-owned BLE client implementation. Fully independent of ble_client.cpp.

#ifndef UNIT_TEST

#include "obd_ble_client.h"
#include "obd_runtime_module.h"
#include "obd_scan_policy.h"

#include <cstring>

// NimBLE's BLE controller resolves ALL scanned addresses against stored IRKs
// (resolving list populated from V1 bonds). A 24-bit AES hash false-positive
// can replace the OBDLink CX address with a zeroed identity address. Disable
// controller-level address resolution during OBD scans and re-enable afterward.
// ble_hs_pvcy_set_resolve_enabled is compiled when BLE_HOST_BASED_PRIVACY==0
// (ESP32-S3 with controller-based privacy).
extern "C" {
    int ble_hs_pvcy_set_resolve_enabled(int enable);
}

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

    const NimBLEAddress& addr = device->getAddress();
    if (addr.isNull()) {
        return;  // Identity resolution produced a null address — skip
    }

    // NimBLE may return identity types (PUBLIC_ID=2, RANDOM_ID=3) when it
    // resolves an RPA via a stored IRK.  The connect API only accepts
    // PUBLIC(0) or RANDOM(1), so strip the identity bit.
    uint8_t addrType = addr.getType();
    if (addrType >= 2) {
        addrType = addrType & 0x01;  // PUBLIC_ID→PUBLIC, RANDOM_ID→RANDOM
    }

    parent_->onDeviceFound(name.c_str(), addr.toString().c_str(), rssi, addrType);
    NimBLEDevice::getScan()->stop();
}

void ObdScanCallback::onScanEnd(const NimBLEScanResults& /*results*/, int /*reason*/) {
    ble_hs_pvcy_set_resolve_enabled(1);
}

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
    // min=12 (15ms), max=40 (50ms): give the BLE 4.2 OBDLink CX room to
    // negotiate a comfortable interval.  Fixed min==max==12 caused connection
    // parameter update rejections when the CX requested a wider interval.
    pClient_->setConnectionParams(12, 40, 0, 400);
    pClient_->setConnectTimeout(obd::CONNECT_TIMEOUT_MS);
}

bool ObdBleClient::startScan(int8_t minRssi) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan->isScanning()) return false;

    ble_hs_pvcy_set_resolve_enabled(0);

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
    ble_hs_pvcy_set_resolve_enabled(1);
}

bool ObdBleClient::connect(const char* address, uint8_t addrType, uint32_t timeoutMs, bool preferCachedAttributes) {
    if (!pClient_ || !address || address[0] == '\0') return false;
    if (pClient_->isConnected()) return true;

    NimBLEAddress addr{std::string(address), addrType};
    pClient_->setConnectTimeout(timeoutMs);
    connectPending_ = true;
    Serial.printf("[OBD] connect addr=%s type=%u timeout=%lums cached=%d\n",
                  address, addrType, timeoutMs, preferCachedAttributes);
    // exchangeMTU=false: the global MTU is 517 (set for V1 BLE 5.x) which can
    // race with GATT discovery on the BLE 4.2 OBDLink CX.  OBD responses are
    // ~10 bytes so the default 23-byte ATT MTU is sufficient.
    const bool ok = pClient_->connect(addr, !preferCachedAttributes, true, false);
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
    if (!pClient_ || !pClient_->isConnected()) {
        Serial.println("[OBD] discoverServices: not connected");
        return false;
    }

    connectPending_ = false;
    // Skip validateCxModel(): the CX was already identified by BLE
    // advertisement name during scan.  Removing the 180A service
    // discovery + 2A24 characteristic read eliminates 1-2 GATT round
    // trips, reducing the setup window where the CX can disconnect.

    NimBLERemoteService* svc = pClient_->getService(kCxServiceUuid);
    if (!svc) {
        Serial.println("[OBD] discoverServices: FFF0 service not found");
        return false;
    }

    pTxChar_ = svc->getCharacteristic(kCxNotifyUuid);
    pRxChar_ = svc->getCharacteristic(kCxWriteUuid);
    if (!pTxChar_ || !pRxChar_) {
        Serial.printf("[OBD] discoverServices: char missing tx=%d rx=%d\n",
                      pTxChar_ != nullptr, pRxChar_ != nullptr);
        pTxChar_ = nullptr;
        pRxChar_ = nullptr;
        return false;
    }

    if (!pTxChar_->canNotify() || !(pRxChar_->canWrite() || pRxChar_->canWriteNoResponse())) {
        Serial.printf("[OBD] discoverServices: capability mismatch notify=%d write=%d writeNR=%d\n",
                      pTxChar_->canNotify(), pRxChar_->canWrite(), pRxChar_->canWriteNoResponse());
        pTxChar_ = nullptr;
        pRxChar_ = nullptr;
        return false;
    }

    Serial.println("[OBD] discoverServices: OK");
    return true;
}

bool ObdBleClient::writeCommand(const char* cmd) {
    if (!pRxChar_ || !pClient_ || !pClient_->isConnected() || !cmd) return false;
    return pRxChar_->writeValue(reinterpret_cast<const uint8_t*>(cmd), strlen(cmd), true);
}

bool ObdBleClient::subscribeNotify(void (*callback)(const uint8_t* data, size_t len)) {
    if (!pTxChar_) return false;
    if (!pClient_ || !pClient_->isConnected()) {
        Serial.println("[OBD] subscribeNotify: connection lost before subscribe");
        return false;
    }
    // response=false: use ATT Write Command (no response) for the CCCD
    // descriptor.  The DA14531 BLE 4.2 modem on the OBDLink CX fails
    // write-with-response on the 0x2902 descriptor, causing subscribe to
    // return false and the connection to drop.
    const bool ok = pTxChar_->subscribe(
        true,
        [callback](NimBLERemoteCharacteristic* /*chr*/, uint8_t* data, size_t length, bool /*isNotify*/) {
            if (callback && data && length > 0) {
                callback(data, length);
            }
        },
        false);
    if (!ok) {
        Serial.println("[OBD] subscribeNotify: subscribe failed");
    }
    return ok;
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
