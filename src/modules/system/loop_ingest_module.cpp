#include "loop_ingest_module.h"

void LoopIngestModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void LoopIngestModule::reset() {}

LoopIngestResult LoopIngestModule::process(const LoopIngestContext& ctx) {
    LoopIngestResult result;

    if (ctx.bleProcessEnabled) {
        if (ctx.runBleProcess) {
            if (providers.timestampUs && providers.recordBleProcessUs) {
                const uint32_t startUs = providers.timestampUs(providers.timestampContext);
                ctx.runBleProcess();
                providers.recordBleProcessUs(
                    providers.bleProcessPerfContext,
                    providers.timestampUs(providers.timestampContext) - startUs);
            } else {
                ctx.runBleProcess();
            }
        } else if (providers.runBleProcess) {
            if (providers.timestampUs && providers.recordBleProcessUs) {
                const uint32_t startUs = providers.timestampUs(providers.timestampContext);
                providers.runBleProcess(providers.bleProcessContext);
                providers.recordBleProcessUs(
                    providers.bleProcessPerfContext,
                    providers.timestampUs(providers.timestampContext) - startUs);
            } else {
                providers.runBleProcess(providers.bleProcessContext);
            }
        }
    }

    if (ctx.runBleDrain) {
        if (providers.timestampUs && providers.recordBleDrainUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            ctx.runBleDrain();
            providers.recordBleDrainUs(
                providers.bleDrainPerfContext,
                providers.timestampUs(providers.timestampContext) - startUs);
        } else {
            ctx.runBleDrain();
        }
    } else if (providers.runBleDrain) {
        if (providers.timestampUs && providers.recordBleDrainUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            providers.runBleDrain(providers.bleDrainContext);
            providers.recordBleDrainUs(
                providers.bleDrainPerfContext,
                providers.timestampUs(providers.timestampContext) - startUs);
        } else {
            providers.runBleDrain(providers.bleDrainContext);
        }
    }

    if (providers.readBleBackpressure) {
        result.bleBackpressure = providers.readBleBackpressure(providers.bleBackpressureContext);
    }
    result.skipLateNonCoreThisLoop = ctx.skipNonCoreThisLoop || result.bleBackpressure;
    result.overloadLateThisLoop = ctx.overloadThisLoop || result.bleBackpressure;

    if (providers.runGpsRuntimeUpdate) {
        if (providers.timestampUs && providers.recordGpsUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            providers.runGpsRuntimeUpdate(providers.gpsRuntimeContext, ctx.nowMs);
            providers.recordGpsUs(
                providers.gpsPerfContext,
                providers.timestampUs(providers.timestampContext) - startUs);
        } else {
            providers.runGpsRuntimeUpdate(providers.gpsRuntimeContext, ctx.nowMs);
        }
    }

    return result;
}
