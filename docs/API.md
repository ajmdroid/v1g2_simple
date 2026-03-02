# V1-Simple API Reference

Complete API documentation for the V1-Simple web interface and REST endpoints.

**Base URL**: `http://192.168.35.5` (default AP mode)  
**Content-Type**: `application/x-www-form-urlencoded` (POST) or `application/json`

---

## Table of Contents

- [Status & Info](#status--info)
- [System Utilities](#system-utilities)
- [Settings](#settings)
- [V1 Profiles](#v1-profiles)
- [Auto-Push](#auto-push)
- [Display Colors](#display-colors)
- [Lockouts](#lockouts)
- [GPS](#gps)
- [WiFi Client](#wifi-client)
- [Debug](#debug)

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

## System Utilities

### POST /api/time/set

Set trusted device time from client/AP context.

**Request (JSON preferred):**
```json
{
  "unixMs": 1739650000000,
  "tzOffsetMin": -300,
  "source": "client"
}
```

**Compatibility keys accepted:** `epochMs`, `clientEpochMs`, `tzOffsetMinutes`.

### POST /api/profile/push

Queue profile push to connected V1 using active slot/profile state.

**Response (example):**
```json
{
  "ok": true,
  "message": "Profile push queued - check display for progress"
}
```

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
  "gpsEnabled": false,
  "gpsLockoutMode": 3,
  "gpsLockoutModeName": "enforce",
  "gpsLockoutCoreGuardEnabled": true,
  "gpsLockoutMaxQueueDrops": 0,
  "gpsLockoutMaxPerfDrops": 0,
  "gpsLockoutMaxEventBusDrops": 0,
  "gpsLockoutKaLearningEnabled": false,
  "displayStyle": 0,
  "autoPowerOffMinutes": 0,
  "apTimeoutMinutes": 0,
  "enableWifiAtBoot": false,
  "enableSignalTraceLogging": true
}
```

| Field | Type | Range | Description |
|-------|------|-------|-------------|
| `ap_ssid` | string | 1-32 chars | WiFi AP SSID |
| `ap_password` | string | 8-63 chars | WiFi AP password (masked in response) |
| `isDefaultPassword` | boolean | - | `true` if using factory default password |
| `proxy_ble` | boolean | - | Enable BLE proxy mode |
| `proxy_name` | string | 0-32 chars | Custom device name for proxy |
| `gpsEnabled` | boolean | - | Enable GPS runtime |
| `gpsLockoutMode` | int | 0-3 | Lockout runtime mode (`off`,`shadow`,`advisory`,`enforce`) |
| `gpsLockoutCoreGuardEnabled` | boolean | - | Enable lockout core safety guard |
| `gpsLockoutMaxQueueDrops` | int | 0-65535 | Queue-drop threshold for core guard |
| `gpsLockoutMaxPerfDrops` | int | 0-65535 | Perf-drop threshold for core guard |
| `gpsLockoutMaxEventBusDrops` | int | 0-65535 | Event-bus-drop threshold for core guard |
| `gpsLockoutKaLearningEnabled` | boolean | - | Allow Ka learning in lockout learner |
| `gpsLockoutPreQuiet` | boolean | - | Pre-drop to muted volume in lockout zones |
| `displayStyle` | int | 0 or 3 | Display theme (0=Classic, 3=Serpentine). Values 1/2 are normalized to 0 at runtime. |
| `autoPowerOffMinutes` | int | 0-60 | Auto power off after V1 disconnect (0=disabled) |
| `apTimeoutMinutes` | int | 0,5-60 | AP auto-off after inactivity (0=always on) |
| `enableWifiAtBoot` | boolean | - | Boot with AP enabled instead of BOOT long-press |
| `enableSignalTraceLogging` | boolean | - | Log all priority bands to lockout CSV for diagnostics (best-effort) |

### POST /api/settings

Update device settings. Send only fields you want to change.

**Request (form data):**
```
ap_ssid=MyV1&ap_password=newpassword123&gpsEnabled=true&gpsLockoutMode=3
```

**Response:** `Settings saved` (text/plain)

### GET /api/settings/backup

Download all settings as JSON file.

**Response:** JSON file download with Content-Disposition header

### POST /api/settings/restore

Restore settings from backup JSON file.

**Request:** JSON body with settings object

**Response:** `Settings restored successfully` (text/plain)

### POST /api/settings/backup-now

Trigger an immediate settings backup to SD/LittleFS.

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

## V1 Devices

### GET /api/v1/devices

Get list of remembered V1 devices.

### POST /api/v1/devices/name

Set a friendly name for a remembered V1 device.

**Request (JSON or form):** `address=<BLE address>&name=<friendly name>`

### POST /api/v1/devices/profile

Assign a default profile to a remembered V1 device.

**Request (JSON or form):** `address=<BLE address>&profile=<profile name>`

### POST /api/v1/devices/delete

Delete a remembered V1 device.

**Request (JSON or form):** `address=<BLE address>`

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

### GET /api/lockouts/summary

Get read-only lockout candidate telemetry summary from the in-memory ring buffer.

**Response fields:**
- `published`: Total observations published since boot
- `drops`: Number of oldest entries overwritten when buffer was full
- `size`: Current entries in buffer
- `capacity`: Fixed ring capacity
- `latest`: Most recent observation (or `null`)
- `sd`: SD persistence counters and path (`enabled`, `path`, `enqueued`, `queueDrops`, `deduped`, `written`, `writeFail`, `rotations`)

### GET /api/lockouts/events

Get recent lockout candidate observations (newest first).

**Query Parameters:**
- `limit` (optional): Number of entries (1-128, default 32)

**Notes:**
- Candidate events are persisted to SD (when available) at `/lockout/lockout_candidates_boot_<bootId>.csv`.
- `enableSignalTraceLogging` (default `true`) also captures unsupported lockout bands for diagnostics.
- SD writes apply a dedupe gate (~15s minimum repeat per same signal bucket) to reduce long-run growth.

### GET /api/lockouts/zones

Get active zones + pending learner candidates.

**Query Parameters:**
- `activeLimit` (optional): `1..200` (default `64`)
- `pendingLimit` (optional): `1..64` (default `64`)

### POST /api/lockouts/zones/delete

Delete a learned zone by slot index.

**Request (JSON or form):**
```json
{
  "slot": 3
}
```

**Notes:**
- Only learned zones are deletable.

### POST /api/lockouts/zones/create

Create a new lockout zone manually.

**Request (JSON body):** Zone definition with lat, lon, radius, band fields.

### POST /api/lockouts/zones/update

Update an existing lockout zone by slot index.

**Request (JSON body):** Slot index and updated zone fields.

### GET /api/lockouts/zones/export

Export all lockout zones as a JSON download.

### POST /api/lockouts/zones/import

Import lockout zones from JSON.

**Request (JSON body):** Array of zone definitions.

---

## GPS

### GET /api/gps/status

Get GPS module status and current position.

**Response:**
```json
{
  "enabled": true,
  "runtimeEnabled": true,
  "mode": "runtime",
  "sampleValid": true,
  "hasFix": true,
  "satellites": 8,
  "hdop": 1.4,
  "locationValid": true,
  "latitude": 10.1234,
  "longitude": -20.5432,
  "courseValid": true,
  "courseDeg": 181.4,
  "speedMph": 42.7,
  "lockout": {
    "mode": "enforce",
    "coreGuardEnabled": true,
    "coreGuardTripped": false
  }
}
```

### GET /api/gps/observations

Get recent GPS observation ring samples.

**Query Parameters:**
- `limit` (optional): `1..32` (default `16`)

### POST /api/gps/config

Update GPS/lockout runtime config and optional scaffold samples.

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

**Notes:**
- Counters are boot-session counters (monotonic until reboot/reset).
- This endpoint is intended to match SD perf CSV counters for runtime/CSV correlation checks.

### POST /api/debug/enable

Enable/disable runtime perf debug reporting.

**Request (form data):**
```
enable=true
```

### GET /api/debug/panic

Get last-reset crash snapshot and optional `/panic.txt` content.

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

### POST /api/debug/metrics/reset

Reset all runtime performance counters to zero.

### GET /api/debug/v1-scenario/list

List available V1 test scenarios (pre-recorded BLE packet sequences).

### GET /api/debug/v1-scenario/status

Get status of currently running scenario (active, name, progress).

### POST /api/debug/v1-scenario/load

Load a V1 test scenario by name.

**Request (form data):** `name=<scenario_name>`

### POST /api/debug/v1-scenario/start

Start playback of the loaded V1 test scenario.

### POST /api/debug/v1-scenario/stop

Stop the currently running V1 test scenario.

---

## Legacy Shorthand Routes

These non-prefixed routes predate the `/api/` convention. They remain for backward
compatibility with older bookmarks and scripts.

### POST /darkmode

Toggle V1 display dark mode (sends display-off/on command to V1).

**Request (form data):** `state=1` (dark on) or `state=0` (dark off)

**Response:**
```json
{
  "success": true,
  "darkMode": true
}
```

### POST /mute

Toggle V1 mute state (sends mute-on/off command to V1).

**Request (form data):** `state=1` (muted) or `state=0` (unmuted)

**Response:**
```json
{
  "success": true,
  "muted": true
}
```

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

---

## Firmware Version

Check firmware version via:
- `GET /_app/version.json`
- `GET /api/status` includes version in response

---

*Last updated: February 2026*
