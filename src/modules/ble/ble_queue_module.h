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
#include "modules/display/display_pipeline_module.h"
#include "v1_profiles.h"

class BleQueueModule {
public:
    struct Config {
        size_t queueDepth;
        size_t rxBufferCap;
        size_t rxTrimKeep;
        Config() : queueDepth(72), rxBufferCap(512), rxTrimKeep(128) {}
    };

    void begin(V1BLEClient* bleClient,
               PacketParser* parser,
               V1ProfileManager* profileMgr,
               DisplayPreviewModule* previewModule,
               DisplayPipelineModule* displayPipeline,
               PowerModule* powerModule,
               Config cfg = Config());

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
    DisplayPipelineModule* displayPipeline = nullptr;
    PowerModule* power = nullptr;
    QueueHandle_t queueHandle = nullptr;

    std::vector<uint8_t> rxBuffer;
    unsigned long lastRxMillis = 0;

    Config config;

#ifdef REPLAY_MODE
    void processReplayData();
    unsigned long lastReplayTime = 0;
    size_t replayIndex = 0;
#endif

#ifdef SERIAL_REPLAY_MODE
    void processSerialReplay();
#endif
};
