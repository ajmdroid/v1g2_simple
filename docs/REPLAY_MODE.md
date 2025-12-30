# Packet Replay Mode

## Overview

REPLAY_MODE is a development/testing feature that simulates V1 radar detector packets without requiring a physical BLE connection. This allows rapid UI development and testing without hardware.

## Enabling Replay Mode

1. Open `include/config.h`
2. Uncomment the REPLAY_MODE define:
   ```cpp
   // Development/Testing Features
   // Uncomment to enable packet replay mode for UI testing without BLE
   #define REPLAY_MODE
   ```
3. Build and upload firmware: `pio run -t upload`

## What It Does

When enabled, REPLAY_MODE:
- **Disables BLE**: No scanning, no connection attempts
- **Injects test packets**: Simulates V1 packet sequences at realistic timing
- **Exercises UI**: Display updates, alert processing, signal bars, etc.
- **Loops continuously**: Sequence repeats for persistent testing

## Packet Sequences

The default replay sequence includes:

1. **Clear state** (2s) - No alerts
2. **Ka alert** (100ms) - Front, 33.800 GHz, strength 5
3. **Muted display** (1s) - Ka active, 3 signal bars, muted
4. **Clear** (1.5s)
5. **X band** (100ms) - Front, 4 signal bars
6. **Clear** (2s)
7. **K alert** (100ms) - Rear, 24.150 GHz, strength 3
8. **Clear** (1.5s)
9. **Laser alert** (100ms) - Front, max strength
10. **Clear** (3s) - Loop back to start

Total cycle time: ~11 seconds

## Memory Impact

REPLAY_MODE reduces memory usage by excluding BLE stack:
- **RAM**: -5.9KB (18.6% → 16.8%)
- **Flash**: -183KB (32.7% → 29.9%)

## Adding Custom Packets

Edit `src/main.cpp` in the `#ifdef REPLAY_MODE` section:

```cpp
// Add your packet definition
const uint8_t REPLAY_PACKET_CUSTOM[] = {
    0xAA, 0x04, 0x0A, 0x43, 0x0C,  // Header
    // ... your packet data ...
    0xXX, 0xAB                      // Checksum, end
};

// Add to sequence
const ReplayPacket replaySequence[] = {
    // ... existing packets ...
    {REPLAY_PACKET_CUSTOM, sizeof(REPLAY_PACKET_CUSTOM), 500},
};
```

## Packet Format

V1 packets follow this structure:
```
0xAA                    - Start byte
<dest>                  - Destination (0x04 for V1)
<origin>                - Origin (0x0A for controller)
<packetID>              - Packet type (0x31=display, 0x43=alert)
<len>                   - Payload length
<payload...>            - Data bytes
<checksum>              - XOR checksum
0xAB                    - End byte
```

## Common Packet Types

- **0x31 (Display Data)**: Band indicators, signal bars, mute status
- **0x43 (Alert Data)**: Band, frequency, direction, strength

## Disabling

To return to normal BLE operation:
1. Open `include/config.h`
2. Comment out the define:
   ```cpp
   // #define REPLAY_MODE
   ```
3. Rebuild and upload

## Use Cases

- **UI development**: Test display logic without hardware
- **Alert handling**: Verify alert logger, database
- **Color themes**: Test appearance across scenarios
- **Touch response**: Validate swipe/mute interactions
- **Web interface**: Check live status updates
- **Performance**: Profile display refresh timing

## Limitations

- **No BLE proxy**: JBV1 forwarding disabled
- **No actual scanning**: Web UI shows "scanning" but nothing happens
- **Fixed sequences**: Real-world variability not captured
- **No user input**: Cannot test user-initiated V1 commands

## Notes

- Replay timing uses `millis()` for non-blocking operation
- Packets inject into same `rxBuffer` as real BLE data
- PacketParser processes replayed data identically to BLE
- Serial monitor shows `[REPLAY]` log messages for debugging
