#include "camera_runtime_module.h"

CameraRuntimeModule cameraRuntimeModule;

void CameraRuntimeModule::begin(bool enabled) {
    enabled_ = enabled;
    lastTickMs_ = 0;
    counters_ = {};
    index_.clear();
    eventLog_.reset();
    dataLoader_.reset();
}

void CameraRuntimeModule::setEnabled(bool enabled) {
    enabled_ = enabled;
}

bool CameraRuntimeModule::tryLoadDefault(uint32_t nowMs) {
    const bool ok = dataLoader_.loadDefault(index_, nowMs);
    if (!ok) {
        counters_.cameraLoadFailures++;
    }
    return ok;
}

void CameraRuntimeModule::process(uint32_t nowMs, bool skipNonCoreThisLoop, bool overloadThisLoop) {
    if (!enabled_) {
        return;
    }

    if (skipNonCoreThisLoop) {
        counters_.cameraTickSkipsNonCore++;
        return;
    }

    if (overloadThisLoop) {
        counters_.cameraTickSkipsOverload++;
        return;
    }

    if (lastTickMs_ != 0 && static_cast<uint32_t>(nowMs - lastTickMs_) < tickIntervalMs_) {
        return;
    }
    lastTickMs_ = nowMs;
    counters_.cameraTicks++;

    // M1: no matching, no display/audio side effects.
}

CameraRuntimeStatus CameraRuntimeModule::snapshot() const {
    CameraRuntimeStatus out;
    out.enabled = enabled_;
    out.indexLoaded = index_.isLoaded();
    out.tickIntervalMs = tickIntervalMs_;
    out.lastTickMs = lastTickMs_;
    out.counters = counters_;
    out.loader = dataLoader_.status();
    return out;
}

