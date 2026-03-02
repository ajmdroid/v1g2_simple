#include "loop_post_display_module.h"

void LoopPostDisplayModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void LoopPostDisplayModule::reset() {}

LoopPostDisplayResult LoopPostDisplayModule::process(const LoopPostDisplayContext& ctx) {
    LoopPostDisplayResult result;
    result.dispatchNowMs = ctx.nowMs;
    result.bleConnectedNow = ctx.bleConnectedNow;

    if (ctx.enableAutoPush) {
        if (ctx.runAutoPush) {
            ctx.runAutoPush();
        } else if (providers.runAutoPush) {
            providers.runAutoPush(providers.autoPushContext);
        }
    }

    if (ctx.runSpeedAndDispatch) {
        const uint32_t dispatchNowMs = providers.readDispatchNowMs
                                           ? providers.readDispatchNowMs(providers.dispatchNowContext)
                                           : ctx.nowMs;
        const bool bleConnectedNow = providers.readBleConnectedNow
                                         ? providers.readBleConnectedNow(providers.bleConnectedContext)
                                         : ctx.bleConnectedNow;

        ConnectionStateDispatchContext dispatchCtx;
        dispatchCtx.nowMs = dispatchNowMs;
        dispatchCtx.displayUpdateIntervalMs = ctx.displayUpdateIntervalMs;
        dispatchCtx.scanScreenDwellMs = ctx.scanScreenDwellMs;
        dispatchCtx.bleConnectedNow = bleConnectedNow;
        dispatchCtx.bootSplashHoldActive = ctx.bootSplashHoldActive;
        dispatchCtx.displayPreviewRunning = ctx.displayPreviewRunning;
        dispatchCtx.maxProcessGapMs = ctx.maxProcessGapMs;

        if (ctx.runConnectionStateDispatch) {
            ctx.runConnectionStateDispatch(dispatchCtx);
        } else if (providers.runConnectionStateDispatch) {
            providers.runConnectionStateDispatch(providers.connectionDispatchContext, dispatchCtx);
        }

        result.dispatchNowMs = dispatchNowMs;
        result.bleConnectedNow = bleConnectedNow;
    }

    return result;
}
