#pragma once

/**
 * Internal dependency storage for the DebugApiService namespace.
 *
 * Only include this from debug_api_service.cpp and debug_api_scenario_service.cpp.
 * Not part of the public API — do not include from outside the debug module.
 *
 * Storage is defined in debug_api_service.cpp; all debug TUs share these pointers
 * after DebugApiService::begin() is called during setup().
 */

class SystemEventBus;
class V1BLEClient;
class BleQueueModule;

namespace DebugApiService {
namespace deps {

extern SystemEventBus* eventBus;
extern V1BLEClient*    bleClient;
extern BleQueueModule* bleQueue;

}  // namespace deps
}  // namespace DebugApiService
