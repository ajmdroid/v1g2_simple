#ifndef BLE_CLIENT_H
#define BLE_CLIENT_H

#include <stdint.h>

/**
 * Mock V1BLEClient - tracks method calls for verification
 */
class V1BLEClient {
public:
    // Call tracking
    int setMuteCalls = 0;
    bool lastMuteValue = false;
    int setVolumeCalls = 0;
    uint8_t lastVolume = 0;
    uint8_t lastMuteVolume = 0;
    int requestAlertDataCalls = 0;
    int processProxyQueueCalls = 0;
    int onUserBytesReceivedCalls = 0;
    bool bootReadyFlag = true;  // Default true to preserve existing test behavior
    int connectionRssi = -70;
    int proxyRssi = -80;
    
    void reset() {
        proxyConnected = false;
        connected = true;
        setMuteCalls = 0;
        lastMuteValue = false;
        setVolumeCalls = 0;
        lastVolume = 0;
        lastMuteVolume = 0;
        requestAlertDataCalls = 0;
        processProxyQueueCalls = 0;
        onUserBytesReceivedCalls = 0;
        bootReadyFlag = true;
        connectionRssi = -70;
        proxyRssi = -80;
    }
    
    // Connection state
    bool isProxyClientConnected() const { return proxyConnected; }
    void setProxyConnected(bool v) { proxyConnected = v; }
    bool isConnected() const { return connected; }
    void setConnected(bool v) { connected = v; }
    void setBootReady(bool ready) { bootReadyFlag = ready; }
    bool isBootReady() const { return bootReadyFlag; }
    int getConnectionRssi() const { return connectionRssi; }
    int getProxyClientRssi() const { return proxyRssi; }
    void setConnectionRssi(int rssi) { connectionRssi = rssi; }
    void setProxyRssi(int rssi) { proxyRssi = rssi; }
    
    // BLE commands (tracked)
    bool setMute(bool mute) { 
        setMuteCalls++; 
        lastMuteValue = mute;
        return true; 
    }
    
    bool setVolume(uint8_t vol, uint8_t muteVol) { 
        setVolumeCalls++;
        lastVolume = vol;
        lastMuteVolume = muteVol;
        return true; 
    }
    
    void requestAlertData() {
        requestAlertDataCalls++;
    }

    void processProxyQueue() {
        processProxyQueueCalls++;
    }

    void onUserBytesReceived(const uint8_t* /*bytes*/) {
        onUserBytesReceivedCalls++;
    }
    
private:
    bool proxyConnected = false;
    bool connected = true;
};

#endif  // BLE_CLIENT_H
