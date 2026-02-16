#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <functional>

namespace WifiAutoPushApiService {

struct SlotConfig {
    String name;
    String profile;
    int mode = 0;
    uint16_t color = 0;
    uint8_t volume = 0;
    uint8_t muteVolume = 0;
    bool darkMode = false;
    bool muteToZero = false;
    uint8_t alertPersist = 0;
    bool priorityArrowOnly = false;
};

struct SlotsSnapshot {
    bool enabled = false;
    int activeSlot = 0;
    SlotConfig slots[3];
};

struct Runtime {
    std::function<void(SlotsSnapshot&)> loadSlotsSnapshot;
    std::function<bool(String&)> loadPushStatusJson;
};

void handleSlots(WebServer& server, const Runtime& runtime);

void handleStatus(WebServer& server, const Runtime& runtime);

}  // namespace WifiAutoPushApiService
