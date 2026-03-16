#pragma once

#include <cstddef>
#include <cstdint>

enum class ObdConnectionState : uint8_t {
    IDLE = 0,
    WAIT_BOOT = 1,
    SCANNING = 2,
    CONNECTING = 4,
    DISCOVERING = 5,
    AT_INIT = 6,
    POLLING = 7,
    ERROR_BACKOFF = 8,
    DISCONNECTED = 9,
};

enum class ObdCommandKind : uint8_t {
    NONE = 0,
    AT_INIT = 1,
    SANITY = 2,
    SPEED = 3,
    VIN = 4,
    EOT_PROBE = 5,
    EOT_POLL = 6,
};

enum class ObdVehicleFamily : uint8_t {
    UNKNOWN = 0,
    FORD = 1,
    FCA = 2,
    VW_AUDI_PORSCHE = 3,
};

enum class ObdEotProfileId : uint8_t {
    NONE = 0,
    SAE_015C = 1,
    FORD_22F45C = 2,
    FORD_221310 = 3,
    FCA_21E8 = 4,
    VW_2230F9 = 5,
    VW_2230DB = 6,
    VW_223A59 = 7,
};

enum class ObdFailureReason : uint8_t {
    NONE = 0,
    CONNECT_START = 1,
    CONNECT_TIMEOUT = 2,
    DISCOVERY = 3,
    SUBSCRIBE = 4,
    INIT_TIMEOUT = 5,
    INIT_RESPONSE = 6,
    COMMAND_TIMEOUT = 7,
    COMMAND_RESPONSE = 8,
    WRITE = 9,
    BUFFER_OVERFLOW = 10,
    VIN_MISMATCH = 11,
};

struct ObdRuntimeStatus {
    bool enabled = false;
    bool connected = false;
    bool speedValid = false;
    float speedMph = 0.0f;
    uint32_t speedAgeMs = UINT32_MAX;
    uint32_t speedSampleTsMs = 0;

    bool eotValid = false;
    int16_t eotC_x10 = 0;
    uint32_t eotAgeMs = UINT32_MAX;
    ObdEotProfileId eotProfileId = ObdEotProfileId::NONE;
    uint32_t eotProbeFailures = 0;
    bool cachedProfileActive = false;

    bool vinDetected = false;
    char vin[18] = {};
    ObdVehicleFamily vehicleFamily = ObdVehicleFamily::UNKNOWN;

    int8_t rssi = 0;
    uint8_t connectAttempts = 0;
    uint32_t connectSuccesses = 0;
    uint32_t connectFailures = 0;
    bool scanInProgress = false;
    bool savedAddressValid = false;
    uint8_t initRetries = 0;

    uint32_t pollCount = 0;
    uint32_t pollErrors = 0;
    uint32_t staleSpeedCount = 0;
    uint32_t consecutiveErrors = 0;
    uint32_t totalBytesReceived = 0;
    uint32_t bufferOverflows = 0;

    uint32_t lastConnectStartMs = 0;
    uint32_t lastConnectSuccessMs = 0;
    uint32_t lastFailureMs = 0;
    ObdFailureReason lastFailure = ObdFailureReason::NONE;
    ObdCommandKind commandInFlight = ObdCommandKind::NONE;
    ObdConnectionState state = ObdConnectionState::IDLE;
};

class ObdRuntimeModule {
public:
    void begin(bool enabled,
               const char* savedAddress,
               uint8_t savedAddrType,
               int8_t minRssi,
               const char* cachedVinPrefix11 = nullptr,
               uint8_t cachedEotProfileId = 0);
    void update(uint32_t nowMs, bool bootReady, bool v1Connected, bool bleScanIdle);
    ObdRuntimeStatus snapshot(uint32_t nowMs) const;

    void setEnabled(bool enabled);
    void setMinRssi(int8_t minRssi);
    bool isEnabled() const { return enabled_; }

    bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const;

    int8_t getConnectionRssi() const { return rssi_; }
    const char* getSavedAddress() const { return savedAddress_; }
    uint8_t getSavedAddrType() const { return savedAddrType_; }
    const char* getCachedVinPrefix11() const { return cachedVinPrefix11_; }
    uint8_t getCachedEotProfileId() const { return static_cast<uint8_t>(cachedEotProfileId_); }

    void startScan();
    void forgetDevice();

    void onDeviceFound(const char* name, const char* address, int rssi, uint8_t addrType = 0);
    void onBleDisconnect();
    void onBleData(const uint8_t* data, size_t len);

#ifdef UNIT_TEST
    void injectSpeedForTest(float speedMph, uint32_t timestampMs);
    void forceStateForTest(ObdConnectionState state, uint32_t enteredMs);
    void setConsecutiveErrorsForTest(uint32_t errors) { consecutiveErrors_ = errors; }
    ObdConnectionState getState() const { return state_; }
    ObdCommandKind getActiveCommandKindForTest() const;
    ObdEotProfileId getActiveEotProfileForTest() const;
    ObdVehicleFamily getVehicleFamilyForTest() const { return vehicleFamily_; }
    const char* getVinForTest() const { return vin_; }
    uint32_t getStartScanCallCountForTest() const { return testStartScanCalls_; }
    uint32_t getConnectCallCountForTest() const { return testConnectCalls_; }
    uint32_t getDiscoverCallCountForTest() const { return testDiscoverCalls_; }
    uint32_t getDisconnectCallCountForTest() const { return testDisconnectCalls_; }
    uint32_t getWriteCallCountForTest() const { return testWriteCalls_; }
    const char* getLastCommandForTest() const { return testLastCommand_; }
    void setTestStartScanResult(bool result) { testStartScanResult_ = result; }
    void setTestConnectResult(bool result) { testConnectResult_ = result; }
    void setTestBleConnected(bool connected) { testBleConnected_ = connected; }
    void setTestDiscoverResult(bool result) { testDiscoverResult_ = result; }
    void setTestSubscribeResult(bool result) { testSubscribeResult_ = result; }
    void setTestWriteResult(bool result) { testWriteResult_ = result; }
    void setTestRssi(int8_t rssi) { testRssi_ = rssi; }
#endif

private:
    static constexpr size_t ADDR_BUF_LEN = 18;
    static constexpr size_t VIN_BUF_LEN = 18;
    static constexpr size_t VIN_PREFIX_LEN = 12;
    static constexpr size_t CMD_BUF_LEN = 16;
    static constexpr size_t BLE_BUF_LEN = 256;

    enum class ParserKind : uint8_t {
        NONE = 0,
        AT_TEXT = 1,
        SIMPLE = 2,
        VIN = 3,
    };

    struct ActiveObdCommand {
        bool active = false;
        ObdCommandKind kind = ObdCommandKind::NONE;
        ParserKind parser = ParserKind::NONE;
        char tx[CMD_BUF_LEN] = {};
        uint8_t expectedService = 0;
        uint8_t expectedPid = 0;
        uint16_t expectedDid = 0;
        uint32_t timeoutMs = 0;
        uint8_t retriesRemaining = 0;
        uint32_t sentMs = 0;
        ObdEotProfileId profileId = ObdEotProfileId::NONE;
    };

    void resetForBegin();
    void transitionTo(ObdConnectionState newState, uint32_t nowMs);
    void clearBleResponseState();
    void clearSpeedState();
    void clearEotState(bool clearProfile);
    void clearVehicleState(bool clearCache);
    void resetPollingSchedule(uint32_t nowMs);
    void resetInitState(bool preferWarmInit);
    void resetCommandState();

    void markFailure(ObdFailureReason reason, uint32_t nowMs);
    void handleConnectFailure(uint32_t nowMs, ObdFailureReason reason);
    void handlePollingError(uint32_t nowMs, bool disconnectBleNow, ObdFailureReason reason);
    void handleCommandFailure(uint32_t nowMs, ObdFailureReason reason, bool disconnectBleNow);
    void setSavedAddressFromBuffer(const char* address);
    void setCachedProfile(const char* vinPrefix11, ObdEotProfileId profileId);
    void clearCachedProfile();

    bool startBleScan();
    bool connectBle(uint32_t timeoutMs, bool preferCachedAttributes);
    bool isBleConnected() const;
    bool discoverBleServices();
    bool subscribeBleNotifications();
    bool writeBleCommand(const char* cmd);
    void disconnectBle();
    void stopBleScan();
    int8_t readBleRssi(uint32_t nowMs);

    bool validateAtResponse(const char* command, const char* response, size_t len) const;
    bool startCommand(ObdCommandKind kind,
                      ParserKind parser,
                      const char* tx,
                      uint8_t expectedService,
                      uint8_t expectedPid,
                      uint16_t expectedDid,
                      uint32_t timeoutMs,
                      uint8_t retries,
                      ObdEotProfileId profileId,
                      uint32_t nowMs);
    bool retryActiveCommand(uint32_t nowMs);
    void completeActiveCommand();
    void handleAtInitResponse(uint32_t nowMs);
    void handlePollingResponse(uint32_t nowMs);
    void updateAtInit(uint32_t nowMs);
    void updatePolling(uint32_t nowMs);

    bool isSpeedFresh(uint32_t nowMs) const;
    bool isEotFresh(uint32_t nowMs) const;
    bool speedDue(uint32_t nowMs) const;
    bool auxWindowOpen(uint32_t nowMs) const;
    bool sendNextPollingCommand(uint32_t nowMs);
    bool startSpeedCommand(uint32_t nowMs);
    bool startVinCommand(uint32_t nowMs);
    bool startEotProbeCommand(uint32_t nowMs);
    bool startEotPollCommand(uint32_t nowMs);

    bool handleSpeedResponse(uint32_t nowMs);
    bool handleVinResponse(uint32_t nowMs);
    bool handleEotResponse(uint32_t nowMs, bool probing);
    bool validateSimpleResponse(uint8_t expectedService,
                                uint8_t expectedPid,
                                uint16_t expectedDid,
                                const char* response,
                                size_t len) const;

    static ObdVehicleFamily detectVehicleFamily(const char* vin);
    static const char* vehicleFamilyName(ObdVehicleFamily family);
    static const char* commandKindName(ObdCommandKind kind);
    static bool profileNeedsVin(ObdEotProfileId profileId);

    bool selectInitialCachedProfile();
    void resetProbeState();
    ObdEotProfileId nextProbeProfile() const;
    bool validateEotSample(int16_t tempC_x10, uint32_t nowMs, ObdEotProfileId profileId) const;
    void markProfileUnsupported(ObdEotProfileId profileId);
    bool isProfileUnsupported(ObdEotProfileId profileId) const;

    bool enabled_ = false;
    ObdConnectionState state_ = ObdConnectionState::IDLE;
    uint32_t stateEnteredMs_ = 0;
    uint32_t bootReadyMs_ = 0;
    bool stateEntryPending_ = false;

    float speedMph_ = 0.0f;
    uint32_t speedSampleTsMs_ = 0;
    bool speedValid_ = false;

    int16_t eotC_x10_ = 0;
    uint32_t eotSampleTsMs_ = 0;
    bool eotValid_ = false;
    ObdEotProfileId activeEotProfileId_ = ObdEotProfileId::NONE;
    bool activeEotProfileFromCache_ = false;
    uint8_t cachedProfileValidSamples_ = 0;
    uint8_t cachedProfileInvalidStreak_ = 0;
    uint32_t eotProbeFailures_ = 0;

    char vin_[VIN_BUF_LEN] = {};
    char vinPrefix11_[VIN_PREFIX_LEN] = {};
    bool vinDetected_ = false;
    ObdVehicleFamily vehicleFamily_ = ObdVehicleFamily::UNKNOWN;
    char cachedVinPrefix11_[VIN_PREFIX_LEN] = {};
    ObdEotProfileId cachedEotProfileId_ = ObdEotProfileId::NONE;
    uint32_t unsupportedProfileMask_ = 0;

    char savedAddress_[ADDR_BUF_LEN] = {};
    char pendingAddress_[ADDR_BUF_LEN] = {};
    int8_t minRssi_ = -80;
    int8_t rssi_ = 0;
    int8_t pendingRssi_ = 0;
    uint8_t pendingAddrType_ = 0;
    uint8_t savedAddrType_ = 0;
    bool pendingDeviceFound_ = false;
    bool scanRequested_ = false;
    bool preferWarmReconnect_ = false;
    bool warmInitPreferred_ = false;
    bool coldInitFallbackUsed_ = false;

    uint8_t connectAttempts_ = 0;
    uint32_t connectSuccesses_ = 0;
    uint32_t connectFailures_ = 0;
    uint32_t pollCount_ = 0;
    uint32_t pollErrors_ = 0;
    uint32_t staleSpeedCount_ = 0;
    uint32_t consecutiveErrors_ = 0;
    uint32_t totalBytesReceived_ = 0;
    uint32_t lastRssiMs_ = 0;
    uint32_t bufferOverflowCount_ = 0;
    uint8_t initRetries_ = 0;
    uint32_t lastConnectStartMs_ = 0;
    uint32_t lastConnectSuccessMs_ = 0;
    uint32_t lastFailureMs_ = 0;
    ObdFailureReason lastFailure_ = ObdFailureReason::NONE;

    uint32_t nextSpeedDueMs_ = 0;
    uint32_t nextVinAttemptMs_ = 0;
    uint32_t nextEotProbeMs_ = 0;
    uint32_t nextEotPollMs_ = 0;
    uint8_t initIndex_ = 0;

    ActiveObdCommand activeCommand_ = {};

    char bleBuf_[BLE_BUF_LEN] = {};
    size_t bleBufLen_ = 0;
    bool bleDataReady_ = false;
    bool bleDisconnected_ = false;
    bool bleOverflowed_ = false;

#ifdef UNIT_TEST
    bool testStartScanResult_ = true;
    bool testConnectResult_ = true;
    bool testBleConnected_ = false;
    bool testDiscoverResult_ = true;
    bool testSubscribeResult_ = true;
    bool testWriteResult_ = true;
    int8_t testRssi_ = 0;
    uint32_t testStartScanCalls_ = 0;
    uint32_t testConnectCalls_ = 0;
    uint32_t testDiscoverCalls_ = 0;
    uint32_t testDisconnectCalls_ = 0;
    uint32_t testWriteCalls_ = 0;
    char testLastCommand_[CMD_BUF_LEN] = {};
#endif
};

extern ObdRuntimeModule obdRuntimeModule;
