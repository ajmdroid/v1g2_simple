/**
 * Configuration file for V1 Gen2 Simple Display
 * Waveshare ESP32-S3-Touch-LCD-3.49 (AXS15231B, 640x172)
 *
 * This file contains:
 * - Firmware version and board definitions
 * - BLE UUIDs for V1 Gen2 protocol
 * - ESP packet framing constants
 * - Display layout and timing parameters
 */

#pragma once
#ifndef CONFIG_H
#define CONFIG_H

// Include display driver abstraction
#include "display_driver.h"

// Firmware Version
#define FIRMWARE_VERSION "4.0.0-dev"

// Board-specific Display
// Waveshare ESP32-S3-Touch-LCD-3.49
#define DISPLAY_ROTATION    1     // Landscape mode (90 degree rotation)

// BLE Configuration
#define V1_SERVICE_UUID         "92A0AFF4-9E05-11E2-AA59-F23C91AEC05E"
#define V1_DISPLAY_DATA_UUID    "92A0B2CE-9E05-11E2-AA59-F23C91AEC05E"  // V1 out SHORT (notify) - display data
#define V1_DISPLAY_DATA_LONG_UUID "92A0B4E0-9E05-11E2-AA59-F23C91AEC05E"  // V1 out LONG (notify) - alert data, voltage responses
#define V1_COMMAND_WRITE_UUID   "92A0B6D4-9E05-11E2-AA59-F23C91AEC05E"  // Client out, V1 in
#define V1_COMMAND_WRITE_ALT_UUID "92A0BAD4-9E05-11E2-AA59-F23C91AEC05E" // Alternate writable characteristic
#define V1_COMMAND_WRITE_LONG_UUID "92A0B8D2-9E05-11E2-AA59-F23C91AEC05E" // Long writable characteristic (optional)
// 16-bit short UUIDs extracted from the V1 128-bit UUIDs above (bytes 4-5 of the string form)
#define V1_SHORT_UUID_DISPLAY_LONG  ((uint16_t)0xB4E0) // Short UUID for V1_DISPLAY_DATA_LONG_UUID
#define V1_SHORT_UUID_COMMAND_LONG  ((uint16_t)0xB8D2) // Short UUID for V1_COMMAND_WRITE_LONG_UUID
#define SCAN_DURATION           10000 // 10-second scan in milliseconds (stops early when V1 found)
#define RECONNECT_DELAY         100   // 100ms delay between scan attempts

// ESP Packet Constants
#define ESP_PACKET_START        0xAA
#define ESP_PACKET_END          0xAB
#define ESP_PACKET_ORIGIN_V1    0x0A  // V1 with checksum
#define ESP_PACKET_DEST_V1      0x0A
#define ESP_PACKET_REMOTE       0x06  // V1connection type (remote app) - was 0x04

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

// Timing
#define DISPLAY_UPDATE_MS   50    // Update display every 50ms (snappier response)

// Development/Testing Features
// Uncomment to enable packet replay mode for UI testing without BLE
// #define REPLAY_MODE

#endif // CONFIG_H
