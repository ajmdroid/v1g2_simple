#pragma once

#ifndef WIFI_H
#define WIFI_H

#include "Arduino.h"

// Minimal WiFi stub for native tests
class WiFiClass {
public:
    uint16_t softAPgetStationNum() const { return apStationCount_; }
    void setApStationCount(uint16_t n) { apStationCount_ = n; }
private:
    uint16_t apStationCount_ = 0;
};

extern WiFiClass WiFi;

#endif  // WIFI_H
