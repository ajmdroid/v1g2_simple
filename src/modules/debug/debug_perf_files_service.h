#pragma once

#include <WebServer.h>

namespace DebugPerfFilesService {

/// Injected dependencies for perf file management handlers.
/// Uses void* for opaque platform handles (SemaphoreHandle_t, fs::FS*)
/// so this header stays free of FreeRTOS and FS includes.
struct PerfFilesRuntime {
    bool (*isStorageReady)(void* ctx) = nullptr;
    bool (*isSDCard)(void* ctx) = nullptr;
    void* (*getSDMutex)(void* ctx) = nullptr;       // Returns SemaphoreHandle_t
    void* (*getFilesystem)(void* ctx) = nullptr;     // Returns fs::FS*
    bool (*isPerfLoggingEnabled)(void* ctx) = nullptr;
    const char* (*getPerfCsvPath)(void* ctx) = nullptr;
    void* ctx = nullptr;
};

void handleApiPerfFilesList(WebServer& server,
                            const PerfFilesRuntime& runtime,
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                            void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiPerfFilesDownload(WebServer& server,
                                const PerfFilesRuntime& runtime,
                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiPerfFilesDelete(WebServer& server,
                              const PerfFilesRuntime& runtime,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                              void (*markUiActivity)(void* ctx), void* uiActivityCtx);

}  // namespace DebugPerfFilesService
