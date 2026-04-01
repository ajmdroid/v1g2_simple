#pragma once

#include "settings.h"

#include "modules/gps/gps_runtime_module.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/speed/speed_source_selector.h"

// These helpers stay header-only so direct-include native tests can compile
// the production paths without introducing extra link-time fixtures. Include
// this header only after the relevant runtime class definitions are visible in
// UNIT_TEST builds where mocks replace the production classes.

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

template <typename TGpsRuntimeModule, typename TSpeedSourceSelector>
inline void syncGpsVehicleRuntimeSettings(const V1Settings& settings,
                                          TGpsRuntimeModule& gpsRuntimeModule,
                                          TSpeedSourceSelector& speedSourceSelector) {
    syncGpsRuntimeEnabled(settings, gpsRuntimeModule);
    syncSpeedSourceSelectorInputs(settings, speedSourceSelector);
}

template <typename TObdRuntimeModule, typename TSpeedSourceSelector>
inline void syncObdVehicleRuntimeSettings(const V1Settings& settings,
                                          TObdRuntimeModule& obdRuntimeModule,
                                          TSpeedSourceSelector& speedSourceSelector) {
    syncObdRuntimeSettings(settings, obdRuntimeModule);
    syncSpeedSourceSelectorInputs(settings, speedSourceSelector);
}

template <typename TGpsRuntimeModule,
          typename TObdRuntimeModule,
          typename TSpeedSourceSelector>
inline void syncVehicleRuntimeInputs(const V1Settings& settings,
                                     TGpsRuntimeModule& gpsRuntimeModule,
                                     TObdRuntimeModule& obdRuntimeModule,
                                     TSpeedSourceSelector& speedSourceSelector) {
    syncGpsRuntimeEnabled(settings, gpsRuntimeModule);
    syncObdRuntimeSettings(settings, obdRuntimeModule);
    syncSpeedSourceSelectorInputs(settings, speedSourceSelector);
}

}  // namespace SettingsRuntimeSync
