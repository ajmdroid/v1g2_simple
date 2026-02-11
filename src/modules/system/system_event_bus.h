#pragma once

#include <stddef.h>
#include <stdint.h>

// Event types for low-overhead module decoupling.
// Keep payloads POD and fixed-size to avoid heap/format overhead in hot paths.
enum class SystemEventType : uint8_t {
    NONE = 0,
    BLE_FRAME_PARSED = 1,
    BLE_CONNECTED = 2,
    BLE_DISCONNECTED = 3,
    GPS_UPDATED = 16,
    OBD_UPDATED = 17,
};

struct SystemEvent {
    SystemEventType type = SystemEventType::NONE;
    uint32_t tsMs = 0;
    uint32_t seq = 0;
    uint16_t detail = 0;
};

class SystemEventBus {
public:
    static constexpr size_t kCapacity = 32;

    void reset() {
        head = 0;
        tail = 0;
        count = 0;
        publishCount = 0;
        dropCount = 0;
    }

    // Non-blocking publish: never waits; drops oldest when full.
    // Returns true if no drop occurred for this publish.
    bool publish(const SystemEvent& event) {
        bool dropped = false;
        if (count == kCapacity) {
            tail = nextIndex(tail);
            count--;
            dropCount++;
            dropped = true;
        }

        ring[head] = event;
        head = nextIndex(head);
        count++;
        publishCount++;

        return !dropped;
    }

    bool consume(SystemEvent& out) {
        if (count == 0) {
            return false;
        }

        out = ring[tail];
        tail = nextIndex(tail);
        count--;
        return true;
    }

    // Consume first event of the requested type while preserving all other events.
    bool consumeByType(SystemEventType type, SystemEvent& out) {
        if (count == 0) {
            return false;
        }

        uint8_t foundIndex = 0;
        bool found = false;
        for (uint8_t offset = 0; offset < count; ++offset) {
            uint8_t idx = static_cast<uint8_t>((tail + offset) % kCapacity);
            if (ring[idx].type == type) {
                foundIndex = idx;
                found = true;
                break;
            }
        }

        if (!found) {
            return false;
        }

        out = ring[foundIndex];

        if (foundIndex == tail) {
            tail = nextIndex(tail);
            count--;
            return true;
        }

        // Shift subsequent elements left one slot to close the gap.
        uint8_t cursor = foundIndex;
        const uint8_t lastElement = prevIndex(head);
        while (cursor != lastElement) {
            uint8_t next = nextIndex(cursor);
            ring[cursor] = ring[next];
            cursor = next;
        }

        head = lastElement;
        count--;
        return true;
    }

    uint32_t getPublishCount() const { return publishCount; }
    uint32_t getDropCount() const { return dropCount; }
    size_t size() const { return count; }

private:
    static uint8_t nextIndex(uint8_t i) {
        return static_cast<uint8_t>((i + 1u) % kCapacity);
    }

    static uint8_t prevIndex(uint8_t i) {
        return static_cast<uint8_t>((i + kCapacity - 1u) % kCapacity);
    }

    SystemEvent ring[kCapacity] = {};
    uint8_t head = 0;
    uint8_t tail = 0;
    uint8_t count = 0;
    uint32_t publishCount = 0;
    uint32_t dropCount = 0;
};
