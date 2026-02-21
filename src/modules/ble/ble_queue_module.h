#pragma once

#include <Arduino.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "modules/display/display_preview_module.h"
#include "modules/power/power_module.h"
#include "v1_profiles.h"

class SystemEventBus;

class BleQueueModule {
public:
    struct Config {
        size_t queueDepth;
        size_t rxBufferCap;
        size_t rxTrimKeep;
        Config() : queueDepth(48), rxBufferCap(512), rxTrimKeep(128) {}
    };

    void begin(V1BLEClient* bleClient,
               PacketParser* parser,
               V1ProfileManager* profileMgr,
               DisplayPreviewModule* previewModule,
               PowerModule* powerModule,
               SystemEventBus* eventBus = nullptr,
               Config cfg = Config());

    // Returns timestamp of last successfully parsed packet (for display latency tracking)
    uint32_t getLastParsedTimestamp() const { return lastParsedTsMs; }
    
    // Returns true if a packet was successfully parsed since last check (and clears flag)
    bool consumeParsedFlag() { bool had = hadSuccessfulParse; hadSuccessfulParse = false; return had; }

    // Callback entry from BLE notifications.
    void onNotify(const uint8_t* data, size_t length, uint16_t charUUID);

    // Drain queue, frame packets, parse, and forward to display pipeline.
    void process();

    unsigned long getLastRxMillis() const { return lastRxMillis; }

private:
    struct BLEDataPacket {
        uint8_t data[256];
        size_t length;
        uint16_t charUUID;
        uint32_t tsMs;
    };

    V1BLEClient* ble = nullptr;
    PacketParser* parser = nullptr;
    V1ProfileManager* profiles = nullptr;
    DisplayPreviewModule* preview = nullptr;
    PowerModule* power = nullptr;
    SystemEventBus* bus = nullptr;
    QueueHandle_t queueHandle = nullptr;

    std::vector<uint8_t> rxBuffer;
    size_t rxReadPos = 0;  // Logical read pointer into rxBuffer (avoids front erases)
    unsigned long lastRxMillis = 0;
    uint32_t lastNotifyTsMs = 0;
    uint32_t lastParsedTsMs = 0;      // Timestamp of last successful parse (for display latency)
    bool hadSuccessfulParse = false;  // Flag: at least one packet parsed since last check
    uint32_t parsedEventSeq = 0;

    Config config;

#ifdef REPLAY_MODE
    void processReplayData();
    unsigned long lastReplayTime = 0;
    size_t replayIndex = 0;
#endif
};
