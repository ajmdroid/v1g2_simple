#pragma once

#include <stddef.h>
#include <stdint.h>

namespace client_write_retry {

inline void service() {
#if defined(ARDUINO)
    yield();
#endif
}

template <typename ClientT>
bool writeAll(ClientT& client,
              const uint8_t* data,
              size_t size,
              uint8_t maxZeroWriteRetries = 64) {
    if (!data || size == 0) {
        return true;
    }

    size_t offset = 0;
    uint8_t zeroWriteRetries = 0;
    while (offset < size) {
        const size_t written = client.write(data + offset, size - offset);
        if (written == 0) {
            if (zeroWriteRetries++ >= maxZeroWriteRetries) {
                return false;
            }
            service();
            continue;
        }

        offset += written;
        zeroWriteRetries = 0;
        if (offset < size) {
            service();
        }
    }

    return true;
}

}  // namespace client_write_retry
