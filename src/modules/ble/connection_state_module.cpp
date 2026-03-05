#include "connection_state_module.h"
#include "ble_client.h"
#include "packet_parser.h"
#include "display.h"
#include "modules/power/power_module.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/system/system_event_bus.h"
#include "debug_logger.h"

#if defined(DISABLE_DEBUG_LOGGER)
#define CONN_LOG(...) do { } while(0)
#else
#define CONN_LOG(...) do { \
    DBG_LOGF(DebugLogCategory::Ble, __VA_ARGS__); \
} while(0)
#endif

void ConnectionStateModule::begin(V1BLEClient* bleClient,
                                  PacketParser* parserPtr,
                                  V1Display* displayPtr,
                                  PowerModule* powerModule,
                                  BleQueueModule* bleQueueModule,
                                  SystemEventBus* eventBus) {
    ble = bleClient;
    parser = parserPtr;
    display = displayPtr;
    power = powerModule;
    bleQueue = bleQueueModule;
    bus = eventBus;
    wasConnected = false;
    lastDataRequestMs = 0;
}

bool ConnectionStateModule::process(unsigned long nowMs) {
    if (!ble || !parser || !display) {
        return false;
    }

    bool isConnected = ble->isConnected();

    // Handle state transitions
    if (isConnected != wasConnected) {
        if (power) {
            power->onV1ConnectionChange(isConnected);
        }

        if (isConnected) {
            // Just connected
            display->showResting();
            CONN_LOG("[BLE] V1 connected");
            if (bus) {
                SystemEvent event;
                event.type = SystemEventType::BLE_CONNECTED;
                event.tsMs = static_cast<uint32_t>(nowMs);
                bus->publish(event);
            }
        } else {
            // Just disconnected - reset stale state
            PacketParser::resetAlertCountTracker();
            parser->resetAlertAssembly();
            V1Display::resetChangeTracking();
            display->showScanning();
            CONN_LOG("[BLE] V1 disconnected - scanning");
            if (bus) {
                SystemEvent event;
                event.type = SystemEventType::BLE_DISCONNECTED;
                event.tsMs = static_cast<uint32_t>(nowMs);
                bus->publish(event);
            }
        }
        wasConnected = isConnected;
    }

    // If connected but not seeing traffic, periodically re-request alert data
    if (isConnected && bleQueue) {
        unsigned long lastRx = bleQueue->getLastRxMillis();
        bool dataStale = (nowMs - lastRx) > DATA_STALE_MS;
        bool canRequest = (nowMs - lastDataRequestMs) > DATA_REQUEST_INTERVAL_MS;

        if (dataStale && canRequest) {
            ble->requestAlertData();
            lastDataRequestMs = nowMs;
        }
    }

    // When disconnected, refresh indicators periodically
    if (!isConnected) {
        display->drawWiFiIndicator();
        display->drawBatteryIndicator();
        display->flush();
    }

    return isConnected;
}
