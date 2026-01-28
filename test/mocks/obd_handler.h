#pragma once

class OBDHandler {
public:
    bool isModuleDetected() const { return moduleDetected; }
    bool hasValidData() const { return validData; }
    float getSpeedMph() const { return speedMph; }

    void setModuleDetected(bool v) { moduleDetected = v; }
    void setValidData(bool v) { validData = v; }
    void setSpeedMph(float v) { speedMph = v; }

private:
    bool moduleDetected = false;
    bool validData = false;
    float speedMph = 0.0f;
};

