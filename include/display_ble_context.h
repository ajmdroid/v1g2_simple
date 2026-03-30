#pragma once
#ifndef DISPLAY_BLE_CONTEXT_H
#define DISPLAY_BLE_CONTEXT_H

/**
 * Lightweight snapshot of BLE state consumed by the display layer.
 * Populated in main.cpp each loop iteration so display files never
 * need an `extern V1BLEClient` dependency.
 */
struct DisplayBleContext {
    bool v1Connected      = false;
    bool proxyConnected   = false;
    int  v1Rssi           = 0;
    int  proxyRssi        = 0;
};

#endif // DISPLAY_BLE_CONTEXT_H
