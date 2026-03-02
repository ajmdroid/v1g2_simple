#include "loop_pre_ingest_module.h"

void LoopPreIngestModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void LoopPreIngestModule::reset() {}

LoopPreIngestResult LoopPreIngestModule::process(const LoopPreIngestContext& ctx) {
    LoopPreIngestResult result;
    result.bootReady = ctx.bootReady;

    if (!ctx.replayMode) {
        if (!result.bootReady && ctx.nowMs >= ctx.bootReadyDeadlineMs) {
            result.bootReady = true;
            result.bootReadyOpenedByTimeout = true;
            if (ctx.openBootReadyGate) {
                ctx.openBootReadyGate(ctx.nowMs);
            } else if (providers.openBootReadyGate) {
                providers.openBootReadyGate(providers.bootReadyContext, ctx.nowMs);
            }
        }

        if (ctx.runWifiPriorityApply) {
            ctx.runWifiPriorityApply(ctx.nowMs);
        } else if (providers.runWifiPriorityApply) {
            providers.runWifiPriorityApply(
                providers.wifiPriorityContext,
                ctx.nowMs);
        }

        result.runBleProcessThisLoop = true;
    }

    if (ctx.runDebugApiProcess) {
        ctx.runDebugApiProcess(ctx.nowMs);
    } else if (providers.runDebugApiProcess) {
        providers.runDebugApiProcess(providers.debugApiContext, ctx.nowMs);
    }

    return result;
}
