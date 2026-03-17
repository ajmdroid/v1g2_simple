#include "wifi_runtime_module.h"

void WifiRuntimeModule::begin(const Providers& hooks) {
    providers = hooks;
}

WifiRuntimeResult WifiRuntimeModule::process(const WifiRuntimeContext& ctx) {
    WifiRuntimeResult result;
    result.wifiAutoStartDone = ctx.wifiAutoStartDone;

    if (providers.runWifiAutoStartProcess) {
        providers.runWifiAutoStartProcess(providers.wifiAutoStartContext,
                                          ctx.nowMs,
                                          ctx.v1ConnectedAtMs,
                                          ctx.enableWifi,
                                          ctx.enableWifiAtBoot,
                                          ctx.bleConnected,
                                          ctx.canStartDma,
                                          result.wifiAutoStartDone);
    }

    if (!ctx.skipLateNonCoreThisLoop &&
        providers.shouldRunWifiProcessingPolicy &&
        providers.shouldRunWifiProcessingPolicy(
            providers.wifiPolicyContext, ctx.enableWifi, ctx.enableWifiAtBoot, result.wifiAutoStartDone) &&
        providers.runWifiCadence &&
        providers.runWifiManagerProcess) {
        WifiProcessCadenceContext wifiCadenceCtx;
        if (providers.perfTimestampUs) {
            wifiCadenceCtx.nowProcessUs = providers.perfTimestampUs(providers.perfContext);
        }
        wifiCadenceCtx.minIntervalUs = WIFI_PROCESS_MIN_INTERVAL_US;
        const WifiProcessCadenceDecision wifiCadenceDecision =
            providers.runWifiCadence(providers.wifiCadenceContext, wifiCadenceCtx);
        if (wifiCadenceDecision.shouldRunProcess) {
            if (providers.perfTimestampUs && providers.recordWifiProcessUs) {
                const uint32_t wifiStartUs = providers.perfTimestampUs(providers.perfContext);
                providers.runWifiManagerProcess(providers.wifiManagerProcessContext);
                providers.recordWifiProcessUs(
                    providers.wifiProcessPerfContext,
                    providers.perfTimestampUs(providers.perfContext) - wifiStartUs);
            } else {
                providers.runWifiManagerProcess(providers.wifiManagerProcessContext);
            }
        }
    }

    if (providers.runWifiVisualSync &&
        providers.readWifiServiceActive &&
        providers.readWifiConnected) {
        const bool wifiVisualActiveNow =
            providers.readWifiServiceActive(providers.wifiServiceContext) ||
            providers.readWifiConnected(providers.wifiConnectedContext);
        uint32_t visualNowMs = ctx.nowMs;
        if (providers.readVisualNowMs) {
            visualNowMs = providers.readVisualNowMs(providers.visualNowContext);
        }
        providers.runWifiVisualSync(providers.wifiVisualSyncContext,
                                    visualNowMs,
                                    wifiVisualActiveNow,
                                    ctx.displayPreviewRunning,
                                    ctx.bootSplashHoldActive);
    }

    return result;
}
