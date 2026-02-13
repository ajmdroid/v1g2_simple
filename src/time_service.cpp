#include "time_service.h"
#include <Preferences.h>
#include <cerrno>
#include <cstdlib>
#include <sys/time.h>

namespace {
constexpr const char* TIME_NS = "v1time";
constexpr const char* KEY_VALID = "valid";
constexpr const char* KEY_EPOCH_MS = "epochMs";
constexpr const char* KEY_TZ_OFFSET = "tzOffset";
constexpr const char* KEY_SOURCE = "source";

constexpr int64_t MIN_VALID_UNIX_MS = 1700000000000LL;  // ~2023-11
constexpr int64_t MAX_VALID_UNIX_MS = 4102444800000LL;  // 2100-01-01
constexpr int64_t MIN_PERSIST_REBASE_DELTA_MS = 5LL * 60LL * 1000LL;  // 5 minutes

struct PersistedTime {
    bool valid = false;
    int64_t epochMs = 0;
    int32_t tzOffsetMin = 0;
    uint8_t source = TimeService::SOURCE_CLIENT_AP;
};

bool isValidUnixMs(int64_t epochMs) {
    return epochMs >= MIN_VALID_UNIX_MS && epochMs <= MAX_VALID_UNIX_MS;
}

int32_t clampTzOffset(int32_t tzOffsetMin) {
    if (tzOffsetMin < -840) return -840;
    if (tzOffsetMin > 840) return 840;
    return tzOffsetMin;
}

bool parseInt64Strict(const String& input, int64_t& out) {
    if (input.length() == 0) {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const char* raw = input.c_str();
    long long value = strtoll(raw, &end, 10);
    if (errno == ERANGE || end == raw || *end != '\0') {
        return false;
    }
    out = static_cast<int64_t>(value);
    return true;
}

bool normalizeSource(uint8_t& source) {
    if (source == TimeService::SOURCE_NONE) {
        source = TimeService::SOURCE_CLIENT_AP;
        return true;
    }
    if (source > TimeService::SOURCE_RTC) {
        return false;
    }
    return true;
}

bool hasMaterialPersistChange(bool hadValid,
                              int64_t previousEpochMs,
                              int32_t previousTzOffsetMin,
                              uint8_t previousSource,
                              int64_t newEpochMs,
                              int32_t newTzOffsetMin,
                              uint8_t newSource) {
    if (!hadValid || !isValidUnixMs(previousEpochMs)) {
        return true;
    }
    if (previousTzOffsetMin != newTzOffsetMin) {
        return true;
    }
    if (previousSource != newSource) {
        return true;
    }
    return llabs(newEpochMs - previousEpochMs) >= MIN_PERSIST_REBASE_DELTA_MS;
}

void clearPersistedTimeSnapshot() {
    Preferences prefs;
    if (!prefs.begin(TIME_NS, false)) {
        return;
    }
    prefs.putBool(KEY_VALID, false);
    prefs.remove(KEY_EPOCH_MS);
    prefs.remove(KEY_TZ_OFFSET);
    prefs.remove(KEY_SOURCE);
    prefs.end();
}

bool savePersistedTimeSnapshot(const PersistedTime& snapshot) {
    Preferences prefs;
    if (!prefs.begin(TIME_NS, false)) {
        return false;
    }

    bool ok = true;
    if (!snapshot.valid) {
        ok &= prefs.putBool(KEY_VALID, false) > 0;
        prefs.remove(KEY_EPOCH_MS);
        prefs.remove(KEY_TZ_OFFSET);
        prefs.remove(KEY_SOURCE);
        prefs.end();
        return ok;
    }

    ok &= prefs.putBool(KEY_VALID, true) > 0;
    ok &= prefs.putString(KEY_EPOCH_MS, String(snapshot.epochMs)) > 0;
    ok &= prefs.putInt(KEY_TZ_OFFSET, clampTzOffset(snapshot.tzOffsetMin)) > 0;
    ok &= prefs.putUChar(KEY_SOURCE, snapshot.source) > 0;
    prefs.end();
    return ok;
}

bool loadPersistedTimeSnapshot(PersistedTime& snapshot) {
    snapshot = PersistedTime{};

    Preferences prefs;
    if (!prefs.begin(TIME_NS, true)) {
        return false;
    }

    const bool valid = prefs.getBool(KEY_VALID, false);
    if (!valid) {
        prefs.end();
        return false;
    }

    const String epochStr = prefs.getString(KEY_EPOCH_MS, "");
    int64_t epochMs = 0;
    if (!parseInt64Strict(epochStr, epochMs) || !isValidUnixMs(epochMs)) {
        prefs.end();
        clearPersistedTimeSnapshot();
        return false;
    }

    int32_t tzOffsetMin = prefs.getInt(KEY_TZ_OFFSET, 0);
    uint8_t source = prefs.getUChar(KEY_SOURCE, TimeService::SOURCE_CLIENT_AP);
    prefs.end();

    if (!normalizeSource(source)) {
        clearPersistedTimeSnapshot();
        return false;
    }

    snapshot.valid = true;
    snapshot.epochMs = epochMs;
    snapshot.tzOffsetMin = clampTzOffset(tzOffsetMin);
    snapshot.source = source;
    return true;
}

bool readSystemClockMs(int64_t& epochMsOut) {
    struct timeval tv = {};
    if (gettimeofday(&tv, nullptr) != 0) {
        return false;
    }

    if (tv.tv_sec < 0 || tv.tv_usec < 0) {
        return false;
    }

    const int64_t epochMs = static_cast<int64_t>(tv.tv_sec) * 1000LL
                          + static_cast<int64_t>(tv.tv_usec / 1000);
    if (!isValidUnixMs(epochMs)) {
        return false;
    }

    epochMsOut = epochMs;
    return true;
}

void setSystemClockIfValid(int64_t epochMs) {
    if (!isValidUnixMs(epochMs)) {
        return;
    }

    struct timeval tv = {};
    tv.tv_sec = static_cast<time_t>(epochMs / 1000LL);
    tv.tv_usec = static_cast<suseconds_t>((epochMs % 1000LL) * 1000LL);
    settimeofday(&tv, nullptr);
}
}  // namespace

TimeService timeService;

void TimeService::begin() {
    if (initialized_.exchange(1, std::memory_order_acq_rel) != 0) {
        return;
    }

    PersistedTime snapshot;
    const bool havePersistedSnapshot = loadPersistedTimeSnapshot(snapshot);

    int64_t systemEpochMs = 0;
    if (readSystemClockMs(systemEpochMs)) {
        const int32_t tzOffsetMin = havePersistedSnapshot ? snapshot.tzOffsetMin : 0;
        const uint32_t monoNow = nowMonoMs();
        const int64_t base = systemEpochMs - static_cast<int64_t>(monoNow);

        epochBaseMs_.store(base, std::memory_order_relaxed);
        setMonoMs_.store(monoNow, std::memory_order_relaxed);
        tzOffsetMinutes_.store(clampTzOffset(tzOffsetMin), std::memory_order_relaxed);
        source_.store(static_cast<uint8_t>(SOURCE_RTC), std::memory_order_relaxed);
        confidence_.store(CONFIDENCE_ACCURATE, std::memory_order_relaxed);
        valid_.store(1, std::memory_order_release);

        Serial.printf("[Time] Restored from system clock: epoch=%lld, tz=%d\n",
                      (long long)systemEpochMs, (int)tzOffsetMin);

        PersistedTime updatedSnapshot;
        updatedSnapshot.valid = true;
        updatedSnapshot.epochMs = systemEpochMs;
        updatedSnapshot.tzOffsetMin = clampTzOffset(tzOffsetMin);
        updatedSnapshot.source = static_cast<uint8_t>(SOURCE_RTC);
        savePersistedTimeSnapshot(updatedSnapshot);
        return;
    }

    if (havePersistedSnapshot) {
        const uint32_t monoNow = nowMonoMs();
        const int64_t base = snapshot.epochMs - static_cast<int64_t>(monoNow);

        epochBaseMs_.store(base, std::memory_order_relaxed);
        setMonoMs_.store(monoNow, std::memory_order_relaxed);
        tzOffsetMinutes_.store(snapshot.tzOffsetMin, std::memory_order_relaxed);
        source_.store(snapshot.source, std::memory_order_relaxed);
        confidence_.store(CONFIDENCE_ESTIMATED, std::memory_order_relaxed);
        valid_.store(1, std::memory_order_release);

        // Set the system clock so the RTC survives deep sleep with a reasonable value.
        setSystemClockIfValid(snapshot.epochMs);

        Serial.printf("[Time] Restored from NVS (estimated): epoch=%lld, tz=%d\n",
                      (long long)snapshot.epochMs, (int)snapshot.tzOffsetMin);
        return;
    }

    valid_.store(0, std::memory_order_release);
    source_.store(SOURCE_NONE, std::memory_order_relaxed);
    confidence_.store(CONFIDENCE_NONE, std::memory_order_relaxed);
    tzOffsetMinutes_.store(0, std::memory_order_relaxed);
    epochBaseMs_.store(0, std::memory_order_relaxed);
    setMonoMs_.store(0, std::memory_order_relaxed);

    Serial.println("[Time] No valid time source at boot - waiting for client sync");
}

int64_t TimeService::nowEpochMsOr0() const {
    if (valid_.load(std::memory_order_acquire) == 0) {
        return 0;
    }

    const int64_t base = epochBaseMs_.load(std::memory_order_relaxed);
    const int64_t mono = static_cast<int64_t>(nowMonoMs());
    const int64_t epoch = base + mono;
    if (epoch < 0) {
        return 0;
    }
    return epoch;
}

uint32_t TimeService::epochAgeMsOr0() const {
    if (valid_.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    const uint32_t now = nowMonoMs();
    const uint32_t setMono = setMonoMs_.load(std::memory_order_relaxed);
    return now - setMono;
}

void TimeService::setEpochBaseMs(int64_t trustedEpochMs, int32_t tzOffsetMinutes, Source source) {
    if (!isValidUnixMs(trustedEpochMs)) {
        clear();
        return;
    }
    if (source == SOURCE_NONE || source > SOURCE_RTC) {
        source = SOURCE_CLIENT_AP;
    }

    const bool hadValid = valid_.load(std::memory_order_acquire) != 0;
    const int64_t previousEpochMs = hadValid ? nowEpochMsOr0() : 0;
    const int32_t previousTzOffsetMinutes = tzOffsetMinutes_.load(std::memory_order_relaxed);
    const uint8_t previousSource = source_.load(std::memory_order_relaxed);

    const int32_t clampedTzOffsetMinutes = clampTzOffset(tzOffsetMinutes);
    const uint32_t monoNow = nowMonoMs();
    const int64_t base = trustedEpochMs - static_cast<int64_t>(monoNow);

    epochBaseMs_.store(base, std::memory_order_relaxed);
    setMonoMs_.store(monoNow, std::memory_order_relaxed);
    tzOffsetMinutes_.store(clampedTzOffsetMinutes, std::memory_order_relaxed);
    source_.store(static_cast<uint8_t>(source), std::memory_order_relaxed);
    confidence_.store(CONFIDENCE_ACCURATE, std::memory_order_relaxed);
    valid_.store(1, std::memory_order_release);
    initialized_.store(1, std::memory_order_release);

    setSystemClockIfValid(trustedEpochMs);

    Serial.printf("[Time] Set from source %u: epoch=%lld, tz=%d\n",
                  (unsigned)source, (long long)trustedEpochMs, (int)clampedTzOffsetMinutes);

    if (hasMaterialPersistChange(hadValid,
                                 previousEpochMs,
                                 previousTzOffsetMinutes,
                                 previousSource,
                                 trustedEpochMs,
                                 clampedTzOffsetMinutes,
                                 static_cast<uint8_t>(source))) {
        PersistedTime snapshot;
        snapshot.valid = true;
        snapshot.epochMs = trustedEpochMs;
        snapshot.tzOffsetMin = clampedTzOffsetMinutes;
        snapshot.source = static_cast<uint8_t>(source);
        savePersistedTimeSnapshot(snapshot);
    }
}

void TimeService::clear() {
    valid_.store(0, std::memory_order_release);
    source_.store(SOURCE_NONE, std::memory_order_relaxed);
    confidence_.store(CONFIDENCE_NONE, std::memory_order_relaxed);
    tzOffsetMinutes_.store(0, std::memory_order_relaxed);
    epochBaseMs_.store(0, std::memory_order_relaxed);
    setMonoMs_.store(0, std::memory_order_relaxed);
    initialized_.store(1, std::memory_order_release);
    clearPersistedTimeSnapshot();
}

void TimeService::persistCurrentTime() {
    const int64_t epoch = nowEpochMsOr0();
    if (epoch == 0) {
        return;  // No valid time to persist
    }

    PersistedTime snapshot;
    snapshot.valid = true;
    snapshot.epochMs = epoch;
    snapshot.tzOffsetMin = tzOffsetMinutes_.load(std::memory_order_relaxed);
    snapshot.source = source_.load(std::memory_order_relaxed);

    if (savePersistedTimeSnapshot(snapshot)) {
        Serial.printf("[Time] Persisted current time to NVS: epoch=%lld\n", (long long)epoch);
    }
}

void TimeService::periodicSave(uint32_t nowMs) {
    if (valid_.load(std::memory_order_acquire) == 0) {
        return;
    }
    if (nowMs - lastPeriodicSaveMs_ < PERIODIC_SAVE_INTERVAL_MS) {
        return;
    }
    lastPeriodicSaveMs_ = nowMs;
    persistCurrentTime();
}
