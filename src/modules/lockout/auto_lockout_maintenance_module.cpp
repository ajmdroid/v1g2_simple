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

    if (nowMs - lastSaveMs > 300000) {
        if (autoLockouts->getClusterCount() > 0) {
            autoLockouts->saveToJSON("/v1profiles/auto_lockouts.json");
        }
        lastSaveMs = nowMs;
    }
}
