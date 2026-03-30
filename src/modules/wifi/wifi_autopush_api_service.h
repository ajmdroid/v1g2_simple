#pragma once

#include <Arduino.h>
#include <WebServer.h>

#include <cstdint>

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

struct SlotUpdateRequest {
    int slot = 0;
    bool hasName = false;
    String name;
    bool hasColor = false;
    uint16_t color = 0;
    bool hasVolume = false;
    uint8_t volume = 0;
    bool hasMuteVolume = false;
    uint8_t muteVolume = 0;
    bool hasDarkMode = false;
    bool darkMode = false;
    bool hasMuteToZero = false;
    bool muteToZero = false;
    bool hasAlertPersist = false;
    uint8_t alertPersist = 0;
    bool hasPriorityArrowOnly = false;
    bool priorityArrowOnly = false;
    String profile;
    int mode = 0;
};

struct ActivationRequest {
    int slot = 0;
    bool enable = true;
};

enum class PushNowQueueResult : uint8_t {
    QUEUED = 0,
    V1_NOT_CONNECTED,
    ALREADY_IN_PROGRESS,
    NO_PROFILE_CONFIGURED,
    PROFILE_LOAD_FAILED,
};

struct Runtime {
    void (*loadSlotsSnapshot)(SlotsSnapshot& snapshot, void* ctx);
    void* loadSlotsSnapshotCtx;
    bool (*loadPushStatusJson)(String& json, void* ctx);
    void* loadPushStatusJsonCtx;
    bool (*applySlotUpdate)(const SlotUpdateRequest& request, void* ctx);
    void* applySlotUpdateCtx;
    void (*setSlotName)(int slot, const String& name, void* ctx);
    void* setSlotNameCtx;
    void (*setSlotColor)(int slot, uint16_t color, void* ctx);
    void* setSlotColorCtx;
    uint8_t (*getSlotVolume)(int slot, void* ctx);
    void* getSlotVolumeCtx;
    uint8_t (*getSlotMuteVolume)(int slot, void* ctx);
    void* getSlotMuteVolumeCtx;
    void (*setSlotVolumes)(int slot, uint8_t volume, uint8_t muteVolume, void* ctx);
    void* setSlotVolumesCtx;
    void (*setSlotDarkMode)(int slot, bool darkMode, void* ctx);
    void* setSlotDarkModeCtx;
    void (*setSlotMuteToZero)(int slot, bool muteToZero, void* ctx);
    void* setSlotMuteToZeroCtx;
    void (*setSlotAlertPersistSec)(int slot, uint8_t alertPersistSec, void* ctx);
    void* setSlotAlertPersistSecCtx;
    void (*setSlotPriorityArrowOnly)(int slot, bool priorityArrowOnly, void* ctx);
    void* setSlotPriorityArrowOnlyCtx;
    void (*setSlotProfileAndMode)(int slot, const String& profile, int mode, void* ctx);
    void* setSlotProfileAndModeCtx;
    int (*getActiveSlot)(void* ctx);
    void* getActiveSlotCtx;
    void (*drawProfileIndicator)(int slot, void* ctx);
    void* drawProfileIndicatorCtx;
    bool (*applyActivation)(const ActivationRequest& request, void* ctx);
    void* applyActivationCtx;
    void (*setActiveSlot)(int slot, void* ctx);
    void* setActiveSlotCtx;
    void (*setAutoPushEnabled)(bool enabled, void* ctx);
    void* setAutoPushEnabledCtx;
    PushNowQueueResult (*queuePushNow)(const PushNowRequest& request, void* ctx);
    void* queuePushNowCtx;
};

void handleApiSlots(WebServer& server, const Runtime& runtime);

void handleApiStatus(WebServer& server, const Runtime& runtime);

void handleApiSlotSave(WebServer& server,
                       const Runtime& runtime,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiActivate(WebServer& server,
                       const Runtime& runtime,
                       bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

void handleApiPushNow(WebServer& server,
                      const Runtime& runtime,
                      bool (*checkRateLimit)(void* ctx), void* rateLimitCtx);

}  // namespace WifiAutoPushApiService
