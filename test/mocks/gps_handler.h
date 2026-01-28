#pragma once

class GPSHandler {
public:
    bool hasValidFix() const { return validFix; }
    float getSpeed() const { return speedMps; }  // meters per second

    void setValidFix(bool v) { validFix = v; }
    void setSpeedMps(float v) { speedMps = v; }

private:
    bool validFix = false;
    float speedMps = 0.0f;
};

