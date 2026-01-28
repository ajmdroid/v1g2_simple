#pragma once
#include <stdint.h>
class V1BLEClient {
public:
    bool isProxyClientConnected() const { return proxyConnected; }
    void setProxyConnected(bool v) { proxyConnected = v; }
    bool setVolume(uint8_t /*vol*/, uint8_t /*muteVol*/) { return true; }
    bool isConnected() const { return connected; }
    void setConnected(bool v) { connected = v; }
private:
    bool proxyConnected = false;
    bool connected = true;
};
