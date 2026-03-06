#pragma once

#include <stdint.h>

enum class CameraType : uint8_t {
    INVALID = 0,
    SPEED = 1,
    RED_LIGHT = 2,
    BUS_LANE = 3,
    ALPR = 4,
};

inline CameraType cameraTypeFromFlags(uint8_t flags) {
    switch (flags) {
        case 1:
            return CameraType::SPEED;
        case 2:
            return CameraType::RED_LIGHT;
        case 3:
            return CameraType::BUS_LANE;
        case 4:
            return CameraType::ALPR;
        default:
            return CameraType::INVALID;
    }
}

inline const char* cameraTypeDisplayLabel(CameraType type) {
    switch (type) {
        case CameraType::SPEED:
            return "SPEED";
        case CameraType::RED_LIGHT:
            return "RED LT";
        case CameraType::BUS_LANE:
            return "BUS LN";
        case CameraType::ALPR:
            return "ALPR";
        case CameraType::INVALID:
        default:
            return "";
    }
}

inline const char* cameraTypeClipName(CameraType type) {
    switch (type) {
        case CameraType::SPEED:
            return "cam_speed";
        case CameraType::RED_LIGHT:
            return "cam_red_light";
        case CameraType::BUS_LANE:
            return "cam_bus_lane";
        case CameraType::ALPR:
            return "cam_alpr";
        case CameraType::INVALID:
        default:
            return "";
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
    CameraType type = CameraType::INVALID;
    bool active = false;
    uint32_t distanceCm = UINT32_MAX;
};

struct CameraVoiceEvent {
    CameraType type = CameraType::INVALID;
    bool isNearStage = false;
};
