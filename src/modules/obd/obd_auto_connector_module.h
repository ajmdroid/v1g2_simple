#pragma once

#include <Arduino.h>
#include "obd_handler.h"

class ObdAutoConnector {
public:
    void begin(OBDHandler* handler);
    void scheduleAfterConnect(unsigned long delayMs);
    void process(unsigned long nowMs);

private:
    OBDHandler* obd = nullptr;
    unsigned long connectAtMs = 0;
};
