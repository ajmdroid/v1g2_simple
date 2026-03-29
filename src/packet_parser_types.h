#pragma once

#include <stdint.h>
#include <math.h>   // NAN

/**
 * Shared data types for the V1 Gen2 packet parser.
 *
 * Pure data — no Arduino dependency, safe to include in native unit tests.
 * Both the real PacketParser header and all test mocks must include this file
 * rather than redefining these types locally.
 */

// ---------------------------------------------------------------------------
// Enumerations
// ---------------------------------------------------------------------------

enum Band {
    BAND_NONE   = 0,
    BAND_LASER  = 1 << 0,
    BAND_KA     = 1 << 1,
    BAND_K      = 1 << 2,
    BAND_X      = 1 << 3
};

enum Direction {
    DIR_NONE    = 0,
    DIR_FRONT   = 1,
    DIR_SIDE    = 2,
    DIR_REAR    = 4
};

// ---------------------------------------------------------------------------
// AlertData
// ---------------------------------------------------------------------------

struct AlertData {
    Band band;
    Direction direction;
    uint8_t frontStrength;  // 0-6 (V1 Gen2 uses 6 bars)
    uint8_t rearStrength;   // 0-6
    uint32_t frequency;     // MHz
    bool isValid;
    bool isPriority;        // aux0 bit 7 — V1's priority flag
    bool isJunk;            // aux0 bit 6 — junked alert
    uint8_t photoType;      // aux0 bits 0..3 — photo type (V4.1037+)
    uint8_t rawBandBits;    // bandArrow low 5 bits (VR-style raw band field)
    bool isKu;              // True when rawBandBits resolves to Ku (0x10)

    AlertData()
        : band(BAND_NONE), direction(DIR_NONE),
          frontStrength(0), rearStrength(0),
          frequency(0), isValid(false), isPriority(false),
          isJunk(false), photoType(0), rawBandBits(0), isKu(false) {}

    // Convenience factory — preferred for constructing test fixtures and
    // one-shot alert values without named temporaries.
    static AlertData create(Band b, Direction d, uint8_t front, uint8_t rear,
                            uint32_t freq, bool valid = true, bool priority = false) {
        AlertData a;
        a.band          = b;
        a.direction     = d;
        a.frontStrength = front;
        a.rearStrength  = rear;
        a.frequency     = freq;
        a.isValid       = valid;
        a.isPriority    = priority;
        return a;
    }
};

// ---------------------------------------------------------------------------
// DisplayState
// ---------------------------------------------------------------------------

struct DisplayState {
    uint8_t activeBands;        // Bitmap of active bands
    Direction arrows;           // Bitmap of arrow directions (all active, from display packet)
    Direction priorityArrow;    // Arrow from V1's priority alert (alerts[0])
    uint8_t signalBars;         // 0-8 signal strength (from V1's LED bitmap)
    bool muted;
    bool systemTest;
    char modeChar;
    bool hasMode;
    bool displayOn;             // True if main display is ON (not dark)
    bool hasDisplayOn;          // True if we've seen explicit on/off ack
    uint8_t flashBits;          // Blink state for arrows (from display packet)
    uint8_t bandFlashBits;      // Blink state for bands (L=0x01, Ka=0x02, K=0x04, X=0x08)
    uint8_t mainVolume;         // Main volume 0-9
    uint8_t muteVolume;         // Muted volume 0-9
    uint32_t v1FirmwareVersion; // V1 firmware version as integer (e.g. 41028 for 4.1028)
    bool hasV1Version;          // True if we've received version from V1
    bool hasVolumeData;         // True if we've received volume data in display packet
    uint8_t v1PriorityIndex;    // Resolved priority alert index for current table (0-based)
    uint8_t bogeyCounterByte;   // Raw 7-segment byte from V1 display
    char bogeyCounterChar;      // Decoded character from bogeyCounterByte
    bool bogeyCounterDot;       // Decimal point from bogeyCounterByte (bit 7)
    bool hasJunkAlert;          // True if any alert row has aux0 junk bit set
    bool hasPhotoAlert;         // True if any alert row has photo type > 0

    DisplayState()
        : activeBands(BAND_NONE), arrows(DIR_NONE), priorityArrow(DIR_NONE),
          signalBars(0), muted(false), systemTest(false),
          modeChar(0), hasMode(false), displayOn(true), hasDisplayOn(false),
          flashBits(0), bandFlashBits(0), mainVolume(0), muteVolume(0),
          v1FirmwareVersion(0), hasV1Version(false), hasVolumeData(false),
          v1PriorityIndex(0), bogeyCounterByte(0), bogeyCounterChar('0'),
          bogeyCounterDot(false), hasJunkAlert(false), hasPhotoAlert(false) {}

    // True if we should show the volume display.
    // Requires either observed volume data OR confirmed firmware 4.1028+.
    bool supportsVolume() const {
        return hasVolumeData || (hasV1Version && v1FirmwareVersion >= 41028);
    }
};
