#pragma once
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
    bool bootReadyFlag = true;  // Default true to preserve existing test behavior
    
    void reset() {
        proxyConnected = false;
        connected = true;
        setMuteCalls = 0;
        lastMuteValue = false;
        setVolumeCalls = 0;
        lastVolume = 0;
        lastMuteVolume = 0;
        requestAlertDataCalls = 0;
        bootReadyFlag = true;
    }
    
    // Connection state
    bool isProxyClientConnected() const { return proxyConnected; }
    void setProxyConnected(bool v) { proxyConnected = v; }
    bool isConnected() const { return connected; }
    void setConnected(bool v) { connected = v; }
    void setBootReady(bool ready) { bootReadyFlag = ready; }
    bool isBootReady() const { return bootReadyFlag; }
    
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
    
private:
    bool proxyConnected = false;
    bool connected = true;
};
