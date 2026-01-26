/**
 * Fixed-Size Event Ring Buffer Implementation
 */

#include "event_ring.h"
#include <ArduinoJson.h>

// Global ring buffer (static allocation, no heap)
Event eventRing[EVENT_RING_SIZE];
volatile uint32_t eventRingHead = 0;
volatile uint32_t eventRingCount = 0;
portMUX_TYPE eventRingMux = portMUX_INITIALIZER_UNLOCKED;

void eventRingInit() {
    eventRingHead = 0;
    eventRingCount = 0;
    memset(eventRing, 0, sizeof(eventRing));
}

void eventRingClear() {
    eventRingHead = 0;
    eventRingCount = 0;
}

const char* eventTypeName(EventType type) {
    switch (type) {
        case EVT_NONE:           return "NONE";
        case EVT_BLE_NOTIFY:     return "BLE_NOTIFY";
        case EVT_BLE_QUEUE_FULL: return "BLE_QUEUE_FULL";
        case EVT_BLE_CONNECT:    return "BLE_CONNECT";
        case EVT_BLE_DISCONNECT: return "BLE_DISCONNECT";
        case EVT_BLE_RECONNECT:  return "BLE_RECONNECT";
        case EVT_PARSE_OK:       return "PARSE_OK";
        case EVT_PARSE_FAIL:     return "PARSE_FAIL";
        case EVT_PARSE_RESYNC:   return "PARSE_RESYNC";
        case EVT_DISPLAY_UPDATE: return "DISPLAY_UPDATE";
        case EVT_DISPLAY_SKIP:   return "DISPLAY_SKIP";
        case EVT_DISPLAY_FLUSH:  return "DISPLAY_FLUSH";
        case EVT_ALERT_NEW:      return "ALERT_NEW";
        case EVT_ALERT_CLEAR:    return "ALERT_CLEAR";
        case EVT_MUTE_ON:        return "MUTE_ON";
        case EVT_MUTE_OFF:       return "MUTE_OFF";
        case EVT_PUSH_START:     return "PUSH_START";
        case EVT_PUSH_CMD:       return "PUSH_CMD";
        case EVT_PUSH_OK:        return "PUSH_OK";
        case EVT_PUSH_FAIL:      return "PUSH_FAIL";
        case EVT_WIFI_CONNECT:   return "WIFI_CONNECT";
        case EVT_WIFI_DISCONNECT:return "WIFI_DISCONNECT";
        case EVT_WIFI_AP_START:  return "WIFI_AP_START";
        case EVT_WIFI_AP_STOP:   return "WIFI_AP_STOP";
        case EVT_HEAP_LOW:       return "HEAP_LOW";
        case EVT_LATENCY_SPIKE:  return "LATENCY_SPIKE";
        case EVT_SLOW_LOOP:      return "SLOW_LOOP";
        case EVT_SLOW_DRAW:      return "SLOW_DRAW";
        case EVT_SLOW_PROXY:     return "SLOW_PROXY";
        case EVT_SLOW_PARSE:     return "SLOW_PARSE";
        case EVT_SETUP_MODE_ENTER: return "SETUP_ENTER";
        case EVT_SETUP_MODE_EXIT:  return "SETUP_EXIT";
        default:                 return "UNKNOWN";
    }
}

void eventRingDump() {
    eventRingDumpLast(EVENT_RING_SIZE);
}

void eventRingDumpLast(uint32_t maxCount) {
    // Calculate how many valid events to show
    uint32_t available = eventRingCount < EVENT_RING_SIZE ? eventRingCount : EVENT_RING_SIZE;
    uint32_t count = (maxCount < available) ? maxCount : available;
    
    Serial.printf("=== Events (last %lu of %lu, overflow=%s) ===\n",
        (unsigned long)count,
        (unsigned long)eventRingCount,
        eventRingHasOverflow() ? "YES" : "no");
    
    if (count == 0) {
        Serial.println("(no events)");
        return;
    }
    
    // Start from oldest of the events we want to show
    uint32_t startIdx = (eventRingHead - count) & (EVENT_RING_SIZE - 1);
    
    // Compact machine-parseable format: TIME,TYPE,DATA
    Serial.println("TIME_MS,TYPE,DATA");
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (startIdx + i) & (EVENT_RING_SIZE - 1);
        const Event& evt = eventRing[idx];
        if (evt.type != EVT_NONE) {
            Serial.printf("%lu,%s,%u\n",
                (unsigned long)evt.timestampMs,
                eventTypeName(evt.type),
                evt.data);
        }
    }
    Serial.println("=== END ===");
}

bool eventRingProcessCommand(const String& cmd) {
    // Check if command starts with "events"
    if (!cmd.startsWith("events")) {
        return false;
    }
    
    String args = cmd.substring(6);
    args.trim();
    
    if (args.length() == 0) {
        // "events" - dump all
        eventRingDump();
        return true;
    }
    
    if (args == "clear") {
        // "events clear" - clear buffer
        eventRingClear();
        Serial.println("Event ring cleared");
        return true;
    }
    
    if (args.startsWith("last ")) {
        // "events last N" - dump last N
        int n = args.substring(5).toInt();
        if (n > 0) {
            eventRingDumpLast(n);
        } else {
            Serial.println("Usage: events last <N>");
        }
        return true;
    }
    
    // Unknown subcommand
    Serial.println("Usage: events | events clear | events last <N>");
    return true;
}

String eventRingToJson() {
    JsonDocument doc;
    doc["totalEvents"] = eventRingCount;
    doc["overflow"] = eventRingHasOverflow();
    
    JsonArray events = doc["events"].to<JsonArray>();
    
    // Lock to prevent torn reads while iterating
    portENTER_CRITICAL(&eventRingMux);
    
    uint32_t count = eventRingCount < EVENT_RING_SIZE ? eventRingCount : EVENT_RING_SIZE;
    uint32_t startIdx = (eventRingHead - count) & (EVENT_RING_SIZE - 1);
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (startIdx + i) & (EVENT_RING_SIZE - 1);
        const Event& evt = eventRing[idx];
        if (evt.type != EVT_NONE) {
            JsonObject evtObj = events.add<JsonObject>();
            evtObj["t"] = evt.timestampMs;
            evtObj["type"] = eventTypeName(evt.type);
            evtObj["data"] = evt.data;
        }
    }
    
    portEXIT_CRITICAL(&eventRingMux);
    
    String json;
    serializeJson(doc, json);
    return json;
}
