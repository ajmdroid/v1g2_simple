#pragma once

class V1BLEClient {
public:
    bool isProxyClientConnected() const { return proxyConnected; }
    void setProxyConnected(bool v) { proxyConnected = v; }

    // Stubbed setVolume for tests that might call it
    bool setVolume(uint8_t /*vol*/, uint8_t /*muteVol*/) { return true; }

private:
    bool proxyConnected = false;
};

