#pragma once

#include <Arduino.h>
#include "auto_lockout_manager.h"

class AutoLockoutMaintenance {
public:
    void begin(AutoLockoutManager* mgr);
    void process(unsigned long nowMs);

private:
    AutoLockoutManager* autoLockouts = nullptr;
    unsigned long lastUpdateMs = 0;
};
