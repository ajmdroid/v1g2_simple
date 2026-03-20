#include "lockout_pending_store.h"

namespace lockout_pending_store {

JsonRollbackLoadResult loadPendingLearnerJsonDocument(fs::FS& fs,
                                                      JsonDocument& outDoc,
                                                      String* errorMessage,
                                                      String* loadedPath) {
    return loadJsonDocumentWithRollback(
        fs,
        kPendingLearnerPath,
        32767,
        outDoc,
        errorMessage,
        loadedPath);
}

}  // namespace lockout_pending_store
