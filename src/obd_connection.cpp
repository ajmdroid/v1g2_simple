// OBD-II BLE connection management — connectToDevice, service discovery,
// adapter initialization, address resolution, and FreeRTOS task lifecycle.
// Extracted from obd_handler.cpp for maintainability.

#include "obd_internals.h"

#include <NimBLEDevice.h>

// ---------------------------------------------------------------------------
// BLE connection
// ---------------------------------------------------------------------------

bool OBDHandler::connectToDevice(bool skipPreScan) {
    lastConnectFailureNoAdvertising = false;

    // BLE controllers generally cannot sustain scan+connect reliably.
    // Guard here as well for auto-connect and retry paths.
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (pScan && pScan->isScanning()) {
        Serial.println("[OBD] Stopping active scan before connect");
        pScan->stop();
        pScan->clearResults();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    if (pOBDClient && pOBDClient->isConnected()) {
        pOBDClient->disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!pOBDClient) {
        pOBDClient = NimBLEDevice::createClient();
        if (!pOBDClient) {
            return false;
        }
        pOBDClient->setClientCallbacks(&obdSecurityCallbacks);
        // Connection parameters: 24-48 (30-60 ms interval) gives the OBDLink CX
        // room to negotiate.  The CX is a power-saving peripheral and rejects or
        // immediately disconnects when forced to a fixed 15 ms interval.
        // Latency 0, supervision timeout 400 (4 s) matches V1 client config.
        pOBDClient->setConnectionParams(24, 48, 0, 400);
        // NimBLE timeout is configured on the client, not via connect() args.
        pOBDClient->setConnectTimeout(10000);
    }

    if (!targetIsObdLink) {
        Serial.printf("[OBD] Connect aborted: unsupported adapter '%s' (CX only)\n",
                      targetDeviceName.c_str());
        return false;
    }

    // Quick pre-scan: is any OBDLink CX advertising?
    // If not, the adapter is likely powered off — bail immediately
    // to avoid dozens of expensive BLE connection attempts.
    // Skipped when the caller already verified via scan-gate.
    if (!skipPreScan) {
        NimBLEScan* pPreScan = NimBLEDevice::getScan();
        if (pPreScan) {
            pPreScan->setDuplicateFilter(true);
            pPreScan->setMaxResults(20);
            pPreScan->setActiveScan(true);
            NimBLEScanResults preResults = pPreScan->getResults(2000, false);

            bool cxAdvertising = false;
            for (int i = 0; i < preResults.getCount(); i++) {
                const NimBLEAdvertisedDevice* dev = preResults.getDevice(i);
                if (dev && isObdLinkName(dev->getName())) {
                    cxAdvertising = true;
                    break;
                }
            }

            pPreScan->setMaxResults(0);
            pPreScan->setActiveScan(true);
            pPreScan->clearResults();

            if (!cxAdvertising) {
                lastConnectFailureNoAdvertising = true;
                Serial.println("[OBD] No OBDLink CX advertising - adapter may be off");
                return false;
            }
        }
    }

    if (targetAddress.isNull() || isAllZeroAddress(targetAddress)) {
        const int bondCount = NimBLEDevice::getNumBonds();
        if (bondCount > 0) {
            Serial.printf("[OBD] Target address unknown - trying %d bonded peer(s) first\n", bondCount);
            s_activePinCode.store(0, std::memory_order_relaxed);
            NimBLEDevice::setSecurityAuth(false, false, false);
            NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

            for (int bi = 0; bi < bondCount; bi++) {
                NimBLEAddress bondedAddr = NimBLEDevice::getBondedAddress(bi);
                if (bondedAddr.isNull() || isAllZeroAddress(bondedAddr)) {
                    continue;
                }

                const std::string bondedStr = bondedAddr.toString();
                const uint8_t bondedPrimaryType = bondedAddr.getType();
                uint8_t bondedTypes[4] = {0};
                size_t bondedTypeCount = 0;
                auto pushBondedType = [&](uint8_t t) {
                    for (size_t k = 0; k < bondedTypeCount; k++) {
                        if (bondedTypes[k] == t) return;
                    }
                    if (bondedTypeCount < (sizeof(bondedTypes) / sizeof(bondedTypes[0]))) {
                        bondedTypes[bondedTypeCount++] = t;
                    }
                };
                pushBondedType(bondedPrimaryType);
                pushBondedType(BLE_ADDR_PUBLIC);
                pushBondedType(BLE_ADDR_RANDOM);
                pushBondedType(BLE_ADDR_PUBLIC_ID);
                pushBondedType(BLE_ADDR_RANDOM_ID);

                for (size_t bj = 0; bj < bondedTypeCount; bj++) {
                    NimBLEAddress candidate(bondedStr, bondedTypes[bj]);
                    Serial.printf("[OBD] Connect try (bonded-list#%d, addr=%s, addrType=%u)\n",
                                  bi,
                                  bondedStr.c_str(),
                                  (unsigned)bondedTypes[bj]);

                    if (!pOBDClient->connect(candidate, false, false, true)) {
                        Serial.printf("[OBD] Connect attempt failed (bonded-list#%d, addrType=%u, err=%d)\n",
                                      bi,
                                      (unsigned)bondedTypes[bj],
                                      pOBDClient->getLastError());
                        continue;
                    }

                    // Allow time for security handshake + conn-param negotiation.
                    vTaskDelay(pdMS_TO_TICKS(500));
                    if (pOBDClient->isConnected()) {
                        if (!connectedPeerLooksLikeObd()) {
                            Serial.printf("[OBD] Bonded peer rejected (missing OBD UART): %s\n",
                                          bondedStr.c_str());
                            pOBDClient->disconnect();
                            vTaskDelay(pdMS_TO_TICKS(100));
                            continue;
                        }
                        targetAddress = candidate;
                        Serial.printf("[OBD] Bonded reconnect selected address: %s\n",
                                      targetAddress.toString().c_str());
                        return true;
                    }

                    pOBDClient->disconnect();
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        }

        Serial.printf("[OBD] Target address is NULL for '%s' - resolving...\n",
                      targetDeviceName.c_str());
        if (!resolveTargetAddress()) {
            Serial.println("[OBD] Address resolve failed - cannot connect");
            return false;
        }
    }

    const std::string targetAddrStr = targetAddress.toString();
    const uint8_t primaryAddrType = targetAddress.getType();

    struct SecurityProfile {
        bool bonding;
        bool mitm;
        bool sc;
        uint8_t ioCap;
        const char* label;
    };

    // Some adapters (or bonded identities) may resolve with ID address types.
    // Try a small set of valid NimBLE peer types in deterministic order.
    uint8_t addrTypes[4] = {0};
    size_t addrTypeCount = 0;
    auto pushAddrType = [&](uint8_t t) {
        for (size_t k = 0; k < addrTypeCount; k++) {
            if (addrTypes[k] == t) return;
        }
        if (addrTypeCount < (sizeof(addrTypes) / sizeof(addrTypes[0]))) {
            addrTypes[addrTypeCount++] = t;
        }
    };

    pushAddrType(primaryAddrType);
    pushAddrType(BLE_ADDR_PUBLIC);
    pushAddrType(BLE_ADDR_RANDOM);
    pushAddrType(BLE_ADDR_PUBLIC_ID);
    pushAddrType(BLE_ADDR_RANDOM_ID);

    auto tryProfile = [&](const SecurityProfile& profile, bool usingPin) {
        NimBLEDevice::setSecurityAuth(profile.bonding, profile.mitm, profile.sc);
        NimBLEDevice::setSecurityIOCap(profile.ioCap);

        if (connectViaAdvertisedDevice(profile.label, usingPin)) {
            return true;
        }

        for (size_t j = 0; j < addrTypeCount; j++) {
            const uint8_t addrType = addrTypes[j];
            NimBLEAddress candidate(targetAddrStr, addrType);

            if (usingPin) {
                    Serial.printf("[OBD] Connect try (pin=%u, %s, addrType=%u)\n",
                              (unsigned)s_activePinCode.load(std::memory_order_relaxed),
                              profile.label,
                              (unsigned)addrType);
            } else {
                Serial.printf("[OBD] Connect try (%s, addrType=%u)\n",
                              profile.label,
                              (unsigned)addrType);
            }

            // NimBLE signature: connect(address, deleteAttributes, asyncConnect, exchangeMTU)
            if (!pOBDClient->connect(candidate, false, false, true)) {
                if (usingPin) {
                    Serial.printf("[OBD] Connect attempt failed (pin=%u, %s, addrType=%u, err=%d)\n",
                                  (unsigned)s_activePinCode.load(std::memory_order_relaxed),
                                  profile.label,
                                  (unsigned)addrType,
                                  pOBDClient->getLastError());
                } else {
                    Serial.printf("[OBD] Connect attempt failed (%s, addrType=%u, err=%d)\n",
                                  profile.label,
                                  (unsigned)addrType,
                                  pOBDClient->getLastError());
                }
                continue;
            }

            // Allow time for security handshake + conn-param negotiation.
            vTaskDelay(pdMS_TO_TICKS(500));
            if (pOBDClient->isConnected()) {
                if (!connectedPeerLooksLikeObd()) {
                    Serial.printf("[OBD] Connected peer rejected (%s, addrType=%u): missing OBD UART\n",
                                  profile.label,
                                  (unsigned)addrType);
                    pOBDClient->disconnect();
                    vTaskDelay(pdMS_TO_TICKS(100));
                    continue;
                }
                targetAddress = candidate;
                return true;
            }

            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return false;
    };

    const bool userPinProvided = targetPin.length() > 0;

    // Phase 1: bonded reconnect only (no pairing).
    bool isBonded = NimBLEDevice::isBonded(targetAddress);
    if (!isBonded) {
        for (size_t j = 0; j < addrTypeCount; j++) {
            if (NimBLEDevice::isBonded(NimBLEAddress(targetAddrStr, addrTypes[j]))) {
                isBonded = true;
                break;
            }
        }
    }

    if (isBonded) {
        Serial.println("[OBD] Connect phase: bonded reconnect");
        s_activePinCode.store(0, std::memory_order_relaxed);
        const SecurityProfile bondedProfile{false, false, false, BLE_HS_IO_NO_INPUT_OUTPUT, "bonded-reconnect"};
        if (tryProfile(bondedProfile, false)) {
            return true;
        }

        bool deletedBond = false;
        if (NimBLEDevice::isBonded(targetAddress)) {
            NimBLEDevice::deleteBond(targetAddress);
            deletedBond = true;
        }
        for (size_t j = 0; j < addrTypeCount; j++) {
            NimBLEAddress candidate(targetAddrStr, addrTypes[j]);
            if (NimBLEDevice::isBonded(candidate)) {
                NimBLEDevice::deleteBond(candidate);
                deletedBond = true;
            }
        }
        if (deletedBond) {
            Serial.println("[OBD] Bonded reconnect failed - cleared stale bond, moving to first-pair phase");
            vTaskDelay(pdMS_TO_TICKS(80));
        } else {
            Serial.println("[OBD] Bonded reconnect failed - moving to first-pair phase");
        }
    } else {
        Serial.println("[OBD] Connect phase: first pair/bond");
    }

    // Phase 2: first pairing + bond establishment.
    uint32_t pinCandidates[2] = {0};
    size_t pinCount = 0;
    if (userPinProvided) {
        pinCandidates[pinCount++] = normalizePin(targetPin, targetIsObdLink);
    } else {
        // CX typically pairs with 123456 on first bond; keep no-pin fallback.
        pinCandidates[pinCount++] = 123456u;
        pinCandidates[pinCount++] = 0u;
    }

    for (size_t pinIndex = 0; pinIndex < pinCount; pinIndex++) {
        s_activePinCode.store(pinCandidates[pinIndex], std::memory_order_relaxed);
        const bool usingPin = s_activePinCode.load(std::memory_order_relaxed) != 0;

        SecurityProfile pairProfiles[2];
        size_t profileCount = 0;
        if (usingPin) {
            pairProfiles[profileCount++] = SecurityProfile{true, true, true, BLE_HS_IO_DISPLAY_YESNO, "sc-pin"};
            pairProfiles[profileCount++] = SecurityProfile{true, true, false, BLE_HS_IO_KEYBOARD_ONLY, "legacy-pin"};
        } else {
            pairProfiles[profileCount++] = SecurityProfile{true, false, true, BLE_HS_IO_NO_INPUT_OUTPUT, "sc-no-pin"};
            pairProfiles[profileCount++] = SecurityProfile{true, false, false, BLE_HS_IO_NO_INPUT_OUTPUT, "legacy-no-pin"};
        }

        for (size_t i = 0; i < profileCount; i++) {
            if (tryProfile(pairProfiles[i], usingPin)) {
                return true;
            }
        }

        if (userPinProvided) {
            break;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// GATT service validation and discovery
// ---------------------------------------------------------------------------

bool OBDHandler::connectedPeerLooksLikeObd() {
    if (!pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }

    // GATT table may not be populated yet after a bonded reconnect.
    // discoverAttributes() is idempotent and returns cached results
    // if already discovered.  Skip the call if services are already
    // cached to avoid hammering the freshly-connected CX.
    if (!pOBDClient->getService(NUS_SERVICE_UUID) &&
        !pOBDClient->getService("FFF0") &&
        !pOBDClient->getService("FFE0")) {
        pOBDClient->discoverAttributes();
    }

    // Check all service UUIDs that discoverServices() accepts.
    NimBLERemoteService* service = pOBDClient->getService(NUS_SERVICE_UUID);
    if (!service) {
        service = pOBDClient->getService("FFF0");
    }
    if (!service) {
        service = pOBDClient->getService("FFE0");
    }
    if (!service) {
        return false;
    }

    // Check for TX/RX characteristics using the same fallback order
    // as discoverServices().
    NimBLERemoteCharacteristic* tx = service->getCharacteristic(NUS_TX_CHAR_UUID);
    if (!tx) {
        tx = service->getCharacteristic("FFF1");
    }
    if (!tx) {
        tx = service->getCharacteristic("FFE1");
    }

    NimBLERemoteCharacteristic* rx = service->getCharacteristic(NUS_RX_CHAR_UUID);
    if (!rx) {
        rx = service->getCharacteristic("FFF2");
    }
    if (!rx) {
        rx = service->getCharacteristic("FFE1");
    }

    return tx != nullptr && rx != nullptr;
}

bool OBDHandler::discoverServices() {
    if (!pOBDClient || !pOBDClient->isConnected()) {
        return false;
    }

    // Allow the link to stabilize after connect/pairing before hammering GATT.
    // The CX needs time to finish conn-param negotiation.
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!pOBDClient->isConnected()) {
        Serial.println("[OBD] Connection lost before service discovery");
        return false;
    }

    pOBDClient->discoverAttributes();

    pNUSService = pOBDClient->getService(NUS_SERVICE_UUID);
    if (!pNUSService) {
        pNUSService = pOBDClient->getService("FFF0");
    }
    if (!pNUSService) {
        pNUSService = pOBDClient->getService("FFE0");
    }
    if (!pNUSService) {
        return false;
    }

    pTXChar = pNUSService->getCharacteristic(NUS_TX_CHAR_UUID);
    pRXChar = pNUSService->getCharacteristic(NUS_RX_CHAR_UUID);

    if (!pTXChar || !pRXChar) {
        // Alternative UART characteristics
        pTXChar = pNUSService->getCharacteristic("FFF1");
        if (!pTXChar) {
            pTXChar = pNUSService->getCharacteristic("FFE1");
        }
        pRXChar = pNUSService->getCharacteristic("FFF2");
        if (!pRXChar) {
            pRXChar = pNUSService->getCharacteristic("FFE1");
        }
    }

    if (!pTXChar || !pRXChar) {
        return false;
    }

    if (!pTXChar->canNotify()) {
        return false;
    }

    return pTXChar->subscribe(true, notificationCallback);
}

// ---------------------------------------------------------------------------
// Adapter initialization (ELM327 AT command sequence)
// ---------------------------------------------------------------------------

bool OBDHandler::initializeAdapter() {
    String response;

    // Let the UART link stabilize after service discovery + notification
    // subscribe.  Sending AT commands too quickly can lose the first command
    // or cause the CX to drop the connection.
    vTaskDelay(pdMS_TO_TICKS(300));

    if (!pOBDClient || !pOBDClient->isConnected()) {
        Serial.println("[OBD] Connection lost before adapter init");
        return false;
    }

    auto probeMode01 = [&](const char* label, uint32_t timeoutMs, int attempts) {
        for (int i = 0; i < attempts; i++) {
            if (sendATCommand("0100", response, timeoutMs)) {
                String normalized(response);
                normalized.toUpperCase();
                normalized.replace(" ", "");
                if (normalized.indexOf("4100") >= 0) {
                    Serial.printf("[OBD] Vehicle bus probe OK (%s): %s\n", label, response.c_str());
                    return true;
                }
                Serial.printf("[OBD] Vehicle bus probe inconclusive (%s): %s\n", label, response.c_str());
            }
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        Serial.printf("[OBD] Vehicle bus probe failed (%s)\n", label);
        return false;
    };

    if (!sendATCommand("ATZ", response, 3000)) return false;
    if (!sendATCommand("ATE0", response)) return false;
    if (!sendATCommand("ATL0", response)) return false;
    if (!sendATCommand("ATS0", response)) return false;
    if (!sendATCommand("ATH0", response)) return false;
    if (!sendATCommand("ATSP0", response, 5000)) return false;
    // Adaptive timing helps on slower ECUs and during ignition transitions.
    (void)sendATCommand("ATAT1", response, 500);

    // On some vehicles the first Mode 01 query after protocol select can take
    // several seconds while the adapter locks bus timing.
    if (probeMode01("auto", 5000, 3)) {
        return true;
    }

    // Fallbacks for CAN vehicles when auto-detect fails to lock quickly.
    // Always probe after selecting a protocol, even if the set command response
    // is odd/inconclusive on this adapter firmware.
    (void)sendATCommand("ATSP6", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can11-500", 5000, 2)) {
        return true;
    }
    (void)sendATCommand("ATSP7", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can29-500", 5000, 2)) {
        return true;
    }
    (void)sendATCommand("ATSP8", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can11-250", 5000, 2)) {
        return true;
    }
    (void)sendATCommand("ATSP9", response, 2000);
    vTaskDelay(pdMS_TO_TICKS(120));
    if (probeMode01("can29-250", 5000, 2)) {
        return true;
    }

    Serial.println("[OBD] Adapter init failed: no ECU response on supported CAN protocols");
    return false;
}

// ---------------------------------------------------------------------------
// Address resolution and advertised-device connect
// ---------------------------------------------------------------------------

bool OBDHandler::resolveTargetAddress() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan) {
        return false;
    }

    if (pScan->isScanning()) {
        pScan->stop();
        pScan->clearResults();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    auto tryResolvePass = [&](bool activeScan, uint32_t durationMs, const char* label) {
        pScan->setActiveScan(activeScan);
        NimBLEScanResults results = pScan->getResults(durationMs, false);

        const NimBLEAdvertisedDevice* bestNamedDevice = nullptr;
        int bestNamedRssi = -127;
        size_t nonNullCount = 0;
        bool sawNamedCx = false;
        bool sawNamedCxNullAddr = false;

        const int count = results.getCount();
        for (int i = 0; i < count; i++) {
            const NimBLEAdvertisedDevice* dev = results.getDevice(i);
            if (!dev) {
                continue;
            }
            const NimBLEAddress& advAddr = dev->getAddress();
            const bool addrIsZero = advAddr.isNull() || isAllZeroAddress(advAddr);
            const bool namedCx = isObdLinkName(dev->getName());
            if (namedCx) {
                sawNamedCx = true;
                if (addrIsZero) {
                    sawNamedCxNullAddr = true;
                }
            }

            if (addrIsZero) {
                continue;
            }

            nonNullCount++;
            const int rssi = dev->getRSSI();
            if (namedCx && (!bestNamedDevice || rssi > bestNamedRssi)) {
                bestNamedDevice = dev;
                bestNamedRssi = rssi;
            }
        }

        if (bestNamedDevice) {
            targetAddress = bestNamedDevice->getAddress();
            if (targetDeviceName.length() == 0) {
                targetDeviceName = String(bestNamedDevice->getName().c_str());
            }
            Serial.printf("[OBD] Resolved CX address via %s scan: %s RSSI:%d\n",
                          label,
                          targetAddress.toString().c_str(),
                          bestNamedRssi);
            return true;
        }

        if (sawNamedCxNullAddr && nonNullCount > 0) {
            Serial.printf("[OBD] Resolve pass (%s) saw CX name on null-address record; no confident non-null CX match\n",
                          label);
        } else {
            Serial.printf("[OBD] Resolve pass (%s) found no usable CX address\n", label);
        }
        return false;
    };

    pScan->clearResults();
    pScan->setDuplicateFilter(true);
    pScan->setMaxResults(20);

    bool resolved = tryResolvePass(false, 1800, "passive");
    if (!resolved) {
        resolved = tryResolvePass(true, 2200, "active");
    }

    // Restore normal scan configuration for V1/OBD callback-driven scans.
    pScan->setMaxResults(0);
    pScan->setActiveScan(true);
    pScan->clearResults();
    return resolved;
}

bool OBDHandler::connectViaAdvertisedDevice(const char* profileLabel, bool usingPin) {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    if (!pScan || !pOBDClient) {
        return false;
    }

    if (pScan->isScanning()) {
        pScan->stop();
        pScan->clearResults();
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    auto restoreScanConfig = [&]() {
        pScan->setMaxResults(0);
        pScan->setActiveScan(true);
        pScan->clearResults();
    };

    const String currentTargetAddr = String(targetAddress.toString().c_str());
    pScan->setDuplicateFilter(true);
    pScan->setMaxResults(20);
    pScan->setActiveScan(true);

    NimBLEScanResults results = pScan->getResults(1800, false);

    const NimBLEAdvertisedDevice* preferred = nullptr;
    int preferredRssi = -127;
    const NimBLEAdvertisedDevice* fallback = nullptr;
    int fallbackRssi = -127;

    const int count = results.getCount();
    for (int i = 0; i < count; i++) {
        const NimBLEAdvertisedDevice* dev = results.getDevice(i);
        if (!dev) {
            continue;
        }
        const NimBLEAddress& advAddr = dev->getAddress();
        if (advAddr.isNull() || isAllZeroAddress(advAddr)) {
            continue;
        }
        if (!isObdLinkName(dev->getName())) {
            continue;
        }

        const int rssi = dev->getRSSI();
        const String devAddr = String(dev->getAddress().toString().c_str());
        const bool matchesCurrentTarget =
            !targetAddress.isNull() && devAddr.equalsIgnoreCase(currentTargetAddr);

        if (matchesCurrentTarget) {
            if (!preferred || rssi > preferredRssi) {
                preferred = dev;
                preferredRssi = rssi;
            }
        } else if (!fallback || rssi > fallbackRssi) {
            fallback = dev;
            fallbackRssi = rssi;
        }
    }

    const NimBLEAdvertisedDevice* bestDevice = preferred ? preferred : fallback;
    const int bestRssi = preferred ? preferredRssi : fallbackRssi;
    if (!bestDevice) {
        Serial.printf("[OBD] Advertised connect skip (%s): no usable CX found\n",
                      profileLabel ? profileLabel : "n/a");
        restoreScanConfig();
        return false;
    }

    if (usingPin) {
        Serial.printf("[OBD] Connect try (pin=%u, %s, advertised, RSSI:%d, addr=%s)\n",
                      (unsigned)s_activePinCode.load(std::memory_order_relaxed),
                      profileLabel ? profileLabel : "n/a",
                      bestRssi,
                      bestDevice->getAddress().toString().c_str());
    } else {
        Serial.printf("[OBD] Connect try (%s, advertised, RSSI:%d, addr=%s)\n",
                      profileLabel ? profileLabel : "n/a",
                      bestRssi,
                      bestDevice->getAddress().toString().c_str());
    }

    if (!pOBDClient->connect(bestDevice, false, false, true)) {
        if (usingPin) {
            Serial.printf("[OBD] Connect attempt failed (pin=%u, %s, advertised, err=%d)\n",
                          (unsigned)s_activePinCode.load(std::memory_order_relaxed),
                          profileLabel ? profileLabel : "n/a",
                          pOBDClient->getLastError());
        } else {
            Serial.printf("[OBD] Connect attempt failed (%s, advertised, err=%d)\n",
                          profileLabel ? profileLabel : "n/a",
                          pOBDClient->getLastError());
        }
        restoreScanConfig();
        return false;
    }

    // Allow time for security handshake + conn-param negotiation.
    vTaskDelay(pdMS_TO_TICKS(500));
    if (pOBDClient->isConnected()) {
        if (!connectedPeerLooksLikeObd()) {
            Serial.printf("[OBD] Advertised peer rejected (%s): missing OBD UART\n",
                          profileLabel ? profileLabel : "n/a");
            pOBDClient->disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));
            restoreScanConfig();
            return false;
        }
        targetAddress = bestDevice->getAddress();
        restoreScanConfig();
        return true;
    }

    pOBDClient->disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    restoreScanConfig();
    return false;
}

// ---------------------------------------------------------------------------
// BLE notification callback
// ---------------------------------------------------------------------------

void OBDHandler::notificationCallback(NimBLERemoteCharacteristic* pChar,
                                      uint8_t* pData,
                                      size_t length,
                                      bool isNotify) {
    (void)pChar;
    (void)isNotify;

    if (!s_obdInstance || !pData || length == 0) {
        return;
    }

    // Write raw bytes into the lock-free stream buffer so the OBD task can
    // drain them without any mutex contention.  Zero timeout keeps the BLE
    // host task non-blocking.
    if (s_obdInstance->notifyStream) {
        size_t sent = xStreamBufferSend(s_obdInstance->notifyStream,
                                        pData, length, 0);
        if (sent < length) {
            s_obdInstance->notifyDropCount.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// ---------------------------------------------------------------------------
// FreeRTOS task lifecycle
// ---------------------------------------------------------------------------

void OBDHandler::taskEntry(void* param) {
    OBDHandler* self = static_cast<OBDHandler*>(param);
    if (!self) {
        vTaskDelete(nullptr);
        return;
    }

    while (!self->taskShouldExit.load(std::memory_order_acquire)) {
        self->runStateMachine();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    self->obdTaskHandle = nullptr;
    self->taskRunning.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void OBDHandler::startTask() {
    if (obdTaskHandle) {
        taskRunning.store(true, std::memory_order_release);
        return;
    }

    taskShouldExit.store(false, std::memory_order_release);
    const BaseType_t res = xTaskCreatePinnedToCore(taskEntry,
                                                    "obdTask",
                                                    6144,
                                                    this,
                                                    1,
                                                    &obdTaskHandle,
                                                    tskNO_AFFINITY);
    taskRunning.store(res == pdPASS, std::memory_order_release);
}

void OBDHandler::stopTask() {
    if (!obdTaskHandle) {
        return;
    }

    taskShouldExit.store(true, std::memory_order_release);
    const uint32_t startMs = millis();

    while (obdTaskHandle && (millis() - startMs) < 500) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (obdTaskHandle) {
        // Avoid deleting by handle from another task context; taskEntry() self-deletes.
        // External delete races with self-delete and can crash FreeRTOS.
        Serial.println("[OBD] WARN: stopTask timeout waiting for cooperative exit");
    }
}
