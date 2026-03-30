#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H
// ============================================================================
// Minimal WiFiManager stub for native unit tests.
// Uses #ifndef guard (matching the real wifi_manager.h) so including this
// file first blocks the real wifi_manager.h from compiling.
// ============================================================================

class WiFiManager {
public:
    bool isWifiServiceActive() const { return wifiServiceActive_; }
    bool isConnected() const         { return staConnected_; }
    bool isSetupModeActive() const   { return apActive_; }

    // Test helpers
    void setWifiServiceActive(bool v) { wifiServiceActive_ = v; }
    void setConnected(bool v)         { staConnected_ = v; }
    void setSetupModeActive(bool v)   { apActive_ = v; }

private:
    bool wifiServiceActive_ = false;
    bool staConnected_       = false;
    bool apActive_           = false;
};

extern WiFiManager wifiManager;

#endif  // WIFI_MANAGER_H
