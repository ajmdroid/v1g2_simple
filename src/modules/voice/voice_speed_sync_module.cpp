#include "voice_speed_sync_module.h"

void VoiceSpeedSyncModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void VoiceSpeedSyncModule::reset() {}

void VoiceSpeedSyncModule::process(const VoiceSpeedSyncContext& ctx) {
    if (!providers.selectSpeedSample) {
        return;
    }

    SpeedSelection selection;
    if (providers.selectSpeedSample(providers.speedSelectorContext, ctx.nowMs, selection)) {
        if (providers.updateVoiceSpeedSample) {
            providers.updateVoiceSpeedSample(
                providers.voiceContext, selection.speedMph, selection.timestampMs);
        }
        return;
    }

    if (providers.clearVoiceSpeedSample) {
        providers.clearVoiceSpeedSample(providers.voiceContext);
    }
}
