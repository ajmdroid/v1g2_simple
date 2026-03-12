#include "loop_runtime_snapshot_module.h"

void LoopRuntimeSnapshotModule::begin(const Providers& hooks) {
    providers = hooks;
}

LoopRuntimeSnapshotValues LoopRuntimeSnapshotModule::process(
    const LoopRuntimeSnapshotContext& ctx) {
    LoopRuntimeSnapshotValues values;

    if (ctx.readBleConnected) {
        values.bleConnected = ctx.readBleConnected();
    } else if (providers.readBleConnected) {
        values.bleConnected = providers.readBleConnected(providers.bleConnectedContext);
    }

    if (ctx.readCanStartDma) {
        values.canStartDma = ctx.readCanStartDma();
    } else if (providers.readCanStartDma) {
        values.canStartDma = providers.readCanStartDma(providers.canStartDmaContext);
    }

    if (ctx.readDisplayPreviewRunning) {
        values.displayPreviewRunning = ctx.readDisplayPreviewRunning();
    } else if (providers.readDisplayPreviewRunning) {
        values.displayPreviewRunning =
            providers.readDisplayPreviewRunning(providers.displayPreviewContext);
    }

    return values;
}
