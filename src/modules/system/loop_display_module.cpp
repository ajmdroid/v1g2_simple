#include "loop_display_module.h"

void LoopDisplayModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void LoopDisplayModule::reset() {}

LoopDisplayResult LoopDisplayModule::process(const LoopDisplayContext& ctx) {
    LoopDisplayResult result;

    const uint32_t displayNowMs =
        providers.readDisplayNowMs ? providers.readDisplayNowMs(providers.displayNowContext) : ctx.nowMs;

    ParsedFrameSignal parsedSignal;
    if (providers.collectParsedSignal) {
        parsedSignal = providers.collectParsedSignal(providers.parsedSignalContext);
    }

    DisplayOrchestrationParsedResult parsedResult;
    DisplayOrchestrationParsedContext parsedCtx;
    parsedCtx.nowMs = displayNowMs;
    parsedCtx.parsedReady = parsedSignal.parsedReady;
    parsedCtx.bootSplashHoldActive = ctx.bootSplashHoldActive;
    parsedCtx.enableSignalTraceLogging = ctx.enableSignalTraceLogging;

    if (providers.runParsedFrame) {
        if (providers.timestampUs && providers.recordLockoutUs) {
            const uint32_t startUs = providers.timestampUs(providers.timestampContext);
            parsedResult = providers.runParsedFrame(providers.parsedFrameContext, parsedCtx);
            if (parsedResult.lockoutEvaluated) {
                providers.recordLockoutUs(
                    providers.lockoutPerfContext,
                    providers.timestampUs(providers.timestampContext) - startUs);
            }
        } else {
            parsedResult = providers.runParsedFrame(providers.parsedFrameContext, parsedCtx);
        }
    }

    bool pipelineRanThisLoop = false;
    if (parsedResult.runDisplayPipeline) {
        if (providers.recordNotifyToDisplayMs &&
            parsedSignal.parsedTsMs != 0 &&
            displayNowMs >= parsedSignal.parsedTsMs) {
            providers.recordNotifyToDisplayMs(
                providers.notifyPerfContext,
                displayNowMs - parsedSignal.parsedTsMs);
        }

        if (ctx.runDisplayPipeline) {
            if (providers.timestampUs && providers.recordDispPipeUs) {
                const uint32_t startUs = providers.timestampUs(providers.timestampContext);
                ctx.runDisplayPipeline(displayNowMs, parsedResult.lockoutPrioritySuppressed);
                providers.recordDispPipeUs(
                    providers.dispPipePerfContext,
                    providers.timestampUs(providers.timestampContext) - startUs);
            } else {
                ctx.runDisplayPipeline(displayNowMs, parsedResult.lockoutPrioritySuppressed);
            }
        } else if (providers.runDisplayPipeline) {
            if (providers.timestampUs && providers.recordDispPipeUs) {
                const uint32_t startUs = providers.timestampUs(providers.timestampContext);
                providers.runDisplayPipeline(
                    providers.displayPipelineContext,
                    displayNowMs,
                    parsedResult.lockoutPrioritySuppressed);
                providers.recordDispPipeUs(
                    providers.dispPipePerfContext,
                    providers.timestampUs(providers.timestampContext) - startUs);
            } else {
                providers.runDisplayPipeline(
                    providers.displayPipelineContext,
                    displayNowMs,
                    parsedResult.lockoutPrioritySuppressed);
            }
        }
        pipelineRanThisLoop = true;
    }

    if (providers.runLightweightRefresh) {
        DisplayOrchestrationRefreshContext refreshCtx;
        refreshCtx.nowMs = displayNowMs;
        refreshCtx.bootSplashHoldActive = ctx.bootSplashHoldActive;
        refreshCtx.overloadLateThisLoop = ctx.overloadLateThisLoop;
        refreshCtx.pipelineRanThisLoop = pipelineRanThisLoop;
        refreshCtx.cameraAlertActive =
            providers.readCameraAlertActive &&
            providers.readCameraAlertActive(providers.cameraAlertContext);
        const auto refreshResult =
            providers.runLightweightRefresh(providers.lightweightRefreshContext, refreshCtx);
        result.signalPriorityActive = refreshResult.signalPriorityActive;
    }

    return result;
}
