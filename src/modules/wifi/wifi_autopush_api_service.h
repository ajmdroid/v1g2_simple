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

struct PushNowRequest {
    int slot = 0;
    bool hasProfileOverride = false;
    String profileName;
    bool hasModeOverride = false;
    int mode = 0;
};

enum class PushNowQueueResult : uint8_t {
    QUEUED = 0,
    V1_NOT_CONNECTED,
    ALREADY_IN_PROGRESS,
    NO_PROFILE_CONFIGURED,
    PROFILE_LOAD_FAILED,
};

struct Runtime {
    std::function<void(SlotsSnapshot&)> loadSlotsSnapshot;
    std::function<bool(String&)> loadPushStatusJson;
    std::function<void(int, const String&)> setSlotName;
    std::function<void(int, uint16_t)> setSlotColor;
    std::function<uint8_t(int)> getSlotVolume;
    std::function<uint8_t(int)> getSlotMuteVolume;
    std::function<void(int, uint8_t, uint8_t)> setSlotVolumes;
    std::function<void(int, bool)> setSlotDarkMode;
    std::function<void(int, bool)> setSlotMuteToZero;
    std::function<void(int, uint8_t)> setSlotAlertPersistSec;
    std::function<void(int, bool)> setSlotPriorityArrowOnly;
    std::function<void(int, const String&, int)> setSlotProfileAndMode;
    std::function<int()> getActiveSlot;
    std::function<void(int)> drawProfileIndicator;
    std::function<void(int)> setActiveSlot;
    std::function<void(bool)> setAutoPushEnabled;
    std::function<PushNowQueueResult(const PushNowRequest&)> queuePushNow;
};

void handleApiSlots(WebServer& server, const Runtime& runtime);

void handleApiStatus(WebServer& server, const Runtime& runtime);

void handleApiSlotSave(WebServer& server,
                       const Runtime& runtime,
                       const std::function<bool()>& checkRateLimit);

void handleApiActivate(WebServer& server,
                       const Runtime& runtime,
                       const std::function<bool()>& checkRateLimit);

void handleApiPushNow(WebServer& server,
                      const Runtime& runtime,
                      const std::function<bool()>& checkRateLimit);

}  // namespace WifiAutoPushApiService
