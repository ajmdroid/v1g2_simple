#pragma once

#include <Arduino.h>

#include <functional>

// Owns speaker quiet-entry/exit synchronization with speed-volume quiet mode.
class SpeakerQuietSyncModule {
public:
    void reset();

    void process(bool quietNow,
                 uint8_t quietVolume,
                 uint8_t configuredVoiceVolume,
                 const std::function<void(uint8_t)>& setSpeakerVolume);

    bool isQuietActive() const { return speakerQuietActive_; }
    uint8_t originalVolume() const { return speakerOriginalVolume_; }

private:
    bool speakerQuietActive_ = false;
    uint8_t speakerOriginalVolume_ = 0;
};
