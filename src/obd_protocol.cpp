// OBD-II Protocol helpers — AT commands, PID requests, response parsers,
// and static utility functions.
// Extracted from obd_handler.cpp for maintainability.

#include "obd_internals.h"

#include <NimBLEDevice.h>
#include <cstring>

// ---------------------------------------------------------------------------
// AT command transport
// ---------------------------------------------------------------------------

bool OBDHandler::sendATCommand(const char* cmd, String& response, uint32_t timeoutMs) {
    auto logCommandFailure = [&](const char* reason, const String* resp = nullptr) {
        static uint32_t lastFailLogMs = 0;
        static uint32_t suppressed = 0;
        const uint32_t now = millis();
        if ((now - lastFailLogMs) < 2000) {
            suppressed++;
            return;
        }

        if (suppressed > 0) {
            Serial.printf("[OBD] AT failure logs suppressed: %lu\n", (unsigned long)suppressed);
            suppressed = 0;
        }

        if (resp) {
            Serial.printf("[OBD] AT '%s' failed: %s, resp='%s'\n",
                          cmd ? cmd : "",
                          reason ? reason : "unknown",
                          resp->c_str());
        } else {
            Serial.printf("[OBD] AT '%s' failed: %s\n",
                          cmd ? cmd : "",
                          reason ? reason : "unknown");
        }
        lastFailLogMs = now;
    };

    if (!pRXChar || !pOBDClient || !pOBDClient->isConnected()) {
        logCommandFailure("link not ready");
        return false;
    }

    // Reset response state and flush any stale data from the stream buffer.
    // No mutex needed here — responseBuffer is only touched by this function
    // (running on the OBD task) now that the notification callback writes
    // exclusively to the stream buffer.
    responseLength = 0;
    responseBuffer[0] = '\0';
    responseComplete.store(false, std::memory_order_release);
    if (notifyStream) {
        xStreamBufferReset(notifyStream);
    }

    String cmdLine = String(cmd) + "\r";
    if (!pRXChar->writeValue((uint8_t*)cmdLine.c_str(), cmdLine.length(), false)) {
        logCommandFailure("writeValue failed");
        return false;
    }

    // Drain the stream buffer until the ELM327 prompt '>' is seen or we
    // time out.  The notification callback writes raw bytes into the stream
    // buffer lock-free, so there is zero mutex contention on this path.
    const uint32_t startMs = millis();
    uint8_t chunk[64];
    while (!responseComplete.load(std::memory_order_acquire)) {
        if ((millis() - startMs) >= timeoutMs) {
            break;
        }

        size_t received = 0;
        if (notifyStream) {
            received = xStreamBufferReceive(notifyStream, chunk, sizeof(chunk),
                                            pdMS_TO_TICKS(10));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        for (size_t i = 0; i < received; i++) {
            const char c = static_cast<char>(chunk[i]);
            if (c == '>') {
                responseComplete.store(true, std::memory_order_release);
                break;
            }
            if (c != '\r' && c != '\n' && responseLength < RESPONSE_BUFFER_SIZE) {
                responseBuffer[responseLength++] = c;
                responseBuffer[responseLength] = '\0';
            }
        }
    }

    response = responseBuffer;
    if (!responseComplete.load(std::memory_order_acquire)) {
        logCommandFailure("timeout waiting for prompt");
        return false;
    }

    String normalized(response);
    normalized.toUpperCase();
    String compact(normalized);
    compact.replace(" ", "");
    const bool hasError =
        normalized.indexOf("ERROR") >= 0 ||
        normalized.indexOf("UNABLE") >= 0 ||
        normalized.indexOf("NO DATA") >= 0 ||
        normalized.indexOf("STOPPED") >= 0 ||
        normalized.indexOf("BUS BUSY") >= 0 ||
        normalized.indexOf("NO RESPONSE") >= 0 ||
        compact == "?";
    if (hasError) {
        logCommandFailure("error response", &response);
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// PID request methods
// ---------------------------------------------------------------------------

bool OBDHandler::requestSpeed() {
    String response;
    if (!sendATCommand("010D", response, 1000)) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            lastData.valid = false;
        }
        return false;
    }

    uint8_t speedKph = 0;
    if (!parseSpeedResponse(response, speedKph)) {
        ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
        if (lock.ok()) {
            lastData.valid = false;
        }
        return false;
    }

    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return false;
    }

    lastData.speed_kph = speedKph;
    lastData.speed_mph = speedKph * 0.621371f;
    lastData.timestamp_ms = millis();
    lastData.valid = true;
    return true;
}

bool OBDHandler::requestRPM() {
    String response;
    if (!sendATCommand("010C", response, 250)) {
        return false;
    }

    uint16_t rpm = 0;
    if (!parseRPMResponse(response, rpm)) {
        return false;
    }

    ObdLock lock(obdMutex, 0);
    if (!lock.ok()) {
        return false;
    }
    lastData.rpm = rpm;
    lastData.timestamp_ms = millis();
    return true;
}

bool OBDHandler::requestVoltage() {
    String response;
    if (!sendATCommand("ATRV", response, 250)) {
        return false;
    }

    float voltage = 0.0f;
    if (!parseVoltageResponse(response, voltage)) {
        return false;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return false;
    }
    lastData.voltage = voltage;
    lastData.timestamp_ms = millis();
    return true;
}

bool OBDHandler::requestIntakeAirTemp() {
    String response;
    if (!sendATCommand("010F", response, 500)) {
        return false;
    }

    int8_t tempC = -128;
    if (!parseIntakeAirTempResponse(response, tempC)) {
        return false;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return false;
    }
    lastData.intake_air_temp_c = tempC;
    lastData.timestamp_ms = millis();
    return true;
}

bool OBDHandler::requestOilTemp() {
    String response;
    if (!sendATCommand("ATSH7E0", response, 500)) {
        return false;
    }

    bool parsed = false;
    int16_t tempC = INT16_MIN;

    if (sendATCommand("22F40C", response, 500)) {
        parsed = parseVwMode22TempResponse(response, "F40C", tempC);
    }

    // Always restore default functional header after VW-specific request.
    String restoreResponse;
    bool restoreOk = sendATCommand("ATSH7DF", restoreResponse, 200);
    if (!restoreOk) {
        Serial.println("[OBD] WARN: Failed to restore functional CAN header (ATSH7DF)");
    }

    if (!parsed) {
        return false;
    }

    ObdLock lock(obdMutex, pdMS_TO_TICKS(20));
    if (!lock.ok()) {
        return false;
    }
    lastData.oil_temp_c = tempC;
    lastData.timestamp_ms = millis();
    return true;
}

// ---------------------------------------------------------------------------
// Response parsers
// ---------------------------------------------------------------------------

bool OBDHandler::isValidHexString(const String& str, size_t expectedLen) {
    if (str.length() == 0) return false;
    if (expectedLen > 0 && str.length() != expectedLen) return false;

    for (size_t i = 0; i < str.length(); i++) {
        const char c = str[i];
        const bool isHex = (c >= '0' && c <= '9') ||
                           (c >= 'A' && c <= 'F') ||
                           (c >= 'a' && c <= 'f');
        if (!isHex) {
            return false;
        }
    }
    return true;
}

bool OBDHandler::parseSpeedResponse(const String& response, uint8_t& speedKph) {
    String normalized(response);
    normalized.toUpperCase();
    normalized.replace(" ", "");

    int idx = normalized.indexOf("410D");
    if (idx < 0) return false;

    const String hexVal = normalized.substring(idx + 4, idx + 6);
    if (!isValidHexString(hexVal, 2)) return false;

    speedKph = static_cast<uint8_t>(strtoul(hexVal.c_str(), nullptr, 16));
    return true;
}

bool OBDHandler::parseRPMResponse(const String& response, uint16_t& rpm) {
    String normalized(response);
    normalized.toUpperCase();
    normalized.replace(" ", "");

    int idx = normalized.indexOf("410C");
    if (idx < 0) return false;

    const String hexA = normalized.substring(idx + 4, idx + 6);
    const String hexB = normalized.substring(idx + 6, idx + 8);

    if (!isValidHexString(hexA, 2) || !isValidHexString(hexB, 2)) {
        return false;
    }

    const uint8_t a = static_cast<uint8_t>(strtoul(hexA.c_str(), nullptr, 16));
    const uint8_t b = static_cast<uint8_t>(strtoul(hexB.c_str(), nullptr, 16));
    rpm = ((uint16_t)a * 256 + b) / 4;
    return true;
}

bool OBDHandler::parseVoltageResponse(const String& response, float& voltage) {
    voltage = response.toFloat();
    return voltage > 0.0f && voltage < 20.0f;
}

bool OBDHandler::parseIntakeAirTempResponse(const String& response, int8_t& tempC) {
    String normalized(response);
    normalized.toUpperCase();
    normalized.replace(" ", "");

    int idx = normalized.indexOf("410F");
    if (idx < 0) return false;

    const String hexVal = normalized.substring(idx + 4, idx + 6);
    if (!isValidHexString(hexVal, 2)) return false;

    const uint8_t raw = static_cast<uint8_t>(strtoul(hexVal.c_str(), nullptr, 16));
    tempC = static_cast<int8_t>(raw - 40);
    return true;
}

bool OBDHandler::parseVwMode22TempResponse(const String& response, const char* pidEcho, int16_t& tempC) {
    if (!pidEcho || pidEcho[0] == '\0') {
        return false;
    }

    String normalized(response);
    normalized.toUpperCase();

    String pattern = String("62") + String(pidEcho);
    pattern.toUpperCase();

    const int idx = normalized.indexOf(pattern);
    if (idx < 0) {
        return false;
    }

    const int dataStart = idx + pattern.length();
    const int remaining = normalized.length() - dataStart;

    // VW UDS temperature DIDs may return 1 or 2 data bytes.
    // When 2 bytes are present (e.g. 62F40C 0C 86), the first byte is
    // a qualifier/format identifier and the second byte is the actual
    // temperature value.  VW MQB oil temp uses offset -60 (per opendbc
    // MO_Oel_Temp signal: scale 1, offset -60, range -60..192 °C).
    uint8_t raw = 0;
    if (remaining >= 4) {
        const String hexA = normalized.substring(dataStart, dataStart + 2);
        const String hexB = normalized.substring(dataStart + 2, dataStart + 4);
        if (isValidHexString(hexA, 2) && isValidHexString(hexB, 2)) {
            raw = static_cast<uint8_t>(strtoul(hexB.c_str(), nullptr, 16));
            Serial.printf("[OBD] VW temp %s raw: %s %s → using byte B=%u\n",
                          pidEcho, hexA.c_str(), hexB.c_str(), (unsigned)raw);
        } else if (isValidHexString(hexA, 2)) {
            raw = static_cast<uint8_t>(strtoul(hexA.c_str(), nullptr, 16));
        } else {
            return false;
        }
    } else if (remaining >= 2) {
        const String hexVal = normalized.substring(dataStart, dataStart + 2);
        if (!isValidHexString(hexVal, 2)) {
            return false;
        }
        raw = static_cast<uint8_t>(strtoul(hexVal.c_str(), nullptr, 16));
    } else {
        return false;
    }

    if (raw == 0) {
        return false;
    }

    // VW MQB oil temp: raw - 60  (opendbc MO_Oel_Temp offset = -60)
    tempC = static_cast<int16_t>(raw) - 60;
    return true;
}

// ---------------------------------------------------------------------------
// Static utility functions
// ---------------------------------------------------------------------------

bool OBDHandler::isObdLinkName(const std::string& name) {
    if (name.empty()) {
        return false;
    }
    String upper(name.c_str());
    upper.toUpperCase();
    const bool isObdLinkBrand = (upper.indexOf("OBDLINK") >= 0) ||
                                (upper.indexOf("OBD LINK") >= 0) ||
                                (upper.indexOf("OBD-LINK") >= 0);
    const bool isCxModel = upper.indexOf("CX") >= 0;
    return isObdLinkBrand && isCxModel;
}

bool OBDHandler::isNullAddressString(const String& address) {
    if (address.length() == 0) {
        return true;
    }
    const NimBLEAddress parsed(std::string(address.c_str()), BLE_ADDR_PUBLIC);
    return parsed.isNull() || isAllZeroAddress(parsed);
}

uint32_t OBDHandler::normalizePin(const String& pin, bool obdLinkDefault) {
    String digits;
    digits.reserve(6);
    for (size_t i = 0; i < pin.length() && digits.length() < 6; i++) {
        const char c = pin[i];
        if (c >= '0' && c <= '9') {
            digits += c;
        }
    }

    if (digits.length() == 0) {
        return obdLinkDefault ? 123456u : 1234u;
    }

    uint32_t value = static_cast<uint32_t>(strtoul(digits.c_str(), nullptr, 10));
    if (value > 999999u) {
        value = 999999u;
    }
    return value;
}
