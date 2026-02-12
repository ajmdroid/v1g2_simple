#pragma once
#include <cstdint>

struct OBDData {
    float voltage = 0.0f;
    int8_t oil_temp_c = -128;
    int8_t intake_air_temp_c = -128;
    bool valid = false;
    unsigned long timestamp_ms = 0;
};

class OBDHandler {
public:
    bool isModuleDetected() const { return moduleDetected; }
    bool hasValidData() const { return validData; }
    float getSpeedMph() const { return speedMph; }
    OBDData getData() const { return data; }

    void setModuleDetected(bool v) { moduleDetected = v; }
    void setValidData(bool v) { validData = v; }
    void setSpeedMph(float v) { speedMph = v; }
    void setData(const OBDData& d) { data = d; }

private:
    bool moduleDetected = false;
    bool validData = false;
    float speedMph = 0.0f;
    OBDData data{};
};
