/**
 * main_globals.h — Centralized extern declarations for globals defined
 * in main.cpp and module .cpp files.
 *
 * Eliminates duplicate extern declarations scattered across translation
 * units.  Each consumer includes its own type-definition headers; this
 * file only forward-declares enough for the extern statements.
 */

#ifndef MAIN_GLOBALS_H
#define MAIN_GLOBALS_H

// =========================================================================
// Forward declarations (avoids pulling in full module headers)
// =========================================================================

// Loop phase modules (src/modules/system/)
class LoopConnectionEarlyModule;
class LoopSettingsPrepModule;
class LoopPreIngestModule;
class LoopIngestModule;
class LoopDisplayModule;
class LoopPostDisplayModule;
class LoopRuntimeSnapshotModule;
class LoopPowerTouchModule;
class LoopTailModule;
class PeriodicMaintenanceModule;
class WifiRuntimeModule;
class WifiAutoStartModule;

// Core subsystems
class V1BLEClient;
class PacketParser;
class TouchHandler;
#include "../src/display_mode.h"  // enum class — cannot forward-declare
#include "main_runtime_state.h"
class AutoPushModule;
class TapGestureModule;
class AlertPersistenceModule;
class BleQueueModule;
class SystemEventBus;
class QuietCoordinatorModule;

#ifndef UNIT_TEST
class DisplayPipelineModule;
#endif

// Perf monitoring (conditional)
#if defined(PERF_METRICS) && defined(PERF_MONITORING)
struct PerfLatency;
#endif

// =========================================================================
// Extern declarations — loop phase modules
// =========================================================================

extern LoopConnectionEarlyModule loopConnectionEarlyModule;
extern LoopSettingsPrepModule loopSettingsPrepModule;
extern LoopPreIngestModule loopPreIngestModule;
extern LoopIngestModule loopIngestModule;
extern LoopDisplayModule loopDisplayModule;
extern LoopPostDisplayModule loopPostDisplayModule;
extern LoopRuntimeSnapshotModule loopRuntimeSnapshotModule;
extern WifiRuntimeModule wifiRuntimeModule;
extern WifiAutoStartModule wifiAutoStartModule;
extern PeriodicMaintenanceModule periodicMaintenanceModule;
extern LoopTailModule loopTailModule;
extern LoopPowerTouchModule loopPowerTouchModule;

// =========================================================================
// Extern declarations — core subsystem instances
// =========================================================================

extern V1BLEClient bleClient;
extern PacketParser parser;
extern TouchHandler touchHandler;
extern AutoPushModule autoPushModule;
extern TapGestureModule tapGestureModule;
extern AlertPersistenceModule alertPersistenceModule;
extern DisplayMode displayMode;
extern BleQueueModule bleQueueModule;
extern SystemEventBus systemEventBus;
extern MainRuntimeState mainRuntimeState;

#ifndef UNIT_TEST
extern DisplayPipelineModule displayPipelineModule;
#endif

#if defined(PERF_METRICS) && defined(PERF_MONITORING)
extern PerfLatency perfLatency;
extern bool perfDebugEnabled;
#endif

// =========================================================================
// Extern function declarations
// =========================================================================

extern void configureTouchUiModule();

#endif // MAIN_GLOBALS_H
