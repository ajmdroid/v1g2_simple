#include "speaker_quiet_sync_module.h"

void SpeakerQuietSyncModule::reset() {
    speakerQuietActive_ = false;
    speakerOriginalVolume_ = 0;
}

void SpeakerQuietSyncModule::process(bool quietNow,
                                     uint8_t quietVolume,
                                     uint8_t configuredVoiceVolume,
                                     const std::function<void(uint8_t)>& setSpeakerVolume) {
    if (quietNow && !speakerQuietActive_) {
        // Entering low-speed quiet: scale speaker volume from configured baseline.
        speakerOriginalVolume_ = configuredVoiceVolume;
        uint8_t scaled = (quietVolume == 0)
                             ? 0
                             : static_cast<uint8_t>((static_cast<uint16_t>(speakerOriginalVolume_) *
                                                     quietVolume) /
                                                    9u);
        if (setSpeakerVolume) {
            setSpeakerVolume(scaled);
        }
        speakerQuietActive_ = true;
    } else if (!quietNow && speakerQuietActive_) {
        // Exiting quiet: restore current configured voice volume.
        if (setSpeakerVolume) {
            setSpeakerVolume(configuredVoiceVolume);
        }
        speakerQuietActive_ = false;
    }
}
