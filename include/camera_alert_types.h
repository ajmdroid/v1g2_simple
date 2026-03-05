#pragma once

#include <stdint.h>

enum class CameraType : uint8_t {
    SPEED = 1,
    RED_LIGHT = 2,
    BUS_LANE = 3,
    ALPR = 4,
};

struct CameraAlertDisplayPayload {
    CameraType type = CameraType::SPEED;
    uint16_t distanceCm = 0xFFFF;
};

struct CameraVoiceEvent {
    CameraType type = CameraType::SPEED;
    bool isNearStage = false;
};

inline bool cameraTypeFromFlags(uint8_t flags, CameraType& out) {
    switch (flags) {
        case 1:
            out = CameraType::SPEED;
            return true;
        case 2:
            out = CameraType::RED_LIGHT;
            return true;
        case 3:
            out = CameraType::BUS_LANE;
            return true;
        case 4:
            out = CameraType::ALPR;
            return true;
        default:
            return false;
    }
}

inline const char* cameraTypeStatusName(CameraType type) {
    switch (type) {
        case CameraType::SPEED:
            return "speed";
        case CameraType::RED_LIGHT:
            return "red_light";
        case CameraType::BUS_LANE:
            return "bus_lane";
        case CameraType::ALPR:
            return "alpr";
        default:
            return "unknown";
    }
}

inline const char* cameraTypeDisplayLabel(CameraType type) {
    switch (type) {
        case CameraType::SPEED:
            return "SPD CAM";
        case CameraType::RED_LIGHT:
            return "RED LT";
        case CameraType::BUS_LANE:
            return "BUS LN";
        case CameraType::ALPR:
            return "ALPR";
        default:
            return "CAM";
    }
}

inline const char* cameraTypeVoiceClip(CameraType type) {
    switch (type) {
        case CameraType::SPEED:
            return "cam_speed.mul";
        case CameraType::RED_LIGHT:
            return "cam_red_light.mul";
        case CameraType::BUS_LANE:
            return "cam_bus_lane.mul";
        case CameraType::ALPR:
            return "cam_alpr.mul";
        default:
            return nullptr;
    }
}
