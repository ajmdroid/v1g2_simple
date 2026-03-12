#include "obd_elm327_parser.h"

#include <cctype>
#include <cstring>

namespace {

/// Skip leading whitespace and trailing \r\n from the response.
/// Returns pointer to trimmed start; sets trimmedLen to trimmed length.
const char* trimResponse(const char* response, size_t len, size_t& trimmedLen) {
    // Skip leading whitespace
    size_t start = 0;
    while (start < len && (response[start] == ' ' || response[start] == '\r' ||
                           response[start] == '\n' || response[start] == '\t')) {
        ++start;
    }
    // Skip trailing whitespace
    size_t end = len;
    while (end > start && (response[end - 1] == ' ' || response[end - 1] == '\r' ||
                           response[end - 1] == '\n' || response[end - 1] == '\t' ||
                           response[end - 1] == '>')) {
        --end;
    }
    trimmedLen = end - start;
    return response + start;
}

/// Parse a single hex digit. Returns 0-15 or -1 on failure.
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// Parse a two-character hex byte. Returns 0-255 or -1 on failure.
int hexByte(const char* s) {
    int hi = hexDigit(s[0]);
    int lo = hexDigit(s[1]);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/// Check if trimmed response starts with a given prefix (case-insensitive).
bool startsWithCI(const char* s, size_t sLen, const char* prefix) {
    size_t pLen = strlen(prefix);
    if (sLen < pLen) return false;
    for (size_t i = 0; i < pLen; ++i) {
        if (toupper(static_cast<unsigned char>(s[i])) !=
            toupper(static_cast<unsigned char>(prefix[i]))) {
            return false;
        }
    }
    return true;
}

}  // namespace

Elm327ParseResult parseElm327Response(const char* response, size_t len) {
    Elm327ParseResult result;

    if (response == nullptr || len == 0) {
        result.error = true;
        return result;
    }

    size_t trimmedLen = 0;
    const char* s = trimResponse(response, len, trimmedLen);

    if (trimmedLen == 0) {
        result.error = true;
        return result;
    }

    // Check for "NO DATA"
    if (startsWithCI(s, trimmedLen, "NO DATA")) {
        result.noData = true;
        return result;
    }

    // Check for "?" error
    if (trimmedLen == 1 && s[0] == '?') {
        result.error = true;
        return result;
    }

    // Check for "SEARCHING..." or "BUS INIT..."
    if (startsWithCI(s, trimmedLen, "SEARCHING") ||
        startsWithCI(s, trimmedLen, "BUS INIT")) {
        result.busInit = true;
        return result;
    }

    // Check for other known non-data responses
    if (startsWithCI(s, trimmedLen, "UNABLE TO CONNECT") ||
        startsWithCI(s, trimmedLen, "CAN ERROR") ||
        startsWithCI(s, trimmedLen, "BUFFER FULL") ||
        startsWithCI(s, trimmedLen, "BUS ERROR") ||
        startsWithCI(s, trimmedLen, "ERROR") ||
        startsWithCI(s, trimmedLen, "STOPPED")) {
        result.error = true;
        return result;
    }

    // Try to parse as hex data response (e.g. "41 0D 3C" or "410D3C")
    // Collect hex bytes, skipping spaces
    uint8_t bytes[8];
    size_t byteCount = 0;
    size_t i = 0;

    while (i < trimmedLen && byteCount < sizeof(bytes)) {
        // Skip spaces
        if (s[i] == ' ') {
            ++i;
            continue;
        }
        // Need at least 2 hex chars
        if (i + 1 >= trimmedLen) {
            result.error = true;
            return result;
        }
        int val = hexByte(s + i);
        if (val < 0) {
            result.error = true;
            return result;
        }
        bytes[byteCount++] = static_cast<uint8_t>(val);
        i += 2;
    }

    // Minimum valid OBD response: service byte + PID byte = 2 bytes
    if (byteCount < 2) {
        result.error = true;
        return result;
    }

    result.valid = true;
    result.service = bytes[0];
    result.pid = bytes[1];
    result.dataLen = static_cast<uint8_t>(byteCount > 2 ? byteCount - 2 : 0);
    for (uint8_t d = 0; d < result.dataLen && d < 4; ++d) {
        result.dataBytes[d] = bytes[2 + d];
    }

    return result;
}

float decodeSpeedKmh(const Elm327ParseResult& result) {
    // Valid speed response: service 0x41, PID 0x0D, 1 data byte
    if (!result.valid || result.service != 0x41 || result.pid != 0x0D ||
        result.dataLen < 1) {
        return -1.0f;
    }
    return static_cast<float>(result.dataBytes[0]);
}
