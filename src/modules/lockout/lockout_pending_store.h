#pragma once

#include "../../storage_json_rollback.h"

namespace lockout_pending_store {

inline constexpr const char* kPendingLearnerPath = "/v1simple_lockout_pending.json";

JsonRollbackLoadResult loadPendingLearnerJsonDocument(fs::FS& fs,
                                                      JsonDocument& outDoc,
                                                      String* errorMessage = nullptr,
                                                      String* loadedPath = nullptr);

}  // namespace lockout_pending_store
