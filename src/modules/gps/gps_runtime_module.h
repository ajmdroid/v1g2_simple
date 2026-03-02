#pragma once

#include <Arduino.h>

struct GpsRuntimeStatus {
    bool enabled = false;
    bool sampleValid = false;
    bool hasFix = false;
    bool stableHasFix = false;
    float speedMph = 0.0f;
    uint8_t satellites = 0;
    uint8_t stableSatellites = 0;
    float hdop = NAN;
    bool locationValid = false;
    float latitudeDeg = NAN;
    float longitudeDeg = NAN;
    bool courseValid = false;
    float courseDeg = NAN;
    uint32_t courseSampleTsMs = 0;
    uint32_t courseAgeMs = UINT32_MAX;
    uint32_t sampleTsMs = 0;
    uint32_t sampleAgeMs = UINT32_MAX;
    uint32_t fixAgeMs = UINT32_MAX;
    uint32_t stableFixAgeMs = UINT32_MAX;
    uint32_t injectedSamples = 0;
    bool moduleDetected = false;
    bool detectionTimedOut = false;
    bool parserActive = false;
    uint32_t hardwareSamples = 0;
    uint32_t bytesRead = 0;
    uint32_t sentencesSeen = 0;
    uint32_t sentencesParsed = 0;
    uint32_t parseFailures = 0;
    uint32_t checksumFailures = 0;
    uint32_t bufferOverruns = 0;
    uint32_t lastSentenceTsMs = 0;
};

class GpsRuntimeModule {
public:
    // Freshness budget for GPS sample telemetry.
    static constexpr uint32_t SAMPLE_MAX_AGE_MS = 3000;

    void begin(bool enabled);
    void setEnabled(bool enabled);
    bool isEnabled() const { return enabled_; }

    // Non-blocking UART/NMEA ingest with bounded per-loop processing.
    void update(uint32_t nowMs);

    // Manual sample injection path used by API/tools and test scaffolding.
    void setScaffoldSample(float speedMph,
                           bool hasFix,
                           uint8_t satellites,
                           float hdop,
                           uint32_t timestampMs,
                           float latitudeDeg = NAN,
                           float longitudeDeg = NAN,
                           float courseDeg = NAN);
    bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const;
    GpsRuntimeStatus snapshot(uint32_t nowMs) const;

    // Parse RMC UTC time + date into Unix epoch milliseconds.
    // Public static for testability.
    static bool parseRmcDateTime(const char* timeField, const char* dateField, int64_t& epochMsOut);

#ifdef UNIT_TEST
    // Test-only: clear all GPS sample state to simulate fix drop.
    void clearSample();

    // Native-test hook for parser coverage without UART hardware.
    bool injectNmeaSentenceForTest(const char* nmeaSentence, uint32_t nowMs);
#endif

private:
    static constexpr int GPS_RX_PIN = 3;
    static constexpr int GPS_TX_PIN = 1;
    static constexpr int GPS_EN_PIN = 2;
    static constexpr uint32_t GPS_BAUD = 9600;
    static constexpr uint32_t DETECTION_TIMEOUT_MS = 60000;
    static constexpr uint32_t FIX_STALE_MS = 15000;
    static constexpr uint32_t STABLE_FIX_HOLD_MS = 3000;
    static constexpr size_t NMEA_LINE_MAX = 128;
    static constexpr uint16_t MAX_BYTES_PER_UPDATE = 1024;
    static constexpr float KNOTS_TO_MPH = 1.150779f;
    static constexpr uint32_t GPS_TIME_UPDATE_INTERVAL_MS = 300000;  // Rebase time at most every 5 minutes

    void resetRuntimeState();
    void invalidateSpeedSample();
    void publishObservation(uint32_t timestampMs);
    void updateDetectionTimeout(uint32_t nowMs);
    void updateFixStaleness(uint32_t nowMs);
    void updateStableFixState(uint32_t nowMs);
    bool stableHasFixAt(uint32_t nowMs) const;
    void ingestByte(char c, uint32_t nowMs);
    bool processSentence(char* sentence, uint32_t nowMs);
    bool parseGga(char* fields[], size_t fieldCount, uint32_t nowMs);
    bool parseRmc(char* fields[], size_t fieldCount, uint32_t nowMs);
    static bool parseNmeaCoordinate(const char* coordText,
                                    const char* hemisphereText,
                                    bool isLatitude,
                                    float& outDegrees);
    static bool parseFloatStrict(const char* text, float& out);
    static bool parseUIntStrict(const char* text, uint32_t& out);
    static bool parseChecksum(const char* checksumText, uint8_t& out);
    static size_t splitCsv(char* payload, char* fields[], size_t maxFields);
    static bool sentenceTypeEquals(const char* type, const char* suffix);
    void tryUpdateGpsTime(const char* timeField, const char* dateField, uint32_t nowMs);

    bool enabled_ = false;
    bool sampleValid_ = false;
    bool hasFix_ = false;
    float speedMph_ = 0.0f;
    uint8_t satellites_ = 0;
    float hdop_ = NAN;
    bool locationValid_ = false;
    float latitudeDeg_ = NAN;
    float longitudeDeg_ = NAN;
    bool courseValid_ = false;
    float courseDeg_ = NAN;
    uint32_t courseSampleTsMs_ = 0;
    uint32_t sampleTsMs_ = 0;
    uint32_t injectedSamples_ = 0;
    bool moduleDetected_ = false;
    bool detectionTimedOut_ = false;
    bool parserActive_ = false;
    bool rmcFix_ = false;
    bool ggaFix_ = false;
    uint32_t detectionStartMs_ = 0;
    uint32_t lastFixTsMs_ = 0;
    uint32_t lastStableFixTsMs_ = 0;
    uint32_t lastSentenceTsMs_ = 0;
    uint32_t hardwareSamples_ = 0;
    uint32_t bytesRead_ = 0;
    uint32_t sentencesSeen_ = 0;
    uint32_t sentencesParsed_ = 0;
    uint32_t parseFailures_ = 0;
    uint32_t checksumFailures_ = 0;
    uint32_t bufferOverruns_ = 0;
    uint32_t lastGpsTimeUpdateMs_ = 0;
    uint32_t lastNoFixPublishMs_ = 0;  // Throttle: last no-fix observation publish timestamp
    uint8_t stableSatellites_ = 0;
    static constexpr uint32_t NO_FIX_PUBLISH_INTERVAL_MS = 5000;  // Publish no-fix at most every 5s
    bool sentenceActive_ = false;
    size_t sentenceLen_ = 0;
    char sentenceBuf_[NMEA_LINE_MAX] = {};
};

extern GpsRuntimeModule gpsRuntimeModule;
