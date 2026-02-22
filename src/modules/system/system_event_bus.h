#pragma once

#include <stddef.h>
#include <stdint.h>
#ifdef UNIT_TEST
#include <atomic>
#else
#include <freertos/FreeRTOS.h>
#endif

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
        LockGuard guard(*this);
        head = 0;
        tail = 0;
        count = 0;
        publishCount = 0;
        dropCount = 0;
    }

    // Reset only counters while preserving any queued events.
    void resetStats() {
        LockGuard guard(*this);
        publishCount = 0;
        dropCount = 0;
    }

    // Non-blocking publish: never waits.
    // Overflow policy favors preserving low-rate control events by dropping the
    // oldest frame event first when available.
    // Returns true if no drop occurred for this publish.
    bool publish(const SystemEvent& event) {
        LockGuard guard(*this);
        bool dropped = false;
        if (count == kCapacity) {
            uint8_t dropOffset = 0;
            if (!findOldestFrameOffset(dropOffset)) {
                dropOffset = 0;  // No frame events present, drop oldest control event
            }
            removeAtOffset(dropOffset, nullptr);
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
        LockGuard guard(*this);
        if (count == 0) {
            return false;
        }

        out = ring[tail];
        tail = nextIndex(tail);
        count--;
        return true;
    }

    // Consume the oldest event matching the requested type while preserving
    // FIFO order for all remaining events.
    bool consumeByType(SystemEventType type, SystemEvent& out) {
        LockGuard guard(*this);
        if (count == 0) {
            return false;
        }

        uint8_t idx = tail;
        uint8_t matchOffset = 0xFF;
        for (uint8_t offset = 0; offset < count; ++offset) {
            if (ring[idx].type == type) {
                out = ring[idx];
                matchOffset = offset;
                break;
            }
            idx = nextIndex(idx);
        }

        if (matchOffset == 0xFF) {
            return false;
        }

        return removeAtOffset(matchOffset, &out);
    }

    uint32_t getPublishCount() const {
        LockGuard guard(*this);
        return publishCount;
    }
    uint32_t getDropCount() const {
        LockGuard guard(*this);
        return dropCount;
    }
    size_t size() const {
        LockGuard guard(*this);
        return count;
    }

private:
    void lock() const {
#ifdef UNIT_TEST
        while (lockFlag.test_and_set(std::memory_order_acquire)) {
        }
#else
        portENTER_CRITICAL(&lockMux);
#endif
    }

    void unlock() const {
#ifdef UNIT_TEST
        lockFlag.clear(std::memory_order_release);
#else
        portEXIT_CRITICAL(&lockMux);
#endif
    }

    struct LockGuard {
        explicit LockGuard(const SystemEventBus& ownerRef) : owner(ownerRef) {
            owner.lock();
        }
        ~LockGuard() {
            owner.unlock();
        }
        const SystemEventBus& owner;
    };

    static bool isFrameEventType(SystemEventType type) {
        return type == SystemEventType::BLE_FRAME_PARSED;
    }

    static uint8_t nextIndex(uint8_t i) {
        return static_cast<uint8_t>((i + 1u) % kCapacity);
    }

    bool removeAtOffset(uint8_t offset, SystemEvent* removed) {
        if (offset >= count) {
            return false;
        }

        uint8_t idx = static_cast<uint8_t>((tail + offset) % kCapacity);
        if (removed) {
            *removed = ring[idx];
        }

        for (uint8_t pos = offset; pos + 1u < count; ++pos) {
            uint8_t dst = static_cast<uint8_t>((tail + pos) % kCapacity);
            uint8_t src = static_cast<uint8_t>((tail + pos + 1u) % kCapacity);
            ring[dst] = ring[src];
        }

        head = static_cast<uint8_t>((head + kCapacity - 1u) % kCapacity);
        count--;
        return true;
    }

    bool findOldestFrameOffset(uint8_t& offsetOut) const {
        uint8_t idx = tail;
        for (uint8_t offset = 0; offset < count; ++offset) {
            if (isFrameEventType(ring[idx].type)) {
                offsetOut = offset;
                return true;
            }
            idx = nextIndex(idx);
        }
        return false;
    }

    SystemEvent ring[kCapacity] = {};
    uint8_t head = 0;
    uint8_t tail = 0;
    uint8_t count = 0;
    uint32_t publishCount = 0;
    uint32_t dropCount = 0;
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux = portMUX_INITIALIZER_UNLOCKED;
#endif
};
