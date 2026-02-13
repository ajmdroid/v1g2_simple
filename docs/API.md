# V1-Simple API Reference

Complete API documentation for the V1-Simple web interface and REST endpoints.

**Base URL**: `http://192.168.35.5` (default AP mode)  
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

Get device status including V1 connection, WiFi, GPS, and alerts.

**Response:**
```json
{
  "wifi": {
    "setup_mode": true,
    "ap_active": true,
    "sta_connected": true,
    "sta_ip": "192.168.1.100",
    "ap_ip": "192.168.35.5",
    "ssid": "HomeNetwork",
    "rssi": -45,
    "sta_enabled": true,
    "sta_ssid": "HomeNetwork"
  },
  "device": {
    "uptime": 3600,
    "heap_free": 180000,
    "hostname": "v1g2",
    "firmware_version": "4.0.0-dev"
  },
  "battery": {
    "voltage_mv": 4150,
    "percentage": 85,
    "on_battery": false,
    "has_battery": true
  },
  "v1_connected": true,
  "alert": {
    "active": true,
    "band": "Ka",
    "frequency": 34700,
    "strength": 5
  }
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
  "gpsEnabled": false,
  "obdEnabled": false,
  "lockoutEnabled": true,
  "lockoutKaProtection": true,
  "lockoutDirectionalUnlearn": true,
  "lockoutFreqToleranceMHz": 8,
  "lockoutLearnCount": 3,
  "lockoutUnlearnCount": 5,
  "lockoutManualDeleteCount": 25,
  "lockoutLearnIntervalHours": 4,
  "lockoutUnlearnIntervalHours": 4,
  "lockoutMaxSignalStrength": 0,
  "lockoutMaxDistanceM": 600,
  "cameraAlertsEnabled": true,
  "cameraAudioEnabled": true,
  "cameraAlertDistanceM": 500,
  "enableWifiAtBoot": false
}
```

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| `ap_ssid` | string | 1-32 chars | WiFi AP SSID |
| `ap_password` | string | 8-63 chars | WiFi AP password (masked in response) |
| `isDefaultPassword` | boolean | - | `true` if using factory default password |
| `proxy_ble` | boolean | - | Enable BLE proxy mode |
| `proxy_name` | string | 0-32 chars | Custom device name for proxy |
| `displayStyle` | int | 0-3 | Display theme (0=Classic, 1=Modern, 2=Hemi, 3=Serpentine) |
| `autoPowerOffMinutes` | int | 0-60 | Auto power off after V1 disconnect (0=disabled) |
| `apTimeoutMinutes` | int | 0,5-60 | AP auto-off after inactivity (0=always on) |
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

**Request (query param):** `type=0|1|2|3`

| type | Meaning | Voice/Display | Default |
| --- | --- | --- | --- |
| 0 | Red Light | REDLIGHT | ✓ (default) |
| 1 | Speed | SPEED |  |
| 2 | ALPR | ALPR |  |
| 3 | Red Light + Speed | RLS |  |

If omitted, `type=0` (Red Light) is used. The device cycles 1→2→3 simulated cameras over 9s and plays the corresponding voice prompt.

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

Forget saved WiFi credentials and disable WiFi client mode.

### POST /api/wifi/enable

Enable or disable WiFi client mode.

**Request (JSON):**
```json
{
  "enabled": true
}
```

**Response:**
```json
{
  "success": true,
  "message": "WiFi client enabled"
}
```

**Notes:**
- When enabling: If saved credentials exist, automatically attempts to connect
- When disabling: Disconnects from network and switches to AP-only mode

---

## Debug

### GET /api/debug/metrics

Get runtime performance counters and subsystem health snapshots.

**Response (example):**
```json
{
  "rxPackets": 15000,
  "parseSuccesses": 14999,
  "parseFailures": 1,
  "queueDrops": 0,
  "cmdBleBusy": 0,
  "powerAutoPowerArmed": 1,
  "powerAutoPowerTimerStart": 1,
  "powerAutoPowerTimerCancel": 1,
  "powerAutoPowerTimerExpire": 0,
  "powerCriticalWarn": 0,
  "powerCriticalShutdown": 0,
  "loopMaxUs": 8200,
  "heapFree": 180000,
  "heapMinFree": 150000,
  "heapDma": 92000,
  "heapDmaMin": 86000,
  "obd": {
    "state": 5,
    "connected": true,
    "scanActive": false,
    "hasValidData": true,
    "sampleAgeMs": 220,
    "speedMphX10": 653,
    "connFailures": 0,
    "pollFailStreak": 0,
    "notifyDrops": 0
  },
  "proxy": {
    "sendCount": 3500,
    "dropCount": 0,
    "errorCount": 0,
    "queueHighWater": 4,
    "connected": true
  },
  "eventBus": {
    "publishCount": 9800,
    "dropCount": 0,
    "size": 0
  }
}
```

**Power counters:**

| Field | Increments when |
|------|------------------|
| `powerAutoPowerArmed` | Auto power-off is armed after first valid V1 data |
| `powerAutoPowerTimerStart` | Auto power-off timer starts after V1 disconnect |
| `powerAutoPowerTimerCancel` | Auto power-off timer is canceled by reconnect |
| `powerAutoPowerTimerExpire` | Auto power-off timer reaches timeout |
| `powerCriticalWarn` | Critical battery warning is issued |
| `powerCriticalShutdown` | Critical battery shutdown path is triggered |

**OBD snapshot (`obd`) fields:**

| Field | Type | Notes |
|------|------|-------|
| `state` | int | OBD state enum value (see map below) |
| `connected` | bool | `true` when OBD state is READY/POLLING |
| `scanActive` | bool | `true` while manual OBD scan is active |
| `hasValidData` | bool | `true` when OBD data is fresh/valid |
| `sampleAgeMs` | int or `null` | Age of last OBD sample in ms, null if unavailable |
| `speedMphX10` | int or `null` | Vehicle speed in mph * 10, null if unavailable |
| `connFailures` | int | Current connect/init failure count |
| `pollFailStreak` | int | Current consecutive poll failure streak |
| `notifyDrops` | int | Stream buffer notification drops |

**OBD state map (`obd.state`):**

| Value | State |
|------|-------|
| `0` | `IDLE` |
| `1` | `SCANNING` |
| `2` | `CONNECTING` |
| `3` | `INITIALIZING` |
| `4` | `READY` |
| `5` | `POLLING` |
| `6` | `DISCONNECTED` |
| `7` | `FAILED` |

**Notes:**
- Counters are boot-session counters (monotonic until reboot/reset).
- This endpoint is intended to match SD perf CSV counters for runtime/CSV correlation checks.

### POST /api/debug/enable

Enable/disable runtime perf debug reporting.

**Request (form data):**
```
enable=true
```

### GET /api/debug/perf-files

List SD-backed perf CSV files under `/perf`.

**Response (example):**
```json
{
  "success": true,
  "storageReady": true,
  "onSdCard": true,
  "path": "/perf",
  "count": 2,
  "files": [
    {
      "name": "perf_boot_9.csv",
      "sizeBytes": 14822,
      "bootId": 9,
      "active": true
    },
    {
      "name": "perf_boot_8.csv",
      "sizeBytes": 9621,
      "bootId": 8,
      "active": false
    }
  ]
}
```

### GET /api/debug/perf-files/download

Download one perf CSV file.

**Query Parameters:**
- `name` (required): file name returned by `/api/debug/perf-files`

### POST /api/debug/perf-files/delete

Delete one perf CSV file.

**Request (form data):** `name=perf_boot_8.csv`

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
