/**
 * BLE Client for Valentine1 Gen2
 * With BLE Server proxy support for JBV1 app
 * 
 * Architecture:
 * - NimBLE 2.2.3 for stable dual-role operation
 * - Client connects to V1 (V1G* device names)
 * - Server advertises as V1C-LE-S3 for JBV1
 * - FreeRTOS task manages advertising timing
 * - Thread-safe with mutexes for BLE operations
 * 
 * Key Features:
 * - Automatic V1 discovery and reconnection
 * - Bidirectional proxy (V1 ↔ JBV1)
 * - Profile settings push
 * - Mode control (All Bogeys/Logic/Advanced Logic)
 * - Mute toggle
 */

#include "ble_client.h"
#include "../include/config.h"
#include <Arduino.h>
#include <set>
#include <string>
#include <cstdlib>

// Task to restart advertising after delay (Kenny's approach for NimBLE 2.x)
static void restartAdvertisingTask(void* param) {
    vTaskDelay(pdMS_TO_TICKS(150));
    Serial.println("Task: Starting advertising...");
    if (NimBLEDevice::startAdvertising()) {
        Serial.println("Task: Advertising started successfully");
    } else {
        Serial.println("Task: Advertising start failed");
    }
    
    // Verify after short delay
    vTaskDelay(pdMS_TO_TICKS(100));
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (pAdv && pAdv->isAdvertising()) {
        Serial.println("✓ Task: Proxy is now advertising!");
        NimBLEAddress addr = NimBLEDevice::getAddress();
        Serial.printf("  Address: %s\n", addr.toString().c_str());
    } else {
        Serial.println("✗ Task: Advertising still not active!");
    }
    
    vTaskDelete(NULL);
}

namespace {
class SemaphoreGuard {
public:
    explicit SemaphoreGuard(SemaphoreHandle_t sem) : sem_(sem), locked_(false) {
        if (sem_) {
            locked_ = xSemaphoreTake(sem_, portMAX_DELAY) == pdTRUE;
        }
    }
    ~SemaphoreGuard() {
        if (sem_ && locked_) {
            xSemaphoreGive(sem_);
        }
    }
    bool locked() const { return locked_; }
private:
    SemaphoreHandle_t sem_;
    bool locked_;
};

uint16_t shortUuid(const NimBLEUUID& uuid) {
    std::string s = uuid.toString();
    if (s.size() >= 8) {
        // UUID is like 92a0b2ce-9e05-11e2-aa59-f23c91aec05e → take b2ce
        return static_cast<uint16_t>(strtoul(s.substr(4, 4).c_str(), nullptr, 16));
    }
    return 0;
}
} // namespace

// Static instance for callbacks
static V1BLEClient* instancePtr = nullptr;

// Global proxy connection status
bool proxyClientConnected = false;

V1BLEClient::V1BLEClient() 
    : pClient(nullptr)
    , pRemoteService(nullptr)
    , pDisplayDataChar(nullptr)
    , pCommandChar(nullptr)
    , pServer(nullptr)
    , pProxyService(nullptr)
    , pProxyNotifyChar(nullptr)
    , pProxyWriteChar(nullptr)
    , proxyEnabled(false)
    , proxyServerInitialized(false)
    , proxyName_("V1C-LE-S3")
    , dataCallback(nullptr)
    , connectCallback(nullptr)
    , connected(false)
    , shouldConnect(false)
    , hasTargetDevice(false)
    , targetAddress()
    , lastScanStart(0) {
    instancePtr = this;
}

bool V1BLEClient::begin(bool enableProxy, const char* proxyName) {
    Serial.println("Initializing BLE...");
    
    proxyEnabled = enableProxy;
    proxyName_ = proxyName ? proxyName : "V1C-LE-S3";
    
    // Create mutexes for thread-safe BLE operations (mirroring Kenny's approach)
    bleMutex = xSemaphoreCreateMutex();
    bleNotifyMutex = xSemaphoreCreateMutex();
    
    if (!bleMutex || !bleNotifyMutex) {
        Serial.println("ERROR: Failed to create BLE mutexes");
        return false;
    }
    
    // Initialize BLE with device name
    NimBLEDevice::init(proxyEnabled ? proxyName_.c_str() : "V1Display");
    // Use public address to match V1 expectation and avoid RPA issues
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_PUBLIC);
    
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max power
    NimBLEDevice::setMTU(185);
    
    // Match Kenny's init flow: create server and START advertising BEFORE scan
    if (proxyEnabled) {
        Serial.println("Creating proxy server and starting advertising (Kenny's flow)...");
        initProxyServer(proxyName_.c_str());
        proxyServerInitialized = true;
        
        // Configure and start advertising now (will be stopped during scan)
        NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
        NimBLEAdvertisementData advData;
        NimBLEAdvertisementData scanRespData;
        advData.setCompleteServices(pProxyService->getUUID());
        advData.setAppearance(0x0C80);
        scanRespData.setName(proxyName_.c_str());
        pAdvertising->setAdvertisementData(advData);
        pAdvertising->setScanResponseData(scanRespData);
        pAdvertising->start();
        Serial.println("Proxy advertising started (will stop during scan)");
    }
    
    // Stop advertising before scanning (Kenny's approach)
    if (proxyEnabled && proxyServerInitialized) {
        Serial.println("Stopping advertising to scan for V1...");
        NimBLEDevice::stopAdvertising();
    }
    
    // Start scanning for V1 - optimized for reliable discovery
    NimBLEScan* pScan = NimBLEDevice::getScan();
    pScan->setScanCallbacks(new ScanCallbacks(this));
    pScan->setActiveScan(true);  // Request scan response to get device names
    pScan->setInterval(16);   // 10ms interval - very aggressive scanning
    pScan->setWindow(16);     // 10ms window - 100% duty cycle for fastest discovery
    pScan->setMaxResults(0);  // Unlimited results
    pScan->setDuplicateFilter(false);  // Don't filter duplicates - we want to see everything
    Serial.println("Scan configured: interval=16 (10ms), window=16 (10ms), active=true, 100% duty");
    
    Serial.println("Scanning for V1 Gen2...");
    lastScanStart = millis();
    bool started = pScan->start(SCAN_DURATION, false, false);  // duration, isContinuous, restart
    Serial.printf("Scan started: %s\n", started ? "YES" : "NO");
    
    return started;
}

bool V1BLEClient::isConnected() {
    SemaphoreGuard lock(bleMutex);
    return connected && pClient && pClient->isConnected();
}

bool V1BLEClient::isProxyClientConnected() {
    return proxyClientConnected;
}

void V1BLEClient::onDataReceived(DataCallback callback) {
    dataCallback = callback;
}

void V1BLEClient::onV1Connected(ConnectionCallback callback) {
    connectCallback = callback;
}

void V1BLEClient::ScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    String name = advertisedDevice->getName().c_str();
    std::string addrStr = advertisedDevice->getAddress().toString();
    int rssi = advertisedDevice->getRSSI();
    
    // Ignore our own proxy advertisement to avoid self-connect loops
    if (bleClient->proxyEnabled) {
        NimBLEAddress selfAddr = NimBLEDevice::getAddress();
        if (advertisedDevice->getAddress() == selfAddr) {
            return;
        }
    }
    
    // Optional: Uncomment for BLE scan debugging
    // static int debugCount = 0;
    // if (debugCount < 20) {
    //     Serial.printf("[BLE %2d] addr=%s RSSI=%3d name='%s'\n",
    //                   debugCount++, addrStr.c_str(), rssi,
    //                   name.length() > 0 ? name.c_str() : "(no name)");
    // }
    
    // *** V1 NAME FILTER - Only connect to Valentine V1 Gen2 devices ***
    // V1 Gen2 advertises as "V1G*" (like "V1G27B7A") or sometimes "V1-*"
    String nameLower = name;
    nameLower.toLowerCase();
    
    bool isV1 = nameLower.startsWith("v1g") || nameLower.startsWith("v1-");
    
    if (!isV1) {
        // Not a V1 device, keep scanning
        return;
    }
    
    // *** FOUND V1! Stop scan and connect ***
    Serial.printf("\n========================================\n");
    Serial.printf("*** FOUND V1: '%s' [%s] RSSI:%d ***\n", 
                  name.c_str(), addrStr.c_str(), rssi);
    Serial.printf("========================================\n");
    
    // Stop scanning immediately
    NimBLEDevice::getScan()->stop();
    
    // Queue connection to this V1 device
    SemaphoreGuard lock(bleClient->bleMutex);
    if (lock.locked()) {
        bleClient->targetDevice = *advertisedDevice;
        bleClient->targetAddress = bleClient->targetDevice.getAddress();
        bleClient->hasTargetDevice = true;
        bleClient->shouldConnect = true;
    }
}

void V1BLEClient::ScanCallbacks::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    Serial.printf("Scan ended: found %d devices, reason=%d\n", 
                  scanResults.getCount(), reason);
}

void V1BLEClient::ClientCallbacks::onConnect(NimBLEClient* pClient) {
    Serial.println("Connected to V1");
    if (instancePtr) {
        SemaphoreGuard lock(instancePtr->bleMutex);
        instancePtr->connected = true;
    }
}

void V1BLEClient::ClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    Serial.printf("Disconnected from V1 (reason: %d)\n", reason);
    if (instancePtr) {
        SemaphoreGuard lock(instancePtr->bleMutex);
        instancePtr->connected = false;
        instancePtr->pClient = nullptr;
        instancePtr->pRemoteService = nullptr;
        instancePtr->pDisplayDataChar = nullptr;
        instancePtr->pCommandChar = nullptr;
        // Keep proxy advertising running so clients can reconnect
        if (instancePtr->proxyEnabled && instancePtr->proxyServerInitialized) {
            NimBLEDevice::startAdvertising();
        }
    }
}

bool V1BLEClient::connectToServer() {
    std::string addrStr = targetAddress.toString();
    int addrType = hasTargetDevice ? targetDevice.getAddressType() : targetAddress.getType();
    Serial.printf("Attempting to connect to %s (type=%d)...\n", addrStr.c_str(), addrType);
    
    // Brief pause for scan to stop
    delay(50);

    bool connectedOk = false;
    int attempts = 3;
    for (int attempt = 1; attempt <= attempts && !connectedOk; ++attempt) {
        Serial.printf("Connect attempt %d/%d\n", attempt, attempts);
        
        // Always create a fresh client for V1 to avoid stale params
        pClient = NimBLEDevice::createClient();
        if (!pClient) {
            Serial.println("Failed to create client");
            break;
        }

        pClient->setClientCallbacks(new ClientCallbacks());
        pClient->setConnectionParams(12, 12, 0, 51);
        pClient->setConnectTimeout(10); // 10 second timeout

        if (hasTargetDevice) {
            Serial.println("Calling pClient->connect(targetDevice)...");
            connectedOk = pClient->connect(targetDevice, false);
            if (!connectedOk) {
                Serial.printf("connect(targetDevice) failed (error: %d); retrying with targetAddress\n", pClient->getLastError());
                connectedOk = pClient->connect(targetAddress, false);
            }
        } else {
            Serial.println("Calling pClient->connect(targetAddress)...");
            connectedOk = pClient->connect(targetAddress, false);
        }

        if (!connectedOk) {
            Serial.printf("connect attempts failed (error: %d)\n", pClient->getLastError());
            NimBLEDevice::deleteClient(pClient);
            pClient = nullptr;
            delay(50);  // Brief pause before retry
        }
    }

    if (!connectedOk) {
        Serial.printf("Failed to connect (error: %d)\n", pClient->getLastError());
        NimBLEDevice::deleteClient(pClient);
        pClient = nullptr;
        {
            SemaphoreGuard lock(bleMutex);
            shouldConnect = false;
            hasTargetDevice = false;
            targetDevice = NimBLEAdvertisedDevice(); // clear stale copy
        }
        NimBLEDevice::getScan()->start(SCAN_DURATION, false, false);
        return false;
    }
    
    Serial.println("Connected! Setting up characteristics...");
    
    // NimBLE 2.x requires explicit service discovery before getService()
    // Try to discover services with a timeout
    Serial.println("Discovering services...");
    int maxRetries = 3;
    for (int retry = 0; retry < maxRetries; retry++) {
        if (pClient->discoverAttributes()) {
            Serial.println("Service discovery completed");
            break;
        }
        Serial.printf("Service discovery attempt %d failed, retrying...\n", retry + 1);
        delay(50);
    }
    
    bool ok = setupCharacteristics();
    if (!ok) {
        Serial.println("Setup failed, disconnecting and restarting scan");
        disconnect();
        {
            SemaphoreGuard lock(bleMutex);
            shouldConnect = false;
            hasTargetDevice = false;
        }
        NimBLEDevice::getScan()->start(SCAN_DURATION, false, false);
    }
    // Advertising will be started in setupCharacteristics() after successful setup
    
    return connected;
}

bool V1BLEClient::setupCharacteristics() {
    pRemoteService = pClient->getService(V1_SERVICE_UUID);
    if (!pRemoteService) {
        Serial.println("Failed to find V1 service");
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }

    // Enumerate all characteristics in the V1 service
    auto& chars = pRemoteService->getCharacteristics(true);
    if (!chars.empty()) {
        Serial.printf("Found %u characteristics on V1 service\n", (unsigned)chars.size());
        for (auto* c : chars) {
            auto uuid = c->getUUID().toString();
            Serial.printf("Char %s props: notify=%d indicate=%d read=%d write=%d writeNR=%d\n",
                          uuid.c_str(), c->canNotify(), c->canIndicate(),
                          c->canRead(), c->canWrite(), c->canWriteNoResponse());
        }
    } else {
        Serial.println("No characteristics found on V1 service");
    }
    
    // Get display data characteristic (notify)
    pDisplayDataChar = pRemoteService->getCharacteristic(V1_DISPLAY_DATA_UUID);
    if (!pDisplayDataChar) {
        Serial.println("Failed to find display data characteristic");
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }
    Serial.printf("DisplayChar props: notify=%d indicate=%d read=%d write=%d writeNR=%d\n",
                  pDisplayDataChar->canNotify(), pDisplayDataChar->canIndicate(),
                  pDisplayDataChar->canRead(), pDisplayDataChar->canWrite(), pDisplayDataChar->canWriteNoResponse());
    
    // Get command characteristic (write)
    pCommandChar = pRemoteService->getCharacteristic(V1_COMMAND_WRITE_UUID);
    NimBLERemoteCharacteristic* altCommandChar = nullptr;
    if (pRemoteService) {
        altCommandChar = pRemoteService->getCharacteristic(V1_COMMAND_WRITE_ALT_UUID);
    }

    // Prefer the primary B6D4 characteristic; only fall back to BAD4 if B6D4 is unusable
    if (!pCommandChar || (!pCommandChar->canWrite() && !pCommandChar->canWriteNoResponse())) {
        if (altCommandChar && (altCommandChar->canWrite() || altCommandChar->canWriteNoResponse())) {
            Serial.println("Primary command char unusable, falling back to BAD4");
            pCommandChar = altCommandChar;
        } else {
            Serial.println("Command characteristic not available");
            pClient->disconnect();
            SemaphoreGuard lock(bleMutex);
            connected = false;
            return false;
        }
    }

    if (!pCommandChar) {
        Serial.println("Failed to find command characteristic");
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }
    Serial.printf("CommandChar props: notify=%d indicate=%d read=%d write=%d writeNR=%d\n",
                  pCommandChar->canNotify(), pCommandChar->canIndicate(),
                  pCommandChar->canRead(), pCommandChar->canWrite(), pCommandChar->canWriteNoResponse());
    
    // Subscribe to notifications (main display data characteristic only)
    // Following Kenny's approach: only subscribe to B2CE for alert data
    bool subscribed = false;
    if (pDisplayDataChar->canNotify()) {
        subscribed = pDisplayDataChar->subscribe(true, notifyCallback, true);
        Serial.println(subscribed ? "Subscribed to display data notifications" : "Failed to subscribe");
    } else if (pDisplayDataChar->canIndicate()) {
        subscribed = pDisplayDataChar->subscribe(false, notifyCallback);
        Serial.println(subscribed ? "Subscribed to indications for display data" : "Failed to subscribe (indicate)");
    } else {
        Serial.println("Display characteristic cannot notify or indicate!");
    }

    if (!subscribed) {
        pClient->disconnect();
        SemaphoreGuard lock(bleMutex);
        connected = false;
        return false;
    }

    // Force CCCD write for notifications if descriptor is present
    NimBLERemoteDescriptor* cccd = pDisplayDataChar->getDescriptor(NimBLEUUID((uint16_t)0x2902));
    if (cccd) {
        uint8_t notifOn[] = {0x01, 0x00};
        if (cccd->writeValue(notifOn, sizeof(notifOn), true)) {
            Serial.println("Wrote CCCD to enable notifications");
        } else {
            Serial.println("Failed to write CCCD for notifications");
            pClient->disconnect();
            SemaphoreGuard lock(bleMutex);
            connected = false;
            return false;
        }
    } else {
        Serial.println("No CCCD descriptor found on display characteristic");
    }
    
    // Try an initial read for sanity
    if (pDisplayDataChar->canRead()) {
        std::string v = pDisplayDataChar->readValue();
        Serial.printf("Initial display value len=%u\n", (unsigned)v.size());
    }

    {
        SemaphoreGuard lock(bleMutex);
        connected = true;
    }
    
    // Now that V1 is connected, start proxy advertising if enabled
    // Use FreeRTOS task with delay for NimBLE 2.x compatibility (Kenny's approach)
    if (proxyEnabled && proxyServerInitialized) {
        Serial.println("V1 connected! Scheduling proxy advertising...");
        startProxyAdvertising();
    }
    
    // Request alert data from V1 - brief delay for MTU negotiation
    delay(100);  // Reduced delay - MTU should settle quickly
    
    if (!requestAlertData()) {
        Serial.println("Failed to request alert data (non-critical)");
    }
    
    // Notify user callback that V1 connection is fully established
    if (connectCallback) {
        Serial.println("Calling V1 connection callback...");
        connectCallback();
    }

    return connected;
}

void V1BLEClient::notifyCallback(NimBLERemoteCharacteristic* pChar, 
                                  uint8_t* pData, 
                                  size_t length, 
                                  bool isNotify) {
    if (!pData || !instancePtr || !pChar) {
        return;
    }
    
    uint16_t charId = shortUuid(pChar->getUUID());
    if (charId == 0) {
        charId = 0xB2CE; // sensible fallback
    }

    // Route proxy notifications (only B2CE alerts)
    if (instancePtr->proxyEnabled && proxyClientConnected && instancePtr->pProxyNotifyChar) {
        if (xSemaphoreTake(instancePtr->bleNotifyMutex, pdMS_TO_TICKS(50))) {
            instancePtr->pProxyNotifyChar->setValue(pData, length);
            instancePtr->pProxyNotifyChar->notify();
            xSemaphoreGive(instancePtr->bleNotifyMutex);
        }
    }
    
    // Also call user callback for display processing (default to B2CE)
    if (instancePtr->dataCallback) {
        instancePtr->dataCallback(pData, length, charId);
    }
}

bool V1BLEClient::sendCommand(const uint8_t* data, size_t length) {
    if (!isConnected() || !pCommandChar) {
        //Serial.println("sendCommand: not connected or command characteristic missing");
        return false;
    }
    
    // Validate inputs
    if (!data) {
        //Serial.println("sendCommand: ERROR - null data pointer");
        return false;
    }
    if (length == 0) {
        //Serial.println("sendCommand: ERROR - zero length");
        return false;
    }
    if (length > 64) {  // Reasonable maximum for BLE packets
        //Serial.printf("sendCommand: ERROR - length %u exceeds maximum (64)\n", (unsigned)length);
        return false;
    }
    
    // Don't print during command forwarding - causes crashes in callback context
    // Serial.printf("sendCommand: sending %u bytes: ", (unsigned)length);
    // for (size_t i = 0; i < length; i++) {
    //     Serial.printf("%02X ", data[i]);
    // }
    // Serial.println();
    
    bool ok = false;
    if (pCommandChar->canWrite()) {
        // Use write-with-response when supported
        ok = pCommandChar->writeValue(data, length, true);
    } else if (pCommandChar->canWriteNoResponse()) {
        // Use write-without-response when that's the only option
        ok = pCommandChar->writeValue(data, length, false);
    } else {
        return false;
    }
    
    // Only log failure to keep output clean
    if (!ok) {
        Serial.println("sendCommand: write failed");
    }
    return ok;
}

bool V1BLEClient::requestAlertData() {
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_REQ_START_ALERT,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.println("Requesting alert data from V1...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::requestVersion() {
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_VERSION,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.println("Requesting version info from V1...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setDisplayOn(bool on) {
    // For dark mode, we need to use reqTurnOffMainDisplay with proper payload
    // Mode 0 = completely off, Mode 1 = only BT icon visible
    // For "dark mode on" we want display OFF, for "dark mode off" we want display ON
    // 
    // Based on Kenny's v1g2-t4s3 implementation:
    // - reqTurnOnMainDisplay: 7-byte packet with payloadLength=1, no actual payload data
    // - reqTurnOffMainDisplay: 8-byte packet with payloadLength=2, only 1 mode byte in payload
    
    if (on) {
        // Turn display back ON (exit dark mode)
        // Packet: AA DA E4 33 01 [checksum] AB  (7 bytes total)
        uint8_t packet[] = {
            ESP_PACKET_START,                               // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
            PACKET_ID_TURN_ON_DISPLAY,                      // [3] 0x33
            0x01,                                           // [4] payload length
            0x00,                                           // [5] checksum placeholder
            ESP_PACKET_END                                  // [6] 0xAB
        };
        
        // Calculate checksum over bytes 0-4 (5 bytes)
        uint8_t checksum = 0;
        for (size_t i = 0; i < 5; ++i) {
            checksum += packet[i];
        }
        packet[5] = checksum;
        
        Serial.printf("Setting V1 display ON (exit dark mode), packet: ");
        for (size_t i = 0; i < sizeof(packet); i++) {
            Serial.printf("%02X ", packet[i]);
        }
        Serial.println();
        
        return sendCommand(packet, sizeof(packet));
    } else {
        // Turn display OFF (enter dark mode)
        // Per Kenny's implementation: payloadLength=2 but only 1 actual payload byte
        // Packet: AA DA E4 32 02 [mode] [checksum] AB  (8 bytes total)
        // mode=0: completely dark, mode=1: only BT icon visible
        uint8_t mode = 0x00;  // Completely dark
        uint8_t packet[] = {
            ESP_PACKET_START,                               // [0] 0xAA
            static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
            static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
            PACKET_ID_TURN_OFF_DISPLAY,                     // [3] 0x32
            0x02,                                           // [4] payload length = 2
            mode,                                           // [5] mode byte
            0x00,                                           // [6] checksum placeholder
            ESP_PACKET_END                                  // [7] 0xAB
        };
        
        // Calculate checksum over bytes 0-5 (6 bytes)
        uint8_t checksum = 0;
        for (size_t i = 0; i < 6; ++i) {
            checksum += packet[i];
        }
        packet[6] = checksum;
        
        Serial.printf("Setting V1 display OFF (dark mode), packet: ");
        for (size_t i = 0; i < sizeof(packet); i++) {
            Serial.printf("%02X ", packet[i]);
        }
        Serial.println();
        
        return sendCommand(packet, sizeof(packet));
    }
}

bool V1BLEClient::setMute(bool muted) {
    uint8_t packetId = muted ? PACKET_ID_MUTE_ON : PACKET_ID_MUTE_OFF;
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        packetId,
        0x01,
        0x00,
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.printf("Setting V1 mute %s...\n", muted ? "ON" : "OFF");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::setMode(uint8_t mode) {
    // Packet ID 0x36 = REQCHANGEMODE
    // Mode: 0x01 = All Bogeys, 0x02 = Logic, 0x03 = Advanced Logic
    uint8_t packet[] = {
        ESP_PACKET_START,                               // [0] 0xAA
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),// [1] 0xDA
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE), // [2] 0xE4
        0x36,                                           // [3] REQCHANGEMODE
        0x02,                                           // [4] payload length = 2
        mode,                                           // [5] mode byte
        0x00,                                           // [6] checksum placeholder
        ESP_PACKET_END                                  // [7] 0xAB
    };
    
    // Calculate checksum over bytes 0-5 (6 bytes)
    uint8_t checksum = 0;
    for (size_t i = 0; i < 6; ++i) {
        checksum += packet[i];
    }
    packet[6] = checksum;
    
    const char* modeName = "Unknown";
    if (mode == 0x01) modeName = "All Bogeys";
    else if (mode == 0x02) modeName = "Logic";
    else if (mode == 0x03) modeName = "Advanced Logic";
    
    Serial.printf("Setting V1 mode to %s (0x%02X), packet: ", modeName, mode);
    for (size_t i = 0; i < sizeof(packet); i++) {
        Serial.printf("%02X ", packet[i]);
    }
    Serial.println();
    
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::requestUserBytes() {
    // Build packet: AA D0+dest E0+src 11 01 [checksum] AB
    uint8_t packet[] = {
        ESP_PACKET_START,
        static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1),
        static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE),
        PACKET_ID_REQ_USER_BYTES,
        0x01,  // length
        0x00,  // checksum placeholder
        ESP_PACKET_END
    };

    uint8_t checksum = 0;
    for (size_t i = 0; i < 5; ++i) {
        checksum += packet[i];
    }
    packet[5] = checksum;

    Serial.println("Requesting V1 user bytes...");
    return sendCommand(packet, sizeof(packet));
}

bool V1BLEClient::writeUserBytes(const uint8_t* bytes) {
    if (!bytes) {
        return false;
    }
    
    // Build packet: AA D0+dest E0+src 13 07 [6 bytes] [checksum] AB
    uint8_t packet[13];
    packet[0] = ESP_PACKET_START;
    packet[1] = static_cast<uint8_t>(0xD0 + ESP_PACKET_DEST_V1);
    packet[2] = static_cast<uint8_t>(0xE0 + ESP_PACKET_REMOTE);
    packet[3] = PACKET_ID_WRITE_USER_BYTES;
    packet[4] = 0x07;  // length = 6 bytes + 1
    memcpy(&packet[5], bytes, 6);
    
    uint8_t checksum = 0;
    for (size_t i = 0; i < 11; ++i) {
        checksum += packet[i];
    }
    packet[11] = checksum;
    packet[12] = ESP_PACKET_END;

    Serial.printf("Writing V1 user bytes: %02X %02X %02X %02X %02X %02X\n",
        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return sendCommand(packet, sizeof(packet));
}

void V1BLEClient::process() {
    bool connectNow = false;
    {
        SemaphoreGuard lock(bleMutex);
        if (lock.locked()) {
            connectNow = shouldConnect;
            shouldConnect = false;
        }
    }

    if (connectNow) {
        connectToServer();
        return;
    }
    
    // If not connected and not currently scanning, restart scan
    bool pendingConnect = false;
    {
        SemaphoreGuard lock(bleMutex);
        if (lock.locked()) {
            pendingConnect = shouldConnect;
        }
    }

    if (!isConnected() && !pendingConnect) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (!pScan->isScanning()) {
            unsigned long now = millis();
            if (now - lastScanStart >= RECONNECT_DELAY) {
                Serial.println("Restarting scan for V1...");
                lastScanStart = now;
                pScan->clearResults();
                bool started = pScan->start(SCAN_DURATION, false, false);
                Serial.printf("Scan restart: %s\n", started ? "YES" : "NO");
            }
        }
    }
}

void V1BLEClient::startScanning() {
    if (!isConnected()) {
        NimBLEScan* pScan = NimBLEDevice::getScan();
        if (!pScan->isScanning()) {
            Serial.println("Starting scan for V1...");
            lastScanStart = millis();
            pScan->start(SCAN_DURATION, false, false);
        }
    }
}

bool V1BLEClient::isScanning() {
    NimBLEScan* pScan = NimBLEDevice::getScan();
    return pScan && pScan->isScanning();
}

void V1BLEClient::disconnect() {
    if (pClient && pClient->isConnected()) {
        pClient->disconnect();
    }
}

// ==================== BLE Proxy Server Functions ====================

void V1BLEClient::ProxyServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
    Serial.println("===== JBV1 PROXY CLIENT CONNECTED =====");
    proxyClientConnected = true;
}

void V1BLEClient::ProxyServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
    Serial.printf("===== JBV1 PROXY CLIENT DISCONNECTED (reason: %d) =====\n", reason);
    proxyClientConnected = false;
    
    // Resume advertising if V1 is still connected
    if (instancePtr && instancePtr->isConnected()) {
        Serial.println("Resuming proxy advertising...");
        NimBLEDevice::startAdvertising();
    }
}

void V1BLEClient::ProxyWriteCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
    // Forward commands from JBV1 to V1
    if (!pCharacteristic || !bleClient) {
        Serial.println("ProxyWrite: null pCharacteristic or bleClient");
        return;
    }
    
    if (!bleClient->isConnected()) {
        Serial.println("ProxyWrite: V1 not connected");
        return;
    }
    
    // Get the raw data pointer and length directly to avoid std::string issues
    NimBLEAttValue attrValue = pCharacteristic->getValue();
    const uint8_t* rawData = attrValue.data();
    size_t rawLen = attrValue.size();
    
    if (rawLen == 0 || !rawData) {
        Serial.println("ProxyWrite: empty data");
        return;
    }
    
    // Sanity check on size before processing
    if (rawLen > 32) {
        Serial.printf("ProxyWrite: data too large (%u bytes), rejecting\n", (unsigned)rawLen);
        return;
    }
    
    // Log the command from JBV1
    char hexBuf[128];
    int pos = snprintf(hexBuf, sizeof(hexBuf), "JBV1→V1: len=%u bytes: ", (unsigned)rawLen);
    for (size_t i = 0; i < rawLen && pos < 120; i++) {
        pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", rawData[i]);
    }
    Serial.println(hexBuf);
    
    // Copy to a safe buffer
    uint8_t cmdBuf[32];
    memcpy(cmdBuf, rawData, rawLen);
    
    // Forward all commands to V1 (no version spoofing)
    if (!bleClient->sendCommand(cmdBuf, rawLen)) {
        Serial.println("ProxyWrite: sendCommand failed!");
    } else {
        Serial.println("ProxyWrite: command forwarded OK");
    }
}

void V1BLEClient::initProxyServer(const char* deviceName) {
    Serial.printf("Creating BLE proxy server as '%s'\n", deviceName);
    
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ProxyServerCallbacks());
    
    // Ensure server allows connections
    NimBLEDevice::setSecurityAuth(false, false, true);
    
    // Create service with V1 UUID so JBV1 recognizes us
    pProxyService = pServer->createService(V1_SERVICE_UUID);
    
    // Proxy using 2 characteristics like Kenny's approach:
    // 1. Display data notification (0xB2CE) - for alerts from V1 → proxy clients
    pProxyNotifyChar = pProxyService->createCharacteristic(
        V1_DISPLAY_DATA_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    
    // 2. Command write (0xB6D4) - for commands from proxy clients → V1
    pProxyWriteChar = pProxyService->createCharacteristic(
        V1_COMMAND_WRITE_UUID,
        NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
    );
    pProxyWriteChar->setCallbacks(new ProxyWriteCallbacks(this));
    
    pProxyService->start();
    Serial.println("Proxy service created with 2 characteristics (notify + write)");
}

void V1BLEClient::startProxyAdvertising() {
    if (!proxyServerInitialized || !pServer) {
        Serial.println("Cannot start advertising - proxy server not initialized");
        return;
    }
    
    // Advertising data already configured in begin()
    // Just restart advertising using task with delay (Kenny's approach)
    Serial.println("Creating advertising restart task...");
    xTaskCreate(restartAdvertisingTask, "adv_restart", 2048, NULL, 1, NULL);
}

void V1BLEClient::forwardToProxy(const uint8_t* data, size_t length, uint16_t sourceCharUUID) {
    if (!proxyEnabled || !proxyClientConnected) {
        return;
    }
    
    // Route data based on source characteristic
    if (pProxyNotifyChar) {
        pProxyNotifyChar->setValue(data, length);
        pProxyNotifyChar->notify();
    }
}
