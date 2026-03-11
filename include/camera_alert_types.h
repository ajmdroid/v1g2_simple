#pragma once

#include <stdint.h>

enum class CameraType : uint8_t {
    INVALID = 0,
    ALPR = 4,
};

inline CameraType cameraTypeFromFlags(uint8_t flags) {
    return flags == static_cast<uint8_t>(CameraType::ALPR)
        ? CameraType::ALPR
        : CameraType::INVALID;
}

inline const char* cameraTypeDisplayLabel(CameraType type) {
    switch (type) {
        case CameraType::ALPR:
            return "ALPR";
        case CameraType::INVALID:
        default:
            return "";
    }
}

inline const char* cameraTypeApiName(CameraType type) {
    switch (type) {
        case CameraType::ALPR:
            return "alpr";
        case CameraType::INVALID:
        default:
            return nullptr;
    }
}

struct CameraAlertContext {
    bool gpsValid = false;
    int32_t latE5 = 0;
    int32_t lonE5 = 0;
    float speedMph = 0.0f;
    bool courseValid = false;
    float courseDeg = 0.0f;
    uint32_t courseAgeMs = 0;
};

struct CameraAlertDisplayPayload {
    bool active = false;
    uint32_t distanceCm = UINT32_MAX;
};
