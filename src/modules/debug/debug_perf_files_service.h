#pragma once

#include <WebServer.h>

namespace DebugPerfFilesService {

void handleApiPerfFilesList(WebServer& server,
                            bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                            void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiPerfFilesDownload(WebServer& server,
                                bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                                void (*markUiActivity)(void* ctx), void* uiActivityCtx);

void handleApiPerfFilesDelete(WebServer& server,
                              bool (*checkRateLimit)(void* ctx), void* rateLimitCtx,
                              void (*markUiActivity)(void* ctx), void* uiActivityCtx);

}  // namespace DebugPerfFilesService
