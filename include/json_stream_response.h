#pragma once
/**
 * Streaming JSON helper — sends JSON directly to the WiFi client
 * without allocating an intermediate Arduino String.
 *
 * Saves ~3-15 KB of internal SRAM per API response depending on
 * document size. Avoids heap fragmentation from short-lived String
 * allocations during WiFi request handling.
 *
 * Usage:
 *   JsonDocument doc;
 *   doc["key"] = "value";
 *   sendJsonStream(server, doc);          // 200 OK
 *   sendJsonStream(server, doc, 400);     // custom status code
 */

#include <ArduinoJson.h>
#include <WebServer.h>
#include <stdint.h>
#include <string.h>
#include "client_write_retry.h"

namespace json_stream_detail {

// Buffered Print adapter to avoid byte-at-a-time TCP writes from serializeJson().
template <typename ClientT, size_t kBufferSize = 512>
class BufferedClientPrint final : public Print {
public:
    explicit BufferedClientPrint(ClientT& client) : client_(client) {}

    size_t write(uint8_t c) override { return write(&c, 1); }

    size_t write(const uint8_t* data, size_t size) override {
        if (!data || size == 0 || failed_) {
            return 0;
        }

        size_t accepted = 0;
        while (size > 0) {
            if (used_ == kBufferSize) {
                if (!flushBuffer()) {
                    break;
                }
            }

            const size_t freeBytes = kBufferSize - used_;
            const size_t toCopy = (size < freeBytes) ? size : freeBytes;
            memcpy(buffer_ + used_, data, toCopy);
            used_ += toCopy;
            data += toCopy;
            size -= toCopy;
            accepted += toCopy;
        }
        return accepted;
    }

    bool flushBuffer() {
        if (failed_ || used_ == 0) {
            return !failed_;
        }

        failed_ = !client_write_retry::writeAll(client_, buffer_, used_);
        used_ = 0;
        return !failed_;
    }

private:
    ClientT& client_;
    uint8_t buffer_[kBufferSize] = {};
    size_t used_ = 0;
    bool failed_ = false;
};

}  // namespace json_stream_detail

inline void sendJsonStream(WebServer& server, JsonDocument& doc, int code = 200) {
#if defined(UNIT_TEST)
    String response;
    serializeJson(doc, response);
    server.send(code, "application/json", response);
#else
    const size_t len = measureJson(doc);
    server.setContentLength(len);
    server.send(code, "application/json", "");

    auto client = server.client();
    json_stream_detail::BufferedClientPrint<decltype(client)> buffered(client);
    serializeJson(doc, buffered);
    buffered.flushBuffer();
#endif
}
