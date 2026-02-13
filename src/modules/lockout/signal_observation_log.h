#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>
#ifdef UNIT_TEST
#include <atomic>
#else
#include <freertos/FreeRTOS.h>
#endif

// Compact signal observation used for lockout learning scaffolding.
// Lat/Lon are fixed-point (E5) to avoid float storage overhead in the ring.
struct SignalObservation {
    static constexpr uint16_t HDOP_X10_INVALID = UINT16_MAX;

    uint32_t tsMs = 0;
    uint8_t bandRaw = 0;
    uint8_t strength = 0;
    uint16_t frequencyMHz = 0;
    bool hasFix = false;
    uint32_t fixAgeMs = UINT32_MAX;
    bool locationValid = false;
    int32_t latitudeE5 = 0;
    int32_t longitudeE5 = 0;
    uint8_t satellites = 0;
    uint16_t hdopX10 = HDOP_X10_INVALID;
};

struct SignalObservationLogStats {
    uint32_t published = 0;
    uint32_t drops = 0;
    size_t size = 0;
};

class SignalObservationLog {
public:
    // Fixed capacity keeps memory bounded and deterministic.
    static constexpr size_t kCapacity = 256;

    void reset();
    bool publish(const SignalObservation& observation);
    size_t copyRecent(SignalObservation* out, size_t maxCount) const;
    SignalObservationLogStats stats() const;

private:
    void lock() const;
    void unlock() const;

    struct LockGuard {
        explicit LockGuard(const SignalObservationLog& ownerRef) : owner(ownerRef) {
            owner.lock();
        }
        ~LockGuard() {
            owner.unlock();
        }
        const SignalObservationLog& owner;
    };

    static uint16_t nextIndex(uint16_t idx) {
        return static_cast<uint16_t>((idx + 1u) % kCapacity);
    }

    static uint16_t prevIndex(uint16_t idx) {
        return static_cast<uint16_t>((idx + kCapacity - 1u) % kCapacity);
    }

    SignalObservation ring_[kCapacity] = {};
    uint16_t head_ = 0;
    uint16_t tail_ = 0;
    uint16_t count_ = 0;
    uint32_t published_ = 0;
    uint32_t drops_ = 0;
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag_ = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};

extern SignalObservationLog signalObservationLog;
