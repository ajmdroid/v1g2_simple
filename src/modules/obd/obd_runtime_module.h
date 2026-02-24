#pragma once

#include <Arduino.h>

class OBDHandler;
class SpeedSourceSelector;

// Owns main-loop OBD runtime gating and speed-source updates.
class ObdRuntimeModule {
public:
    void reset();

    void process(unsigned long nowMs,
                 bool obdServiceEnabled,
                 bool& obdAutoConnectPending,
                 unsigned long obdAutoConnectAtMs,
                 OBDHandler& obdHandler,
                 SpeedSourceSelector& speedSourceSelector);

private:
    bool obdRuntimeDisabledLatched_ = false;
};
