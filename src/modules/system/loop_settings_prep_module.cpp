#include "loop_settings_prep_module.h"

void LoopSettingsPrepModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void LoopSettingsPrepModule::reset() {}

LoopSettingsPrepValues LoopSettingsPrepModule::process(const LoopSettingsPrepContext& ctx) {
    if (ctx.runTapGesture) {
        ctx.runTapGesture(ctx.nowMs);
    } else if (providers.runTapGesture) {
        providers.runTapGesture(providers.tapGestureContext, ctx.nowMs);
    }

    if (ctx.readSettingsValues) {
        return ctx.readSettingsValues();
    }
    if (providers.readSettingsValues) {
        return providers.readSettingsValues(providers.settingsContext);
    }
    return LoopSettingsPrepValues{};
}
