#pragma once

#include "settings.h"

#ifndef UNIT_TEST
#include "modules/gps/gps_runtime_module.h"
#include "modules/lockout/lockout_learner.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/speed/speed_source_selector.h"
#endif

// These helpers stay header-only so direct-include native tests can compile
// the production paths without introducing extra link-time fixtures. Include
// this header only after the relevant runtime class definitions are visible in
// UNIT_TEST builds where mocks replace the production classes.

void lockoutSetKaLearningEnabled(bool enabled);
void lockoutSetKLearningEnabled(bool enabled);
void lockoutSetXLearningEnabled(bool enabled);

namespace SettingsRuntimeSync {

template <typename TGpsRuntimeModule>
inline void syncGpsRuntimeEnabled(const V1Settings& settings,
                                  TGpsRuntimeModule& gpsRuntimeModule) {
    gpsRuntimeModule.setEnabled(settings.gpsEnabled);
}

template <typename TObdRuntimeModule>
inline void syncObdRuntimeSettings(const V1Settings& settings,
                                   TObdRuntimeModule& obdRuntimeModule) {
    obdRuntimeModule.setEnabled(settings.obdEnabled);
    obdRuntimeModule.setMinRssi(settings.obdMinRssi);
}

template <typename TSpeedSourceSelector>
inline void syncSpeedSourceSelectorInputs(const V1Settings& settings,
                                          TSpeedSourceSelector& speedSourceSelector) {
    speedSourceSelector.syncEnabledInputs(settings.gpsEnabled, settings.obdEnabled);
}

inline void syncLockoutBandLearningPolicy(const V1Settings& settings) {
    lockoutSetKaLearningEnabled(settings.gpsLockoutKaLearningEnabled);
    lockoutSetKLearningEnabled(settings.gpsLockoutKLearningEnabled);
    lockoutSetXLearningEnabled(settings.gpsLockoutXLearningEnabled);
}

template <typename TLockoutLearner>
inline void syncLockoutLearnerTuning(const V1Settings& settings,
                                     TLockoutLearner& lockoutLearner) {
    lockoutLearner.setTuning(settings.gpsLockoutLearnerPromotionHits,
                             settings.gpsLockoutLearnerRadiusE5,
                             settings.gpsLockoutLearnerFreqToleranceMHz,
                             settings.gpsLockoutLearnerLearnIntervalHours,
                             settings.gpsLockoutMaxHdopX10,
                             settings.gpsLockoutMinLearnerSpeedMph);
}

}  // namespace SettingsRuntimeSync
