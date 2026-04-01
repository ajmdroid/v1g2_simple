#pragma once

#include "quiet_coordinator_module.h"

template <typename SpeedMuteLike>
void QuietCoordinatorModule::applyVoicePresentation(VoiceContext& voiceCtx,
                                                    const SpeedMuteLike* speedMute,
                                                    const bool hasRenderablePriority,
                                                    const uint8_t priorityBand) {
    syncCommittedState();

    presentation_.voiceSuppressed = false;
    presentation_.voiceAllowVolZeroBypass = false;
    voiceCtx.isSuppressed = false;

    if (!voiceCtx.isSuppressed && speedMute) {
        const auto& smState = speedMute->getState();
        if (smState.muteActive && hasRenderablePriority) {
            if (!speedMute->isBandOverridden(priorityBand)) {
                voiceCtx.isSuppressed = true;
                presentation_.voiceSuppressed = true;
            }
        }
    }

    if (speedMute && hasRenderablePriority) {
        const auto& smState = speedMute->getState();
        if (smState.muteActive && speedMute->isBandOverridden(priorityBand)) {
            voiceCtx.isMuted = false;
            if (voiceCtx.mainVolume == 0) {
                voiceCtx.mainVolume = 1;
            }
            presentation_.voiceAllowVolZeroBypass = true;
        }
    }
}
