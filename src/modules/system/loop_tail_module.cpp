#include "loop_tail_module.h"

void LoopTailModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void LoopTailModule::reset() {}

uint32_t LoopTailModule::process(bool bleBackpressure, uint32_t loopStartUs) {
    if (bleBackpressure) {
        uint32_t drainStartUs = 0;
        if (providers.perfTimestampUs) {
            drainStartUs = providers.perfTimestampUs(providers.perfTimestampContext);
        }

        if (providers.runBleDrain) {
            providers.runBleDrain(providers.bleDrainContext);
        }

        if (providers.recordBleDrainUs && providers.perfTimestampUs) {
            const uint32_t elapsedUs = static_cast<uint32_t>(
                providers.perfTimestampUs(providers.perfTimestampContext) - drainStartUs);
            providers.recordBleDrainUs(providers.bleDrainRecordContext, elapsedUs);
        }
    }

    if (providers.yieldOneTick) {
        providers.yieldOneTick(providers.yieldContext);
    }

    if (providers.loopMicrosUs) {
        return static_cast<uint32_t>(providers.loopMicrosUs(providers.loopMicrosContext) - loopStartUs);
    }

    return 0;
}
