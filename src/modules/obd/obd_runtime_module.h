#pragma once

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

struct ObdRuntimeStatus {
    bool enabled = false;
    bool connected = false;
    bool speedValid = false;
    float speedMph = 0.0f;
    uint32_t speedAgeMs = UINT32_MAX;
    uint32_t speedSampleTsMs = 0;

    int8_t rssi = 0;
    uint8_t connectAttempts = 0;
    bool scanInProgress = false;
    bool savedAddressValid = false;

    uint32_t pollCount = 0;
    uint32_t pollErrors = 0;
    uint32_t consecutiveErrors = 0;
    uint32_t totalBytesReceived = 0;
    ObdConnectionState state = ObdConnectionState::IDLE;
};

class ObdRuntimeModule {
public:
    void begin(bool enabled, const char* savedAddress, int8_t minRssi);
    void update(uint32_t nowMs, bool bootReady, bool v1Connected, bool bleScanIdle);
    ObdRuntimeStatus snapshot(uint32_t nowMs) const;

    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const;

    int8_t getConnectionRssi() const { return rssi_; }
    const char* getSavedAddress() const { return savedAddress_; }

    void startScan();
    void forgetDevice();

    void onDeviceFound(const char* name, const char* address, int rssi);

    /// Called by ObdClientCallback when BLE connection drops.
    void onBleDisconnect();

    /// Called by BLE notify callback when data arrives from OBD adapter.
    void onBleData(const uint8_t* data, size_t len);

#ifdef UNIT_TEST
    void injectSpeedForTest(float speedMph, uint32_t timestampMs);
    ObdConnectionState getState() const { return state_; }
#endif

private:
    static constexpr size_t ADDR_BUF_LEN = 18;  // "XX:XX:XX:XX:XX:XX\0"

    void transitionTo(ObdConnectionState newState, uint32_t nowMs);

    bool enabled_ = false;
    ObdConnectionState state_ = ObdConnectionState::IDLE;
    uint32_t stateEnteredMs_ = 0;
    uint32_t bootReadyMs_ = 0;

    // Speed data
    float speedMph_ = 0.0f;
    uint32_t speedSampleTsMs_ = 0;
    bool speedValid_ = false;

    // Connection
    char savedAddress_[ADDR_BUF_LEN] = {};
    char pendingAddress_[ADDR_BUF_LEN] = {};
    int8_t minRssi_ = -80;
    int8_t rssi_ = 0;
    int8_t pendingRssi_ = 0;
    bool pendingDeviceFound_ = false;
    bool scanRequested_ = false;

    // Counters
    uint8_t connectAttempts_ = 0;
    uint32_t pollCount_ = 0;
    uint32_t pollErrors_ = 0;
    uint32_t consecutiveErrors_ = 0;
    uint32_t totalBytesReceived_ = 0;
    uint32_t lastPollMs_ = 0;
    uint32_t lastRssiMs_ = 0;

    // AT init tracking
    uint8_t atInitIndex_ = 0;
    uint32_t atInitSentMs_ = 0;

    // BLE data buffer
    static constexpr size_t BLE_BUF_LEN = 64;
    char bleBuf_[BLE_BUF_LEN] = {};
    size_t bleBufLen_ = 0;
    bool bleDataReady_ = false;
    bool bleDisconnected_ = false;
};

extern ObdRuntimeModule obdRuntimeModule;
