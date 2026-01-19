/**
 * ALP BLE Client Implementation
 * 
 * Phase 1: Discovery & Logging
 * Connects to ALP and logs all BLE traffic for protocol analysis.
 */

#include "alp_client.h"
#include "settings.h"
#include "storage_manager.h"
#include <LittleFS.h>
#include <SD_MMC.h>

// Global instance
ALPClient alpClient;
ALPClient* ALPClient::instance_ = nullptr;

// Serial logging helper
#define ALP_LOG(fmt, ...) Serial.printf("[ALP] " fmt "\n", ##__VA_ARGS__)

// Known ALP device name patterns (we'll discover the actual name)
static const char* ALP_NAME_PATTERNS[] = {
    "ALP",
    "AntiLaser",
    "AL Priority",
    nullptr
};

// ============= Callback implementations =============

void ALPScanCallbacks::onResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    alpClient->handleScanResult(advertisedDevice);
}

void ALPScanCallbacks::onScanEnd(const NimBLEScanResults& scanResults, int reason) {
    alpClient->handleScanEnd(scanResults, reason);
}

void ALPClientCallbacks::onConnect(NimBLEClient* pClient) {
    alpClient->handleConnect(pClient);
}

void ALPClientCallbacks::onDisconnect(NimBLEClient* pClient, int reason) {
    alpClient->handleDisconnect(pClient, reason);
}

// ============= ALPClient implementation =============

ALPClient::ALPClient() {
    instance_ = this;
}

ALPClient::~ALPClient() {
    closeLogFile();
    if (pClient_) {
        NimBLEDevice::deleteClient(pClient_);
    }
    if (targetAddress_) {
        delete targetAddress_;
    }
    if (pScanCallbacks_) {
        delete pScanCallbacks_;
    }
    if (pClientCallbacks_) {
        delete pClientCallbacks_;
    }
    instance_ = nullptr;
}

bool ALPClient::init() {
    ALP_LOG("Initializing ALP client...");
    
    // Get the shared scan object
    pScan_ = NimBLEDevice::getScan();
    
    // Create callback objects
    if (!pScanCallbacks_) {
        pScanCallbacks_ = new ALPScanCallbacks(this);
    }
    if (!pClientCallbacks_) {
        pClientCallbacks_ = new ALPClientCallbacks(this);
    }
    
    ALP_LOG("ALP client initialized (disabled by default)");
    return true;
}

void ALPClient::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    
    enabled_ = enabled;
    ALP_LOG("ALP integration %s", enabled ? "ENABLED" : "DISABLED");
    
    if (enabled) {
        setState(ALPState::ALP_DISCONNECTED);
    } else {
        stopScan();
        disconnect();
        setState(ALPState::ALP_DISABLED);
    }
}

void ALPClient::setPairingCode(const String& code) {
    pairingCode_ = code;
    ALP_LOG("Pairing code set: %s", code.c_str());
}

void ALPClient::setState(ALPState newState) {
    if (state_ != newState) {
        ALP_LOG("State: %s -> %s", alpStateToString(state_), alpStateToString(newState));
        state_ = newState;
    }
}

bool ALPClient::startScan() {
    if (!enabled_) {
        ALP_LOG("Cannot scan - ALP disabled");
        return false;
    }
    
    if (state_ == ALPState::ALP_SCANNING) {
        ALP_LOG("Already scanning");
        return true;
    }
    
    ALP_LOG("Starting ALP device scan...");
    setState(ALPState::ALP_SCANNING);
    lastScanStart_ = millis();
    
    // Configure scan
    pScan_->setScanCallbacks(pScanCallbacks_);
    pScan_->setActiveScan(true);
    pScan_->setInterval(100);
    pScan_->setWindow(99);
    
    // Start async scan (10 seconds)
    pScan_->start(10, false, false);
    
    return true;
}

void ALPClient::stopScan() {
    if (state_ == ALPState::ALP_SCANNING) {
        pScan_->stop();
        if (state_ == ALPState::ALP_SCANNING) {
            setState(ALPState::ALP_DISCONNECTED);
        }
    }
}

// Handle scan result
void ALPClient::handleScanResult(const NimBLEAdvertisedDevice* advertisedDevice) {
    String name = advertisedDevice->getName().c_str();
    
    // Check if this looks like an ALP device
    bool isALP = false;
    for (int i = 0; ALP_NAME_PATTERNS[i] != nullptr; i++) {
        if (name.indexOf(ALP_NAME_PATTERNS[i]) >= 0) {
            isALP = true;
            break;
        }
    }
    
    // Log any device with a name (for discovery purposes)
    if (isALP || name.length() > 0) {
        ALP_LOG("Found: '%s' [%s] RSSI:%d", 
                name.c_str(),
                advertisedDevice->getAddress().toString().c_str(),
                advertisedDevice->getRSSI());
    }
    
    if (isALP) {
        ALP_LOG("*** ALP DEVICE FOUND ***");
        deviceName_ = name;
        deviceAddress_ = advertisedDevice->getAddress().toString().c_str();
        rssi_ = advertisedDevice->getRSSI();
        
        // Store address for connection
        if (targetAddress_) delete targetAddress_;
        targetAddress_ = new NimBLEAddress(advertisedDevice->getAddress());
        
        // Stop scanning
        pScan_->stop();
        setState(ALPState::ALP_FOUND);
    }
}

void ALPClient::handleScanEnd(const NimBLEScanResults& scanResults, int reason) {
    ALP_LOG("Scan ended (reason: %d, found %d devices)", reason, scanResults.getCount());
    if (state_ == ALPState::ALP_SCANNING) {
        setState(ALPState::ALP_DISCONNECTED);
    }
}

bool ALPClient::connect() {
    if (!enabled_ || !targetAddress_) {
        ALP_LOG("Cannot connect - not enabled or no target");
        return false;
    }
    
    if (state_ == ALPState::ALP_CONNECTED) {
        ALP_LOG("Already connected");
        return true;
    }
    
    ALP_LOG("Connecting to ALP: %s", targetAddress_->toString().c_str());
    setState(ALPState::ALP_CONNECTING);
    connectAttemptStart_ = millis();
    
    // Create client if needed
    if (!pClient_) {
        pClient_ = NimBLEDevice::createClient();
        pClient_->setClientCallbacks(pClientCallbacks_, false);
    }
    
    // Set connection parameters
    pClient_->setConnectionParams(12, 12, 0, 200);  // Fast connection
    
    // Attempt connection
    if (!pClient_->connect(*targetAddress_)) {
        ALP_LOG("Connection failed!");
        setState(ALPState::ALP_ERROR);
        return false;
    }
    
    // Connection successful - handled in onConnect callback
    return true;
}

void ALPClient::disconnect() {
    if (pClient_ && pClient_->isConnected()) {
        ALP_LOG("Disconnecting from ALP...");
        pClient_->disconnect();
    }
    closeLogFile();
}

void ALPClient::handleConnect(NimBLEClient* pClient) {
    ALP_LOG("*** CONNECTED TO ALP ***");
    setState(ALPState::ALP_CONNECTED);
    
    // Open log file
    openLogFile();
    
    // Discover and log all services
    dumpServices();
    
    // Subscribe to all notifications
    subscribeToAllNotifications();
}

void ALPClient::handleDisconnect(NimBLEClient* pClient, int reason) {
    ALP_LOG("Disconnected from ALP (reason: %d)", reason);
    setState(ALPState::ALP_DISCONNECTED);
    closeLogFile();
}

void ALPClient::dumpServices() {
    if (!pClient_ || !pClient_->isConnected()) return;
    
    ALP_LOG("========== ALP SERVICE DISCOVERY ==========");
    
    // Get all services
    auto& services = pClient_->getServices(true);
    
    ALP_LOG("Found %d services:", services.size());
    
    for (auto& service : services) {
        NimBLEUUID svcUUID = service->getUUID();
        ALP_LOG("  Service: %s", svcUUID.toString().c_str());
        
        // Get characteristics for this service
        auto& chars = service->getCharacteristics(true);
        
        for (auto& chr : chars) {
            NimBLEUUID chrUUID = chr->getUUID();
            
            String propStr = "";
            if (chr->canRead()) propStr += "R";
            if (chr->canWrite()) propStr += "W";
            if (chr->canWriteNoResponse()) propStr += "w";
            if (chr->canNotify()) propStr += "N";
            if (chr->canIndicate()) propStr += "I";
            
            ALP_LOG("    Char: %s [%s]", chrUUID.toString().c_str(), propStr.c_str());
            
            // Try to read if readable
            if (chr->canRead()) {
                NimBLEAttValue value = chr->readValue();
                if (value.length() > 0) {
                    String hexStr = "";
                    for (size_t i = 0; i < value.length() && i < 32; i++) {
                        char hex[4];
                        sprintf(hex, "%02X ", value.data()[i]);
                        hexStr += hex;
                    }
                    ALP_LOG("      Value: %s", hexStr.c_str());
                    
                    // Log to file
                    String svcStr = svcUUID.toString().c_str();
                    String chrStr = chrUUID.toString().c_str();
                    logPacketRaw(svcStr, chrStr, 'R', value.data(), value.length());
                }
            }
        }
    }
    
    ALP_LOG("========== END SERVICE DISCOVERY ==========");
}

bool ALPClient::subscribeToAllNotifications() {
    if (!pClient_ || !pClient_->isConnected()) return false;
    
    ALP_LOG("Subscribing to all notifications...");
    int subscribed = 0;
    
    auto& services = pClient_->getServices(false);
    
    for (auto& service : services) {
        auto& chars = service->getCharacteristics(false);
        
        for (auto& chr : chars) {
            if (chr->canNotify()) {
                if (chr->subscribe(true, notifyCallback)) {
                    ALP_LOG("  Subscribed: %s", chr->getUUID().toString().c_str());
                    subscribed++;
                }
            } else if (chr->canIndicate()) {
                if (chr->subscribe(false, notifyCallback)) {
                    ALP_LOG("  Subscribed (indicate): %s", chr->getUUID().toString().c_str());
                    subscribed++;
                }
            }
        }
    }
    
    ALP_LOG("Subscribed to %d characteristics", subscribed);
    return subscribed > 0;
}

// Static notification callback
void ALPClient::notifyCallback(NimBLERemoteCharacteristic* pChar,
                                uint8_t* pData, size_t length, bool isNotify) {
    if (!instance_) return;
    
    NimBLEUUID chrUUID = pChar->getUUID();
    NimBLEUUID svcUUID = pChar->getRemoteService()->getUUID();
    
    String svcStr = svcUUID.toString().c_str();
    String chrStr = chrUUID.toString().c_str();
    
    // Log the packet
    instance_->logPacketRaw(svcStr, chrStr, 'N', pData, length);
    instance_->packetCount_++;
}

void ALPClient::logPacketRaw(const String& serviceUUID, const String& charUUID, char operation,
                              const uint8_t* data, size_t len) {
    // Format: [timestamp] SVC:xxxx CHR:xxxx OP HEX...
    String line = "[";
    line += String(millis());
    line += "] SVC:";
    line += serviceUUID;
    line += " CHR:";
    line += charUUID;
    line += " ";
    line += operation;
    line += " ";
    
    char hex[4];
    for (size_t i = 0; i < len && i < 64; i++) {
        sprintf(hex, "%02X ", data[i]);
        line += hex;
    }
    if (len > 64) line += "...";
    
    // Log to serial
    if (logToSerial_) {
        ALP_LOG("%s", line.c_str());
    }
    
    // Log to SD card
    if (logToSD_ && logFile_) {
        logFile_.println(line);
        // Flush periodically (every 10 packets)
        if (packetCount_ % 10 == 0) {
            logFile_.flush();
        }
    }
}

void ALPClient::openLogFile() {
    if (!logToSD_) return;
    
    // Create filename with timestamp
    char filename[64];
    sprintf(filename, "/alp_log_%lu.txt", millis());
    logFilePath_ = filename;
    
    // Try SD card first, then LittleFS
    if (SD_MMC.begin()) {
        logFile_ = SD_MMC.open(logFilePath_, FILE_WRITE);
        if (logFile_) {
            ALP_LOG("Log file opened: %s (SD)", logFilePath_.c_str());
            logFile_.println("=== ALP BLE LOG ===");
            logFile_.printf("Device: %s [%s]\n", deviceName_.c_str(), deviceAddress_.c_str());
            logFile_.printf("Started: %lu ms\n", millis());
            logFile_.println("==================");
            return;
        }
    }
    
    // Fallback to LittleFS
    logFile_ = LittleFS.open(logFilePath_, FILE_WRITE);
    if (logFile_) {
        ALP_LOG("Log file opened: %s (LittleFS)", logFilePath_.c_str());
    } else {
        ALP_LOG("Failed to open log file!");
    }
}

void ALPClient::closeLogFile() {
    if (logFile_) {
        logFile_.printf("\n=== END LOG (%lu packets) ===\n", packetCount_);
        logFile_.close();
        ALP_LOG("Log file closed: %lu packets logged", packetCount_);
    }
}

uint32_t ALPClient::getLogFileSize() const {
    if (logFile_) {
        return logFile_.size();
    }
    return 0;
}

void ALPClient::process() {
    if (!enabled_) return;
    
    // Handle state machine
    switch (state_) {
        case ALPState::ALP_DISCONNECTED:
            // Could auto-reconnect here if desired
            break;
            
        case ALPState::ALP_SCANNING:
            // Check for scan timeout
            if (millis() - lastScanStart_ > 15000) {
                ALP_LOG("Scan timeout - no ALP found");
                setState(ALPState::ALP_DISCONNECTED);
            }
            break;
            
        case ALPState::ALP_FOUND:
            // Auto-connect if we have a pairing code
            if (pairingCode_.length() > 0) {
                connect();
            }
            break;
            
        case ALPState::ALP_CONNECTING:
            // Check for connection timeout
            if (millis() - connectAttemptStart_ > 10000) {
                ALP_LOG("Connection timeout");
                setState(ALPState::ALP_ERROR);
            }
            break;
            
        case ALPState::ALP_CONNECTED:
            // Normal operation - just logging
            break;
            
        case ALPState::ALP_ERROR:
            // Could retry here
            break;
            
        default:
            break;
    }
}
