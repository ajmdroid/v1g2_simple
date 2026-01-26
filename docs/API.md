# V1-Simple API Reference

Complete API documentation for the V1-Simple web interface and REST endpoints.

**Base URL**: `http://192.168.4.1` (default AP mode)  
**Content-Type**: `application/x-www-form-urlencoded` (POST) or `application/json`

---

## Table of Contents

- [Status & Info](#status--info)
- [Settings](#settings)
- [V1 Profiles](#v1-profiles)
- [Auto-Push](#auto-push)
- [Display Colors](#display-colors)
- [Lockouts](#lockouts)
- [OBD/Speedometer](#obdspeedometer)
- [GPS](#gps)
- [Camera Alerts](#camera-alerts)
- [WiFi Client](#wifi-client)
- [Debug](#debug)
- [Audio](#audio)

---

## Status & Info

### GET /api/status

Get device status including V1 connection, GPS, and alerts.

**Response:**
```json
{
  "v1Connected": true,
  "bleRssi": -65,
  "batteryPercent": 85,
  "batteryCharging": false,
  "gps": {
    "fix": true,
    "lat": 37.7749,
    "lon": -122.4194,
    "speed": 45.5,
    "heading": 180,
    "satellites": 12,
    "hdop": 1.2
  },
  "alerts": [...],
  "muted": false,
  "displayStyle": 0,
  "freeHeap": 180000,
  "uptime": 3600
}
```

### GET /ping

Health check endpoint.

**Response:** `OK` (text/plain)

---

## Settings

### GET /api/settings

Get all device settings.

**Response:**
```json
{
  "ap_ssid": "V1-Simple",
  "ap_password": "********",
  "isDefaultPassword": true,
  "proxy_ble": false,
  "proxy_name": "",
  "displayStyle": 0,
  "autoPowerOffMinutes": 0,
  "gpsEnabled": true,
  "obdEnabled": true,
  "lockoutEnabled": true,
  "lockoutKaProtection": true,
  "lockoutDirectionalUnlearn": false,
  "lockoutFreqToleranceMHz": 5,
  "lockoutLearnCount": 2,
  "lockoutUnlearnCount": 3,
  "lockoutManualDeleteCount": 3,
  "lockoutLearnIntervalHours": 24,
  "lockoutUnlearnIntervalHours": 168,
  "lockoutMaxSignalStrength": 6,
  "lockoutMaxDistanceM": 50,
  "cameraAlertsEnabled": false,
  "cameraAudioEnabled": true,
  "cameraAlertDistanceM": 500,
  "enableWifiAtBoot": true,
  "enableDebugLogging": false,
  "logAlerts": false,
  "logWifi": false,
  "logBle": false,
  "logGps": false
}
```

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| `ap_ssid` | string | 1-32 chars | WiFi AP SSID |
| `ap_password` | string | 8-63 chars | WiFi AP password (masked in response) |
| `isDefaultPassword` | boolean | - | `true` if using factory default password |
| `proxy_ble` | boolean | - | Enable BLE proxy mode |
| `proxy_name` | string | 0-32 chars | Custom device name for proxy |
| `displayStyle` | int | 0-4 | Display theme (0=Classic) |
| `autoPowerOffMinutes` | int | 0-60 | Auto power off (0=disabled) |
| `lockoutEnabled` | boolean | - | Enable auto-lockout |
| `lockoutFreqToleranceMHz` | int | 1-50 | Frequency matching tolerance |
| `cameraAlertDistanceM` | int | 100-2000 | Camera alert distance in meters |

### POST /api/settings

Update device settings. Send only fields you want to change.

**Request (form data):**
```
ap_ssid=MyV1&ap_password=newpassword123&lockoutEnabled=true
```

**Response:** `Settings saved` (text/plain)

### GET /api/settings/backup

Download all settings as JSON file.

**Response:** JSON file download with Content-Disposition header

### POST /api/settings/restore

Restore settings from backup JSON file.

**Request:** JSON body with settings object

**Response:** `Settings restored successfully` (text/plain)

---

## V1 Profiles

### GET /api/v1/profiles

List all saved V1 profiles.

**Response:**
```json
{
  "profiles": ["Highway", "City", "Custom"],
  "current": "Highway"
}
```

### GET /api/v1/profile?name={name}

Get a specific profile's settings.

**Query Parameters:**
- `name` (required): Profile name

**Response:**
```json
{
  "name": "Highway",
  "settings": {
    "XBand": true,
    "KBand": true,
    "KaBand": true,
    "LaserBand": true,
    ...
  }
}
```

### POST /api/v1/profile

Create or update a V1 profile.

**Request (JSON body):**
```json
{
  "name": "Custom",
  "settings": { ... }
}
```

### POST /api/v1/profile/delete

Delete a V1 profile.

**Request (form data):** `name=CustomProfile`

### POST /api/v1/pull

Pull current settings from connected V1 device.

**Response:**
```json
{
  "success": true,
  "settings": { ... }
}
```

### POST /api/v1/push

Push a profile to the connected V1 device.

**Request (form data):** `profile=Highway`

### GET /api/v1/current

Get current V1 device settings (from last pull).

---

## Auto-Push

### GET /api/autopush/slots

Get all auto-push slot configurations.

**Response:**
```json
{
  "slots": [
    {
      "slot": 1,
      "profile": "Highway",
      "minSpeed": 45,
      "maxSpeed": 999,
      "enabled": true
    }
  ],
  "activeSlot": 1
}
```

### POST /api/autopush/slot

Configure an auto-push slot.

**Request (form data):**
```
slot=1&profile=Highway&minSpeed=45&maxSpeed=999&enabled=true
```

### POST /api/autopush/activate

Manually activate a slot.

**Request (form data):** `slot=1`

### POST /api/autopush/push

Force push current active slot profile.

### GET /api/autopush/status

Get auto-push status and last push info.

---

## Display Colors

### GET /api/displaycolors

Get current display color configuration.

**Response:**
```json
{
  "theme": "dark",
  "xColor": "#FF0000",
  "kColor": "#FFFF00",
  "kaColor": "#00FF00",
  "laserColor": "#0000FF",
  "bgColor": "#000000",
  "textColor": "#FFFFFF"
}
```

### POST /api/displaycolors

Save display color configuration.

**Request (JSON body):**
```json
{
  "xColor": "#FF0000",
  "kaColor": "#00FF00"
}
```

### POST /api/displaycolors/reset

Reset colors to default theme.

### POST /api/displaycolors/preview

Preview colors without saving (temporary display).

**Request:** Same as POST /api/displaycolors

### POST /api/displaycolors/clear

Clear preview and restore saved colors.

---

## Lockouts

### GET /api/lockouts

Get all lockout entries.

**Query Parameters:**
- `page` (optional): Page number (default 1)
- `limit` (optional): Items per page (default 100)

**Response:**
```json
{
  "lockouts": [
    {
      "id": 1,
      "freq": 34700,
      "band": "Ka",
      "lat": 37.7749,
      "lon": -122.4194,
      "direction": "Front",
      "hits": 5,
      "learned": true,
      "lastSeen": "2024-01-15T10:30:00Z"
    }
  ],
  "total": 150,
  "page": 1
}
```

### POST /api/lockouts/add

Manually add a lockout.

**Request (JSON body):**
```json
{
  "freq": 34700,
  "lat": 37.7749,
  "lon": -122.4194,
  "band": "Ka"
}
```

### POST /api/lockouts/delete

Delete a specific lockout.

**Request (form data):** `id=123`

### POST /api/lockouts/clear

Clear all lockouts.

### GET /api/lockouts/export

Export lockouts as CSV or JSON.

**Query Parameters:**
- `format`: `csv` or `json` (default: json)

### POST /api/lockouts/import

Import lockouts from CSV or JSON.

---

## OBD/Speedometer

### GET /api/obd/status

Get OBD connection status and current data.

**Response:**
```json
{
  "connected": true,
  "deviceName": "VEEPEAK",
  "rssi": -55,
  "speed": 65,
  "rpm": 2500,
  "voltage": 14.2
}
```

### POST /api/obd/scan

Start scanning for OBD devices.

### POST /api/obd/scan/stop

Stop OBD scan.

### GET /api/obd/devices

Get list of discovered OBD devices.

**Response:**
```json
{
  "devices": [
    {
      "name": "VEEPEAK",
      "address": "AA:BB:CC:DD:EE:FF",
      "rssi": -55
    }
  ]
}
```

### POST /api/obd/connect

Connect to an OBD device.

**Request (form data):** `address=AA:BB:CC:DD:EE:FF`

### POST /api/obd/forget

Forget saved OBD device.

### POST /api/obd/devices/clear

Clear discovered devices list.

---

## GPS

### GET /api/gps/status

Get GPS module status and current position.

**Response:**
```json
{
  "enabled": true,
  "fix": true,
  "fixType": "3D",
  "lat": 37.7749,
  "lon": -122.4194,
  "altitude": 15.5,
  "speed": 45.5,
  "heading": 180,
  "satellites": 12,
  "hdop": 1.2,
  "moduleType": "M10",
  "lastUpdate": "2024-01-15T10:30:00Z"
}
```

### POST /api/gps/reset

Reset GPS module (cold start).

---

## Camera Alerts

### GET /api/cameras/status

Get camera database status.

**Response:**
```json
{
  "enabled": true,
  "cameraCount": 1500,
  "lastSync": "2024-01-15T00:00:00Z",
  "databaseSize": 245000,
  "osmSyncEnabled": true
}
```

### POST /api/cameras/reload

Reload camera database from storage.

### POST /api/cameras/upload

Upload a camera database CSV file.

**Request:** multipart/form-data with `file` field

### POST /api/cameras/test

Trigger a test camera alert.

**Request (form data):** `lat=37.7749&lon=-122.4194`

### POST /api/cameras/sync-osm

Sync cameras from OpenStreetMap (requires WiFi client connection).

**Request (JSON body):**
```json
{
  "lat": 37.7749,
  "lon": -122.4194,
  "radius": 50000
}
```

| Parameter | Type | Range | Description |
|-----------|------|-------|-------------|
| `lat` | float | -90 to 90 | Center latitude |
| `lon` | float | -180 to 180 | Center longitude |
| `radius` | int | 1000-100000 | Search radius in meters (default 50km) |

---

## WiFi Client

### GET /api/wifi/status

Get WiFi client (station) status.

**Response:**
```json
{
  "enabled": true,
  "connected": true,
  "ssid": "HomeNetwork",
  "ip": "192.168.1.100",
  "rssi": -45,
  "savedSsid": "HomeNetwork"
}
```

### POST /api/wifi/scan

Start scanning for WiFi networks.

**Response:**
```json
{
  "networks": [
    {
      "ssid": "HomeNetwork",
      "rssi": -45,
      "secure": true,
      "channel": 6
    }
  ]
}
```

### POST /api/wifi/connect

Connect to a WiFi network.

**Request (form data):**
```
ssid=HomeNetwork&password=mypassword123
```

**Validation:**
- Password must be ≥8 characters

### POST /api/wifi/disconnect

Disconnect from current WiFi network.

### POST /api/wifi/forget

Forget saved WiFi credentials.

---

## Debug

### GET /api/debug/metrics

Get performance metrics.

**Response:**
```json
{
  "freeHeap": 180000,
  "minFreeHeap": 150000,
  "uptime": 3600,
  "blePackets": 15000,
  "gpsUpdates": 3600,
  "wifiClients": 1,
  "taskHighWater": {...}
}
```

### GET /api/debug/events

Get recent debug events.

**Response:**
```json
{
  "events": [
    {
      "time": 3600,
      "type": "BLE",
      "message": "V1 connected"
    }
  ]
}
```

### POST /api/debug/events/clear

Clear event log.

### POST /api/debug/enable

Enable/disable debug logging.

**Request (form data):**
```
logAlerts=true&logBle=true&logGps=false
```

### GET /api/debug/logs

Get log file metadata.

### GET /api/debug/logs/download

Download log file.

### GET /api/debug/logs/tail

Get last N lines of log file.

**Query Parameters:**
- `lines` (optional): Number of lines (default 100)

### POST /api/debug/logs/clear

Clear log file.

---

## Audio

### GET /api/audio/profiles

Get audio alert profiles.

### POST /api/audio/profile

Set audio alert profile.

**Request (form data):** `profile=beeps` or `profile=voice`

### POST /api/audio/test

Play a test sound.

**Request (form data):** `sound=alert`

---

## Error Responses

All endpoints return appropriate HTTP status codes:

| Code | Description |
|------|-------------|
| 200 | Success |
| 400 | Bad request (invalid parameters) |
| 404 | Not found |
| 500 | Internal server error |
| 503 | Service unavailable (V1 not connected) |

Error response body:
```json
{
  "error": "Description of the error"
}
```

---

## Error Codes Reference

The firmware uses structured error codes defined in `src/error_codes.h` for consistent error reporting.

### Error Categories

| Range | Category | Description |
|-------|----------|-------------|
| 0 | Success | No error |
| 100-199 | BLE | Bluetooth errors |
| 200-299 | GPS | GPS module errors |
| 300-399 | Storage | Filesystem errors |
| 400-499 | WiFi | Network errors |
| 500-599 | V1 | V1 protocol errors |
| 900-999 | System | General system errors |

### BLE Errors (100-199)

| Code | Name | Description |
|------|------|-------------|
| 100 | `ERR_BLE_NOT_INITIALIZED` | BLE stack not initialized |
| 101 | `ERR_BLE_SCAN_FAILED` | BLE scan failed to start |
| 102 | `ERR_BLE_CONNECT_FAILED` | Connection attempt failed |
| 103 | `ERR_BLE_CONNECT_TIMEOUT` | Connection timed out |
| 104 | `ERR_BLE_DISCONNECT` | Unexpected disconnection |
| 105 | `ERR_BLE_SERVICE_NOT_FOUND` | Required BLE service missing |
| 106 | `ERR_BLE_CHAR_NOT_FOUND` | Required characteristic missing |
| 107 | `ERR_BLE_WRITE_FAILED` | Write operation failed |
| 108 | `ERR_BLE_NOTIFY_FAILED` | Notification setup failed |
| 109 | `ERR_BLE_MTU_NEGOTIATION_FAILED` | MTU negotiation failed |
| 110 | `ERR_BLE_ALREADY_CONNECTED` | Already connected |
| 111 | `ERR_BLE_DEVICE_NOT_FOUND` | Device not found in scan |
| 112 | `ERR_BLE_PAIRING_FAILED` | Pairing/bonding failed |

### GPS Errors (200-299)

| Code | Name | Description |
|------|------|-------------|
| 200 | `ERR_GPS_NOT_INITIALIZED` | GPS handler not initialized |
| 201 | `ERR_GPS_NO_FIX` | No GPS fix available |
| 202 | `ERR_GPS_INVALID_DATA` | Invalid NMEA data |
| 203 | `ERR_GPS_TIMEOUT` | GPS communication timeout |
| 204 | `ERR_GPS_COMM_ERROR` | Serial communication error |
| 205 | `ERR_GPS_CONFIG_FAILED` | Module configuration failed |
| 206 | `ERR_GPS_MODULE_NOT_DETECTED` | No GPS module detected |
| 207 | `ERR_GPS_BAUD_MISMATCH` | Baud rate mismatch |

### Storage Errors (300-399)

| Code | Name | Description |
|------|------|-------------|
| 300 | `ERR_STORAGE_NOT_INITIALIZED` | Storage not mounted |
| 301 | `ERR_STORAGE_MOUNT_FAILED` | LittleFS mount failed |
| 302 | `ERR_STORAGE_FULL` | Storage full |
| 303 | `ERR_STORAGE_READ_FAILED` | File read failed |
| 304 | `ERR_STORAGE_WRITE_FAILED` | File write failed |
| 305 | `ERR_STORAGE_FILE_NOT_FOUND` | File not found |
| 306 | `ERR_STORAGE_CORRUPT_DATA` | Data corruption detected |
| 307 | `ERR_STORAGE_PERMISSION_DENIED` | Permission denied |
| 308 | `ERR_STORAGE_SD_NOT_INSERTED` | SD card not inserted |
| 309 | `ERR_STORAGE_FORMAT_FAILED` | Format operation failed |
| 310 | `ERR_STORAGE_JSON_PARSE_FAILED` | JSON parsing error |
| 311 | `ERR_STORAGE_JSON_TOO_LARGE` | JSON exceeds size limit |

### WiFi Errors (400-499)

| Code | Name | Description |
|------|------|-------------|
| 400 | `ERR_WIFI_NOT_INITIALIZED` | WiFi not initialized |
| 401 | `ERR_WIFI_AP_START_FAILED` | AP mode failed to start |
| 402 | `ERR_WIFI_STA_CONNECT_FAILED` | Station connection failed |
| 403 | `ERR_WIFI_STA_TIMEOUT` | Connection timeout |
| 404 | `ERR_WIFI_STA_WRONG_PASSWORD` | Incorrect password |
| 405 | `ERR_WIFI_STA_NO_SSID` | SSID not found |
| 406 | `ERR_WIFI_SERVER_START_FAILED` | Web server failed |
| 407 | `ERR_WIFI_DNS_START_FAILED` | DNS server failed |
| 408 | `ERR_WIFI_MDNS_START_FAILED` | mDNS failed |
| 409 | `ERR_WIFI_OTA_FAILED` | OTA update failed |
| 410 | `ERR_WIFI_HTTP_REQUEST_FAILED` | HTTP request failed |
| 411 | `ERR_WIFI_HTTP_TIMEOUT` | HTTP timeout |
| 412 | `ERR_WIFI_INVALID_URL` | Invalid URL format |

### V1 Protocol Errors (500-599)

| Code | Name | Description |
|------|------|-------------|
| 500 | `ERR_V1_INVALID_PACKET` | Malformed V1 packet |
| 501 | `ERR_V1_CHECKSUM_MISMATCH` | Packet checksum failed |
| 502 | `ERR_V1_UNKNOWN_PACKET_TYPE` | Unknown packet type |
| 503 | `ERR_V1_MALFORMED_DATA` | Invalid packet data |
| 504 | `ERR_V1_QUEUE_FULL` | Packet queue overflow |
| 505 | `ERR_V1_NOT_CONNECTED` | V1 not connected |
| 506 | `ERR_V1_COMMAND_FAILED` | V1 command failed |
| 507 | `ERR_V1_VERSION_MISMATCH` | Protocol version mismatch |

### System Errors (900-999)

| Code | Name | Description |
|------|------|-------------|
| 900 | `ERR_SYSTEM_OUT_OF_MEMORY` | Heap exhausted |
| 901 | `ERR_SYSTEM_TASK_CREATE_FAILED` | FreeRTOS task creation failed |
| 902 | `ERR_SYSTEM_MUTEX_TIMEOUT` | Mutex acquisition timeout |
| 903 | `ERR_SYSTEM_INVALID_PARAMETER` | Invalid function parameter |
| 904 | `ERR_SYSTEM_NOT_IMPLEMENTED` | Feature not implemented |
| 905 | `ERR_SYSTEM_HARDWARE_FAULT` | Hardware fault detected |
| 906 | `ERR_SYSTEM_WATCHDOG_TIMEOUT` | Watchdog triggered |
| 907 | `ERR_SYSTEM_LOW_BATTERY` | Battery critically low |

### Helper Macros

```c
// Get error category
ERR_CATEGORY(err)       // Returns 100, 200, etc.

// Check error type
IS_BLE_ERROR(err)       // true if 100-199
IS_GPS_ERROR(err)       // true if 200-299
IS_STORAGE_ERROR(err)   // true if 300-399
IS_WIFI_ERROR(err)      // true if 400-499
IS_V1_ERROR(err)        // true if 500-599
IS_SYSTEM_ERROR(err)    // true if 900-999

// Get human-readable string
errToString(err)        // Returns "BLE connection failed" etc.
```

### Usage Example

```cpp
#include "error_codes.h"

int result = connectToV1();
if (result != ERR_NONE) {
    Serial.printf("Error %d: %s\n", result, errToString(result));
    if (IS_BLE_ERROR(result)) {
        // BLE-specific recovery
    }
}
```

---

## Security Notes

1. **Change Default Password**: The default AP password is `setupv1g2`. Change it immediately in Settings.

2. **Network Isolation**: The device creates an isolated WiFi AP. Only connect trusted devices.

3. **No HTTPS**: All communication is unencrypted HTTP. Do not use over untrusted networks.

4. **WiFi Client Mode**: When connected to external WiFi, the device may be accessible to other devices on that network.

---

## Rate Limits

- No explicit rate limiting implemented
- BLE operations are serialized (one at a time)
- GPS updates: 1Hz maximum
- OBD polling: 2Hz maximum

---

## Firmware Version

Check firmware version via:
- `GET /_app/version.json`
- `GET /api/status` includes version in response

---

*Last updated: 2024*
