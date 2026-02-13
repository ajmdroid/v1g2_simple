#include "gps_runtime_module.h"
#include "gps_observation_log.h"
#include "modules/speed/speed_source_selector.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

GpsRuntimeModule gpsRuntimeModule;

namespace {
bool elapsedExceeded(uint32_t nowMs, uint32_t startMs, uint32_t thresholdMs) {
    if (startMs == 0) {
        return false;
    }
    return static_cast<uint32_t>(nowMs - startMs) > thresholdMs;
}
}  // namespace

void GpsRuntimeModule::begin(bool enabled) {
    setEnabled(enabled);
}

void GpsRuntimeModule::setEnabled(bool enabled) {
    if (enabled_ == enabled) {
        return;
    }

    enabled_ = enabled;
#if !defined(UNIT_TEST)
    if (!enabled) {
        if (parserActive_) {
            Serial2.end();
        }
        pinMode(GPS_EN_PIN, OUTPUT);
        digitalWrite(GPS_EN_PIN, HIGH);
    } else {
        pinMode(GPS_EN_PIN, OUTPUT);
        digitalWrite(GPS_EN_PIN, LOW);
        Serial2.begin(GPS_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
    }
#endif

    resetRuntimeState();
    if (enabled_) {
        detectionStartMs_ = millis();
        if (detectionStartMs_ == 0) {
            detectionStartMs_ = 1;
        }
        parserActive_ = true;
    }
}

void GpsRuntimeModule::resetRuntimeState() {
    sampleValid_ = false;
    hasFix_ = false;
    speedMph_ = 0.0f;
    satellites_ = 0;
    hdop_ = NAN;
    locationValid_ = false;
    latitudeDeg_ = NAN;
    longitudeDeg_ = NAN;
    sampleTsMs_ = 0;
    moduleDetected_ = false;
    detectionTimedOut_ = false;
    parserActive_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    detectionStartMs_ = 0;
    lastFixTsMs_ = 0;
    lastSentenceTsMs_ = 0;
    hardwareSamples_ = 0;
    bytesRead_ = 0;
    sentencesSeen_ = 0;
    sentencesParsed_ = 0;
    parseFailures_ = 0;
    checksumFailures_ = 0;
    bufferOverruns_ = 0;
    sentenceActive_ = false;
    sentenceLen_ = 0;
    sentenceBuf_[0] = '\0';
    gpsObservationLog.reset();
}

void GpsRuntimeModule::invalidateSpeedSample() {
    sampleValid_ = false;
    speedMph_ = 0.0f;
    sampleTsMs_ = 0;
}

void GpsRuntimeModule::updateDetectionTimeout(uint32_t nowMs) {
    if (!enabled_ || moduleDetected_ || detectionTimedOut_ || detectionStartMs_ == 0) {
        return;
    }
    if (!elapsedExceeded(nowMs, detectionStartMs_, DETECTION_TIMEOUT_MS)) {
        return;
    }

    detectionTimedOut_ = true;
    parserActive_ = false;
    hasFix_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    satellites_ = 0;
    hdop_ = NAN;
    locationValid_ = false;
    latitudeDeg_ = NAN;
    longitudeDeg_ = NAN;
    lastFixTsMs_ = 0;
    invalidateSpeedSample();
    publishObservation(nowMs);

#if !defined(UNIT_TEST)
    Serial2.end();
    pinMode(GPS_EN_PIN, OUTPUT);
    digitalWrite(GPS_EN_PIN, HIGH);
#endif
}

void GpsRuntimeModule::updateFixStaleness(uint32_t nowMs) {
    if (!hasFix_ || lastFixTsMs_ == 0) {
        return;
    }
    if (!elapsedExceeded(nowMs, lastFixTsMs_, FIX_STALE_MS)) {
        return;
    }

    hasFix_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    satellites_ = 0;
    hdop_ = NAN;
    locationValid_ = false;
    latitudeDeg_ = NAN;
    longitudeDeg_ = NAN;
    lastFixTsMs_ = 0;
    invalidateSpeedSample();
    publishObservation(nowMs);
}

void GpsRuntimeModule::update(uint32_t nowMs) {
    if (!enabled_) {
        return;
    }

#if !defined(UNIT_TEST)
    if (parserActive_) {
        uint16_t processed = 0;
        while (processed < MAX_BYTES_PER_UPDATE && Serial2.available() > 0) {
            const int raw = Serial2.read();
            if (raw < 0) {
                break;
            }
            ingestByte(static_cast<char>(raw), nowMs);
            processed++;
        }
    }
#endif

    updateDetectionTimeout(nowMs);
    updateFixStaleness(nowMs);
}

void GpsRuntimeModule::ingestByte(char c, uint32_t nowMs) {
    bytesRead_++;

    if (c == '$') {
        sentenceActive_ = true;
        sentenceLen_ = 0;
        sentenceBuf_[sentenceLen_++] = c;
        return;
    }

    if (!sentenceActive_) {
        return;
    }

    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        sentenceBuf_[sentenceLen_] = '\0';
        if (sentenceLen_ > 0) {
            (void)processSentence(sentenceBuf_, nowMs);
        }
        sentenceActive_ = false;
        sentenceLen_ = 0;
        return;
    }

    if (sentenceLen_ >= (NMEA_LINE_MAX - 1)) {
        sentenceActive_ = false;
        sentenceLen_ = 0;
        bufferOverruns_++;
        return;
    }

    sentenceBuf_[sentenceLen_++] = c;
}

bool GpsRuntimeModule::processSentence(char* sentence, uint32_t nowMs) {
    if (!sentence || sentence[0] != '$') {
        parseFailures_++;
        return false;
    }

    sentencesSeen_++;
    lastSentenceTsMs_ = (nowMs == 0) ? millis() : nowMs;

    char* star = std::strchr(sentence, '*');
    if (!star || star <= (sentence + 1) || star[1] == '\0' || star[2] == '\0') {
        parseFailures_++;
        return false;
    }

    uint8_t expectedChecksum = 0;
    if (!parseChecksum(star + 1, expectedChecksum)) {
        parseFailures_++;
        return false;
    }

    uint8_t computedChecksum = 0;
    for (const char* p = sentence + 1; p < star; ++p) {
        computedChecksum ^= static_cast<uint8_t>(*p);
    }
    if (computedChecksum != expectedChecksum) {
        checksumFailures_++;
        parseFailures_++;
        return false;
    }

    moduleDetected_ = true;
    detectionTimedOut_ = false;

    *star = '\0';
    char* fields[20] = {};
    const size_t fieldCount = splitCsv(sentence + 1, fields, 20);
    if (fieldCount == 0 || !fields[0]) {
        parseFailures_++;
        return false;
    }

    bool ok = true;
    if (sentenceTypeEquals(fields[0], "RMC")) {
        ok = parseRmc(fields, fieldCount, nowMs);
    } else if (sentenceTypeEquals(fields[0], "GGA")) {
        ok = parseGga(fields, fieldCount, nowMs);
    }

    if (ok) {
        sentencesParsed_++;
    } else {
        parseFailures_++;
    }
    return ok;
}

bool GpsRuntimeModule::parseGga(char* fields[], size_t fieldCount, uint32_t nowMs) {
    if (!fields || fieldCount < 8) {
        return false;
    }

    uint32_t fixQuality = 0;
    if (fields[6] && fields[6][0] != '\0' && !parseUIntStrict(fields[6], fixQuality)) {
        return false;
    }

    uint32_t satelliteCount = 0;
    if (fields[7] && fields[7][0] != '\0' && !parseUIntStrict(fields[7], satelliteCount)) {
        return false;
    }
    satellites_ = static_cast<uint8_t>(std::min<uint32_t>(satelliteCount, 99));
    const bool ggaFix = (fixQuality > 0) && (satellites_ > 0);

    float parsedLatitude = NAN;
    float parsedLongitude = NAN;
    if (ggaFix) {
        if (!fields[2] || fields[2][0] == '\0' ||
            !fields[3] || fields[3][0] == '\0' ||
            !fields[4] || fields[4][0] == '\0' ||
            !fields[5] || fields[5][0] == '\0') {
            return false;
        }
        if (!parseNmeaCoordinate(fields[2], fields[3], true, parsedLatitude) ||
            !parseNmeaCoordinate(fields[4], fields[5], false, parsedLongitude)) {
            return false;
        }
    }

    float parsedHdop = NAN;
    if (fieldCount > 8 && fields[8] && fields[8][0] != '\0') {
        if (!parseFloatStrict(fields[8], parsedHdop) || parsedHdop < 0.0f) {
            return false;
        }
    }
    hdop_ = std::isfinite(parsedHdop) ? parsedHdop : NAN;

    ggaFix_ = ggaFix;
    hasFix_ = ggaFix_ || rmcFix_;
    if (ggaFix_) {
        locationValid_ = true;
        latitudeDeg_ = parsedLatitude;
        longitudeDeg_ = parsedLongitude;
    } else if (!rmcFix_) {
        locationValid_ = false;
        latitudeDeg_ = NAN;
        longitudeDeg_ = NAN;
    }
    if (hasFix_) {
        lastFixTsMs_ = (nowMs == 0) ? millis() : nowMs;
    } else {
        invalidateSpeedSample();
    }

    publishObservation((nowMs == 0) ? millis() : nowMs);

    return true;
}

bool GpsRuntimeModule::parseRmc(char* fields[], size_t fieldCount, uint32_t nowMs) {
    if (!fields || fieldCount < 8 || !fields[2]) {
        return false;
    }

    const char status = fields[2][0];
    if (status != 'A' && status != 'a') {
        rmcFix_ = false;
        hasFix_ = ggaFix_;
        if (!hasFix_) {
            invalidateSpeedSample();
            locationValid_ = false;
            latitudeDeg_ = NAN;
            longitudeDeg_ = NAN;
        }
        publishObservation((nowMs == 0) ? millis() : nowMs);
        return true;
    }

    if (!fields[3] || fields[3][0] == '\0' ||
        !fields[4] || fields[4][0] == '\0' ||
        !fields[5] || fields[5][0] == '\0' ||
        !fields[6] || fields[6][0] == '\0') {
        return false;
    }

    float parsedLatitude = NAN;
    float parsedLongitude = NAN;
    if (!parseNmeaCoordinate(fields[3], fields[4], true, parsedLatitude) ||
        !parseNmeaCoordinate(fields[5], fields[6], false, parsedLongitude)) {
        return false;
    }

    float speedKnots = 0.0f;
    if (fields[7] && fields[7][0] != '\0') {
        if (!parseFloatStrict(fields[7], speedKnots) || speedKnots < 0.0f) {
            return false;
        }
    }

    const float speedMph = speedKnots * KNOTS_TO_MPH;
    if (!std::isfinite(speedMph) || speedMph < 0.0f) {
        return false;
    }

    rmcFix_ = true;
    hasFix_ = true;
    sampleValid_ = true;
    speedMph_ = std::clamp(speedMph, 0.0f, SpeedSourceSelector::MAX_VALID_SPEED_MPH);
    sampleTsMs_ = (nowMs == 0) ? millis() : nowMs;
    lastFixTsMs_ = sampleTsMs_;
    locationValid_ = true;
    latitudeDeg_ = parsedLatitude;
    longitudeDeg_ = parsedLongitude;
    hardwareSamples_++;
    publishObservation(sampleTsMs_);

    return true;
}

bool GpsRuntimeModule::parseNmeaCoordinate(const char* coordText,
                                           const char* hemisphereText,
                                           bool isLatitude,
                                           float& outDegrees) {
    if (!coordText || coordText[0] == '\0' || !hemisphereText || hemisphereText[0] == '\0') {
        return false;
    }

    const char hemi = hemisphereText[0];
    bool negative = false;
    if (isLatitude) {
        if (hemi == 'S' || hemi == 's') {
            negative = true;
        } else if (hemi != 'N' && hemi != 'n') {
            return false;
        }
    } else {
        if (hemi == 'W' || hemi == 'w') {
            negative = true;
        } else if (hemi != 'E' && hemi != 'e') {
            return false;
        }
    }

    char* end = nullptr;
    const double raw = std::strtod(coordText, &end);
    if (end == coordText || *end != '\0' || !std::isfinite(raw) || raw < 0.0) {
        return false;
    }

    const double degreesPart = std::floor(raw / 100.0);
    const double minutesPart = raw - (degreesPart * 100.0);
    if (!std::isfinite(degreesPart) || !std::isfinite(minutesPart)) {
        return false;
    }
    if (minutesPart < 0.0 || minutesPart >= 60.0) {
        return false;
    }

    double decimalDegrees = degreesPart + (minutesPart / 60.0);
    const double maxDegrees = isLatitude ? 90.0 : 180.0;
    if (decimalDegrees > maxDegrees) {
        return false;
    }

    if (negative) {
        decimalDegrees = -decimalDegrees;
    }

    outDegrees = static_cast<float>(decimalDegrees);
    return std::isfinite(outDegrees);
}

bool GpsRuntimeModule::parseFloatStrict(const char* text, float& out) {
    if (!text || text[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(parsed)) {
        return false;
    }
    out = parsed;
    return true;
}

bool GpsRuntimeModule::parseUIntStrict(const char* text, uint32_t& out) {
    if (!text || text[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool GpsRuntimeModule::parseChecksum(const char* checksumText, uint8_t& out) {
    if (!checksumText || checksumText[0] == '\0' || checksumText[1] == '\0' || checksumText[2] != '\0') {
        return false;
    }

    auto nibble = [](char c, uint8_t& value) -> bool {
        if (c >= '0' && c <= '9') {
            value = static_cast<uint8_t>(c - '0');
            return true;
        }
        if (c >= 'A' && c <= 'F') {
            value = static_cast<uint8_t>(10 + (c - 'A'));
            return true;
        }
        if (c >= 'a' && c <= 'f') {
            value = static_cast<uint8_t>(10 + (c - 'a'));
            return true;
        }
        return false;
    };

    uint8_t high = 0;
    uint8_t low = 0;
    if (!nibble(checksumText[0], high) || !nibble(checksumText[1], low)) {
        return false;
    }
    out = static_cast<uint8_t>((high << 4) | low);
    return true;
}

size_t GpsRuntimeModule::splitCsv(char* payload, char* fields[], size_t maxFields) {
    if (!payload || !fields || maxFields == 0) {
        return 0;
    }

    size_t count = 0;
    fields[count++] = payload;

    for (char* p = payload; *p != '\0' && count < maxFields; ++p) {
        if (*p == ',') {
            *p = '\0';
            fields[count++] = p + 1;
        }
    }

    return count;
}

bool GpsRuntimeModule::sentenceTypeEquals(const char* type, const char* suffix) {
    if (!type || !suffix) {
        return false;
    }

    const size_t typeLen = std::strlen(type);
    const size_t suffixLen = std::strlen(suffix);
    if (typeLen < suffixLen) {
        return false;
    }
    return std::strcmp(type + (typeLen - suffixLen), suffix) == 0;
}

void GpsRuntimeModule::setScaffoldSample(float speedMph,
                                         bool hasFix,
                                         uint8_t satellites,
                                         float hdop,
                                         uint32_t timestampMs,
                                         float latitudeDeg,
                                         float longitudeDeg) {
    if (!enabled_) {
        return;
    }
    if (!std::isfinite(speedMph)) {
        return;
    }

    sampleValid_ = true;
    hasFix_ = hasFix;
    rmcFix_ = hasFix;
    ggaFix_ = hasFix;
    speedMph_ = std::clamp(speedMph, 0.0f, SpeedSourceSelector::MAX_VALID_SPEED_MPH);
    satellites_ = satellites;
    hdop_ = std::isfinite(hdop) ? std::max(0.0f, hdop) : NAN;
    if (hasFix &&
        std::isfinite(latitudeDeg) &&
        std::isfinite(longitudeDeg) &&
        latitudeDeg >= -90.0f &&
        latitudeDeg <= 90.0f &&
        longitudeDeg >= -180.0f &&
        longitudeDeg <= 180.0f) {
        locationValid_ = true;
        latitudeDeg_ = latitudeDeg;
        longitudeDeg_ = longitudeDeg;
    } else {
        locationValid_ = false;
        latitudeDeg_ = NAN;
        longitudeDeg_ = NAN;
    }
    sampleTsMs_ = (timestampMs == 0) ? millis() : timestampMs;
    if (hasFix_) {
        lastFixTsMs_ = sampleTsMs_;
    }
    injectedSamples_++;
    publishObservation(sampleTsMs_);
}

void GpsRuntimeModule::clearSample() {
    invalidateSpeedSample();
    hasFix_ = false;
    rmcFix_ = false;
    ggaFix_ = false;
    satellites_ = 0;
    hdop_ = NAN;
    locationValid_ = false;
    latitudeDeg_ = NAN;
    longitudeDeg_ = NAN;
    lastFixTsMs_ = 0;
    publishObservation(millis());
}

bool GpsRuntimeModule::getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const {
    if (!enabled_ || !sampleValid_ || !hasFix_ || sampleTsMs_ == 0) {
        return false;
    }
    if (static_cast<uint32_t>(nowMs - sampleTsMs_) > SAMPLE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = speedMph_;
    tsMsOut = sampleTsMs_;
    return true;
}

GpsRuntimeStatus GpsRuntimeModule::snapshot(uint32_t nowMs) const {
    GpsRuntimeStatus status;
    status.enabled = enabled_;
    status.sampleValid = sampleValid_;
    status.hasFix = hasFix_;
    status.speedMph = speedMph_;
    status.satellites = satellites_;
    status.hdop = hdop_;
    status.locationValid = locationValid_;
    status.latitudeDeg = latitudeDeg_;
    status.longitudeDeg = longitudeDeg_;
    status.sampleTsMs = sampleTsMs_;
    status.injectedSamples = injectedSamples_;
    status.moduleDetected = moduleDetected_;
    status.detectionTimedOut = detectionTimedOut_;
    status.parserActive = parserActive_;
    status.hardwareSamples = hardwareSamples_;
    status.bytesRead = bytesRead_;
    status.sentencesSeen = sentencesSeen_;
    status.sentencesParsed = sentencesParsed_;
    status.parseFailures = parseFailures_;
    status.checksumFailures = checksumFailures_;
    status.bufferOverruns = bufferOverruns_;
    status.lastSentenceTsMs = lastSentenceTsMs_;

    if (sampleValid_ && sampleTsMs_ != 0) {
        status.sampleAgeMs = static_cast<uint32_t>(nowMs - sampleTsMs_);
    } else {
        status.sampleAgeMs = UINT32_MAX;
    }

    return status;
}

void GpsRuntimeModule::publishObservation(uint32_t timestampMs) {
    GpsObservation observation;
    observation.tsMs = (timestampMs == 0) ? millis() : timestampMs;
    observation.hasFix = hasFix_;
    bool speedFresh = sampleValid_ && hasFix_ && sampleTsMs_ != 0 && observation.tsMs >= sampleTsMs_;
    if (speedFresh) {
        speedFresh = (observation.tsMs - sampleTsMs_) <= SAMPLE_MAX_AGE_MS;
    }
    observation.speedValid = speedFresh;
    observation.speedMph = speedFresh ? speedMph_ : 0.0f;
    observation.satellites = satellites_;
    observation.hdop = hdop_;
    observation.locationValid = locationValid_ && hasFix_;
    observation.latitudeDeg = observation.locationValid ? latitudeDeg_ : NAN;
    observation.longitudeDeg = observation.locationValid ? longitudeDeg_ : NAN;
    gpsObservationLog.publish(observation);
}

#ifdef UNIT_TEST
bool GpsRuntimeModule::injectNmeaSentenceForTest(const char* nmeaSentence, uint32_t nowMs) {
    if (!enabled_ || !nmeaSentence) {
        return false;
    }

    const size_t inputLen = std::strlen(nmeaSentence);
    if (inputLen == 0 || inputLen >= NMEA_LINE_MAX) {
        bufferOverruns_++;
        return false;
    }

    char localBuf[NMEA_LINE_MAX] = {};
    std::memcpy(localBuf, nmeaSentence, inputLen);
    localBuf[inputLen] = '\0';
    return processSentence(localBuf, nowMs);
}
#endif
