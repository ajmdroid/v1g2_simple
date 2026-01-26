/**
 * Mock external dependencies for display testing
 * Provides stubs for BLE client, GPS handler, battery manager
 */
#pragma once

#include "Arduino.h"
#include <cstdint>

// V1 BLE Client stub
class V1BLEClient {
public:
    bool isConnected() const { return connected_; }
    bool hasProxyClient() const { return hasProxy_; }
    int getV1Rssi() const { return rssi_; }
    
    // Test helpers
    void setConnected(bool c) { connected_ = c; }
    void setHasProxyClient(bool p) { hasProxy_ = p; }
    void setRssi(int r) { rssi_ = r; }
    
private:
    bool connected_ = false;
    bool hasProxy_ = false;
    int rssi_ = -70;
};

// GPS Handler stub
class GPSHandler {
public:
    bool hasFix() const { return hasFix_; }
    int getSatelliteCount() const { return satellites_; }
    bool isEnabled() const { return enabled_; }
    float getLatitude() const { return lat_; }
    float getLongitude() const { return lon_; }
    float getSpeed() const { return speed_; }
    
    // Haversine distance calculation (real implementation for testing)
    static float haversineDistance(float lat1, float lon1, float lat2, float lon2) {
        constexpr float R = 6371000.0f; // Earth radius in meters
        float dLat = (lat2 - lat1) * 0.017453292519943295f;
        float dLon = (lon2 - lon1) * 0.017453292519943295f;
        float a = sin(dLat/2) * sin(dLat/2) +
                  cos(lat1 * 0.017453292519943295f) * cos(lat2 * 0.017453292519943295f) *
                  sin(dLon/2) * sin(dLon/2);
        float c = 2 * atan2(sqrt(a), sqrt(1-a));
        return R * c;
    }
    
    // Test helpers
    void setHasFix(bool f) { hasFix_ = f; }
    void setSatellites(int s) { satellites_ = s; }
    void setEnabled(bool e) { enabled_ = e; }
    void setPosition(float lat, float lon) { lat_ = lat; lon_ = lon; }
    void setSpeed(float s) { speed_ = s; }
    
private:
    bool hasFix_ = false;
    bool enabled_ = true;
    int satellites_ = 0;
    float lat_ = 0.0f;
    float lon_ = 0.0f;
    float speed_ = 0.0f;
};

// Battery Manager stub
class BatteryManager {
public:
    bool isOnBattery() const { return onBattery_; }
    int getBatteryPercent() const { return percent_; }
    float getBatteryVoltage() const { return voltage_; }
    bool isCharging() const { return charging_; }
    
    // Test helpers
    void setOnBattery(bool b) { onBattery_ = b; }
    void setBatteryPercent(int p) { percent_ = p; }
    void setVoltage(float v) { voltage_ = v; }
    void setCharging(bool c) { charging_ = c; }
    
private:
    bool onBattery_ = false;
    int percent_ = 100;
    float voltage_ = 4.2f;
    bool charging_ = false;
};

// OBD Handler stub
class OBDHandler {
public:
    bool isConnected() const { return connected_; }
    int getSpeed() const { return speed_; }
    
    // Test helpers
    void setConnected(bool c) { connected_ = c; }
    void setSpeed(int s) { speed_ = s; }
    
private:
    bool connected_ = false;
    int speed_ = 0;
};

// Global instances
extern V1BLEClient bleClient;
extern GPSHandler gpsHandler;
extern BatteryManager batteryManager;
extern OBDHandler obdHandler;

#endif // external_deps mock
