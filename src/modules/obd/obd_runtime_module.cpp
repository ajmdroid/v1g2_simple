#include "obd_runtime_module.h"

#ifdef UNIT_TEST
#include "../../../test/mocks/obd_handler.h"
#else
#include "obd_handler.h"
#endif
#include "modules/speed/speed_source_selector.h"

void ObdRuntimeModule::reset() {
    obdRuntimeDisabledLatched_ = false;
}

void ObdRuntimeModule::process(unsigned long nowMs,
                               bool obdServiceEnabled,
                               bool& obdAutoConnectPending,
                               unsigned long obdAutoConnectAtMs,
                               OBDHandler& obdHandler,
                               SpeedSourceSelector& speedSourceSelector) {
    if (!obdServiceEnabled) {
        obdAutoConnectPending = false;
        if (!obdRuntimeDisabledLatched_) {
            obdHandler.stopScan();
            obdHandler.disconnect();
            obdRuntimeDisabledLatched_ = true;
        }
    } else {
        obdRuntimeDisabledLatched_ = false;

        if (obdAutoConnectPending && nowMs >= obdAutoConnectAtMs) {
            obdAutoConnectPending = false;
            obdHandler.tryAutoConnect();
        }

        if (obdHandler.update()) {
            OBDData obdData = obdHandler.getData();
            speedSourceSelector.updateObdSample(obdData.speed_mph, obdData.timestamp_ms, obdData.valid);
        }
    }

    speedSourceSelector.setObdConnected(obdServiceEnabled && obdHandler.isConnected());
}
