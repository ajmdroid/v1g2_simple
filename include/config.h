/**
 * Configuration file for V1 Gen2 Simple Display
 * Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, 640x172)
 */

#ifndef CONFIG_H
#define CONFIG_H

// Include display driver abstraction
#include "display_driver.h"

// Firmware Version
#define FIRMWARE_VERSION "1.0.0"

// Board-specific Power Management (Waveshare only)
// Waveshare ESP32-S3-Touch-LCD-3.49
#define PIN_POWER_ON    -1  // No dedicated power pin (check schematic)
#define DISPLAY_ROTATION    1     // Landscape mode (90 degree rotation)

// Default Brightness
#define DEFAULT_BRIGHTNESS  200   // 0-255

// BLE Configuration
#define V1_SERVICE_UUID         "92A0AFF4-9E05-11E2-AA59-F23C91AEC05E"
#define V1_DISPLAY_DATA_UUID    "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E"  // V1 out, client in (notify)
#define V1_COMMAND_WRITE_UUID   "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E"  // Client out, V1 in
#define V1_COMMAND_WRITE_ALT_UUID "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E" // Alternate writable characteristic
#define SCAN_DURATION           10    // 10-second scan (stops early when V1 found)
#define RECONNECT_DELAY         100   // 100ms delay between scan attempts

// ESP Packet Constants
#define ESP_PACKET_START        0xAA
#define ESP_PACKET_END          0xAB
#define ESP_PACKET_ORIGIN_V1    0x0A  // V1 with checksum
#define ESP_PACKET_DEST_V1      0x0A
#define ESP_PACKET_REMOTE       0x04  // Third-party device

// Packet IDs
#define PACKET_ID_DISPLAY_DATA  0x31  // infDisplayData
#define PACKET_ID_ALERT_DATA    0x43  // respAlertData
#define PACKET_ID_REQ_START_ALERT 0x41 // reqStartAlertData (matches T4S3 logic)
#define PACKET_ID_VERSION       0x01  // reqVersion
#define PACKET_ID_TURN_OFF_DISPLAY 0x32  // reqTurnOffMainDisplay (dark mode)
#define PACKET_ID_TURN_ON_DISPLAY  0x33  // reqTurnOnMainDisplay
#define PACKET_ID_MUTE_ON       0x34  // reqMuteOn
#define PACKET_ID_MUTE_OFF      0x35  // reqMuteOff
#define PACKET_ID_REQ_WRITE_VOLUME      0x39  // reqWriteVolume (mainVolume, mutedVolume, aux0)
#define PACKET_ID_REQ_USER_BYTES   0x11  // reqUserBytes
#define PACKET_ID_RESP_USER_BYTES  0x12  // respUserBytes
#define PACKET_ID_WRITE_USER_BYTES 0x13  // reqWriteUserBytes

// Display Layout (pixels in landscape 640x172)
#define BAND_Y              20
#define BAND_SPACING        80
#define ARROW_Y             70
#define BARS_Y              110
#define BAR_WIDTH           30
#define BAR_HEIGHT          40
#define BAR_SPACING         5
#define MAX_SIGNAL_BARS     6

// Timing
#define DISPLAY_UPDATE_MS   100   // Update display every 100ms
#define STATUS_UPDATE_MS    1000  // Update status indicators every second

// Development/Testing Features
// Uncomment to enable packet replay mode for UI testing without BLE
// #define REPLAY_MODE

#endif // CONFIG_H
