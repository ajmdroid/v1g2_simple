#include "speed_volume_runtime_module.h"

void SpeedVolumeRuntimeModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void SpeedVolumeRuntimeModule::reset() {}

void SpeedVolumeRuntimeModule::process(const SpeedVolumeRuntimeContext& ctx) {
    if (providers.runSpeedVolumeProcess) {
        providers.runSpeedVolumeProcess(providers.speedVolumeContext, ctx.nowMs);
    }

    if (providers.runSpeakerQuietSync &&
        providers.readSpeedQuietActive &&
        providers.readSpeedQuietVolume) {
        const bool quietNow = providers.readSpeedQuietActive(providers.speedQuietActiveContext);
        const uint8_t quietVolume = providers.readSpeedQuietVolume(providers.speedQuietVolumeContext);
        providers.runSpeakerQuietSync(
            providers.speakerQuietContext, quietNow, quietVolume, ctx.configuredVoiceVolume);
    }
}
