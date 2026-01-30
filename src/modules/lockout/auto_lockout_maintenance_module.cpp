#include "auto_lockout_maintenance_module.h"

void AutoLockoutMaintenance::begin(AutoLockoutManager* mgr) {
    autoLockouts = mgr;
}

void AutoLockoutMaintenance::process(unsigned long nowMs) {
    if (!autoLockouts) return;

    if (nowMs - lastUpdateMs > 30000) {
        autoLockouts->update();
        lastUpdateMs = nowMs;
    }
    autoLockouts->maintenanceTick(nowMs);
}
