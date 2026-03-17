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
- [Audio Settings](#audio-settings)
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
    "sta_ssid": "HomeNetwork",
    "ap_last_transition_reason_code": 4,
    "ap_last_transition_reason": "low_dma",
    "low_dma_cooldown_ms": 12000,
    "auto_start": {
      "gate": "waiting_dma",
      "gateCode": 6,
      "enableWifi": true,
      "enableWifiAtBoot": true,
      "bleConnected": true,
      "v1ConnectedAtMs": 1739650000000,
      "msSinceV1Connect": 2500,
      "settleMs": 3000,
      "bootTimeoutMs": 30000,
      "canStartDma": false,
      "wifiAutoStartDone": false,
      "bleSettled": false,
      "bootTimeoutReached": false,
      "shouldAutoStart": false,
      "startTriggered": false,
      "startSucceeded": false
    }
  },
  "lockout": {
    "mode": "enforce",
    "modeRaw": 3,
    "coreGuardEnabled": true,
    "coreGuardTripped": false,
    "coreGuardReason": "none",
    "maxQueueDrops": 10,
    "maxPerfDrops": 10,
    "maxEventBusDrops": 10,
    "queueDrops": 0,
    "perfDrops": 0,
    "eventBusDrops": 0,
    "enforceRequested": true,
    "enforceAllowed": true
  },
  "device": {
    "uptime": 3600,
    "heap_free": 180000,
    "hostname": "v1g2",
    "firmware_version": "4.0.0-dev"
  },
  "time": {
    "valid": true,
    "source": 2,
    "confidence": 3,
    "tzOffsetMin": -300,
    "tzOffsetMinutes": -300,
    "epochMs": 1739650000000,
    "ageMs": 1234
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

`lockout` surfaces GPS lockout core-guard state so enforcement fail-open decisions are visible in the main status response. `wifi.ap_last_transition_reason*`, `wifi.low_dma_cooldown_ms`, and `wifi.auto_start` expose WiFi safety gating and deferred auto-start state without requiring the debug metrics surface.

### GET /ping

Health check endpoint.

**Response:** `OK` (text/plain)

---

## System Utilities

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

### GET /api/device/settings

Get device-owned settings for the Settings and Development pages.

**Response:**
```json
{
  "ap_ssid": "V1-Simple",
  "ap_password": "********",
  "isDefaultPassword": true,
  "proxy_ble": false,
  "proxy_name": "",
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
| `autoPowerOffMinutes` | int | 0-60 | Auto power off after V1 disconnect (0=disabled) |
| `apTimeoutMinutes` | int | 0,5-60 | AP auto-off after inactivity (0=always on) |
| `enableWifiAtBoot` | boolean | - | Boot with AP enabled instead of BOOT long-press |
| `enableSignalTraceLogging` | boolean | - | Log all active V1 alert bands to lockout CSV for diagnostics (best-effort) |

### POST /api/device/settings

Update device-owned settings. Send only fields you want to change.

**Request (form data):**
```
ap_ssid=MyV1&ap_password=newpassword123&proxy_ble=true&autoPowerOffMinutes=15
```

**Response:**
```json
{"success": true}
```

### GET /api/gps/config

Get persisted GPS/lockout configuration from the GPS-owned settings surface.

**Response:**
```json
{
  "success": true,
  "enabled": false,
  "gpsEnabled": false,
  "lockout": {
    "mode": "enforce",
    "modeRaw": 3,
    "coreGuardEnabled": true,
    "maxQueueDrops": 0,
    "maxPerfDrops": 0,
    "maxEventBusDrops": 0,
    "learnerPromotionHits": 3,
    "learnerRadiusE5": 45,
    "learnerFreqToleranceMHz": 8,
    "learnerLearnIntervalHours": 12,
    "learnerUnlearnIntervalHours": 0,
    "learnerUnlearnCount": 0,
    "manualDemotionMissCount": 12,
    "kaLearningEnabled": false,
    "kLearningEnabled": true,
    "xLearningEnabled": false,
    "preQuiet": false,
    "preQuietBufferE5": 0,
    "maxHdopX10": 20,
    "minLearnerSpeedMph": 2,
    "minSatellites": 4
  }
}
```

### GET /api/settings/backup

Download all settings as JSON file.

**Response:** JSON file download with Content-Disposition header

### POST /api/settings/restore

Restore settings from backup JSON file.

**Request:** JSON body with settings object

**Response:**
```json
{"success": true, "message": "Settings restored successfully"}
```

### POST /api/settings/backup-now

Trigger an immediate settings backup to SD.

**Response (200):**
```json
{"success": true, "message": "Backup written to SD"}
```

**Response (500):**
```json
{"success": false, "error": "Backup write failed"}
```

**Response (503):**
```json
{"success": false, "error": "SD card unavailable"}
```

---

## V1 Profiles

### GET /api/v1/profiles

List all saved V1 profiles.

**Response:**
```json
{
  "profiles": [
    {
      "name": "Highway",
      "description": "Default highway settings",
      "displayOn": true
    }
  ]
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

**Request (JSON body):**
```json
{"name": "CustomProfile"}
```

### POST /api/v1/pull

Pull current settings from connected V1 device.

**Response:**
```json
{
  "success": true,
  "message": "Request sent. Check current settings."
}
```

### POST /api/v1/push

Push a profile to the connected V1 device.

**Request (JSON body):**

Push by profile name:
```json
{"name": "Highway"}
```

Push raw bytes:
```json
{"bytes": [1, 2, 3, 4, 5, 6], "displayOn": true}
```

Push parsed settings:
```json
{"settings": { ... }, "displayOn": true}
```

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
  "enabled": true,
  "activeSlot": 0,
  "slots": [
    {
      "name": "Day",
      "profile": "Highway",
      "mode": 1,
      "color": 63488,
      "volume": 5,
      "muteVolume": 2,
      "darkMode": false,
      "muteToZero": false,
      "alertPersist": 0,
      "priorityArrowOnly": false
    }
  ]
}
```

### POST /api/autopush/slot

Configure an auto-push slot.

**Request (form data):**
```
slot=0&profile=Highway&mode=1
```

| Field | Required | Type | Description |
|-------|----------|------|-------------|
| `slot` | Yes | int (0-2) | Slot index |
| `profile` | Yes | string | Profile name |
| `mode` | Yes | int | Push mode |
| `name` | No | string | Slot display name |
| `color` | No | int | RGB565 color |
| `volume` | No | int | Alert volume |
| `muteVol` | No | int | Mute volume |
| `darkMode` | No | bool | Dark mode |
| `muteToZero` | No | bool | Mute to zero |
| `alertPersist` | No | int (0-5) | Alert persistence |
| `priorityArrowOnly` | No | bool | Priority arrow only |

### POST /api/autopush/activate

Manually activate a slot.

**Request (form data):** `slot=0&enable=true`

| Field | Required | Description |
|-------|----------|-------------|
| `slot` | Yes | Slot index (0-2) |
| `enable` | No | Enable auto-push (default `true`) |

### POST /api/autopush/push

Force push a specific slot's profile.

**Request (form data):** `slot=0`

| Field | Required | Description |
|-------|----------|-------------|
| `slot` | Yes | Slot index (0-2) |
| `profile` | No | Override profile name |
| `mode` | No | Override mode (only with `profile`) |

### GET /api/autopush/status

Get auto-push status and last push info.

---

## Audio Settings

### GET /api/audio/settings

Get current audio and voice-alert configuration.

**Response:** JSON with `voiceAlertMode`, `voiceDirectionEnabled`, `announceBogeyCount`,
`muteVoiceIfVolZero`, `voiceVolume`, secondary-alert toggles, and volume-fade settings.

### POST /api/audio/settings

Save audio and voice-alert configuration.

**Request (form data):** Audio-related fields only, including `voiceAlertMode`,
`voiceDirectionEnabled`, `announceBogeyCount`, `muteVoiceIfVolZero`, `voiceVolume`,
`announceSecondaryAlerts`, `secondaryLaser`, `secondaryKa`, `secondaryK`, `secondaryX`,
`alertVolumeFadeEnabled`, `alertVolumeFadeDelaySec`, and `alertVolumeFadeVolume`.

---

## Display Colors

### GET /api/display/settings

Get current display color configuration.

**Response:** JSON with RGB565 integer color values, display visibility toggles, `brightness`, and `displayStyle`.

Key color fields: `bogey`, `freq`, `arrowFront`, `arrowSide`, `arrowRear`, `bandL`, `bandKa`, `bandK`, `bandX`, `bandPhoto`, `wifiIcon`, `wifiConnected`, `bleConnected`, `bleDisconnected`, `bar1`..`bar6`, `muted`, `persisted`, `volumeMain`, `volumeMute`, `rssiV1`, `rssiProxy`, `lockout`, `gps`.

Also includes boolean display toggles plus `brightness` and `displayStyle`.

### POST /api/display/settings

Save display color configuration.

**Request (form data):** RGB565 integer color values, display visibility toggles, `brightness`, and `displayStyle`.

```
bogey=63488&freq=65535&arrowFront=2016&arrowSide=65504&arrowRear=63488
```

Accepts the same display-owned field names returned by GET.

### POST /api/display/settings/reset

Reset colors to default theme.

### POST /api/display/preview

Toggle a 5.5-second color preview demo on the physical display. No request body. If preview is running, it cancels; otherwise it starts.

**Response:**
```json
{"success": true, "active": true}
```

### POST /api/display/preview/clear

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
- `limit` (optional): Number of entries (1-96, default 24)

**Notes:**
- Candidate events are persisted to SD (when available) at `/lockout/lockout_candidates_boot_<bootId>.csv`.
- Supported lockout observations are captured from the full active V1 alert set, not only the current priority alert.
- `enableSignalTraceLogging` (default `true`) also captures unsupported lockout bands from that same alert set for diagnostics.
- SD writes apply a dedupe gate (~15s minimum repeat per same signal bucket) to reduce long-run growth.

### GET /api/lockouts/zones

Get active zones + pending learner candidates.

**Query Parameters:**
- `activeLimit` (optional): `1..96` (default `24`)
- `pendingLimit` (optional): `1..48` (default `24`)
- `activeOffset` (optional): Pagination offset for active zones (default `0`)
- `pendingOffset` (optional): Pagination offset for pending zones (default `0`)
- `details` (optional): `1` or `true` to include timing fields

### POST /api/lockouts/zones/delete

Delete a learned zone by slot index.

**Request (JSON or form):**
```json
{
  "slot": 3
}
```

**Notes:**
- Both manual and learned zones are deletable.

### POST /api/lockouts/zones/create

Create a new lockout zone manually.

**Request (JSON body):**
```json
{
  "latitude": 35.10000,
  "longitude": -80.80000,
  "radiusE5": 135,
  "bandMask": 4,
  "frequencyMHz": 24100,
  "frequencyToleranceMHz": 10,
  "confidence": 100,
  "directionMode": "all",
  "headingDeg": null,
  "headingToleranceDeg": 45
}
```

**Notes:**
- `latitude`, `longitude`, `bandMask`, and `frequencyMHz` are required.
- Successful creates are stored as active manual zones (`manual=true`, `learned=false`).
- The endpoint rejects duplicate zone signatures with `409` and includes the conflicting `slot`.

### POST /api/lockouts/zones/update

Update an existing lockout zone by slot index.

**Request (JSON body):** Slot index plus any updated zone fields.

**Notes:**
- `slot` is required.
- The resulting zone must still have a valid `frequencyMHz`.
- Existing `manual`/`learned` flags are preserved on update.
- Updates that would duplicate another active zone are rejected with `409` and include the conflicting `slot`.

### GET /api/lockouts/zones/export

Export all lockout zones as a JSON download.

### POST /api/lockouts/zones/import

Import lockout zones from JSON.

**Request (JSON body):** Array of zone definitions.

### POST /api/lockouts/pending/clear

Clear all pending learner candidates.

**Response:**
```json
{"success": true, "cleared": 3}
```

---

## GPS

### GET /api/gps/status

Get GPS module status and current position.

**Response:** Large JSON object with GPS fix, satellite, and lockout state.

Key fields include: `enabled`, `runtimeEnabled`, `sampleValid`, `hasFix`, `stableHasFix`, `satellites`, `stableSatellites`, `hdop`, `locationValid`, `latitude`, `longitude`, `courseValid`, `courseDeg`, `speedMph`, `moduleDetected`, `parserActive`. Also includes nested `lockout`, `observations`, and `speedSource` objects with detailed counters.

### GET /api/gps/observations

Get recent GPS observation ring samples.

**Query Parameters:**
- `limit` (optional): `1..32` (default `16`)

### POST /api/gps/config

Update GPS/lockout runtime config and optional scaffold samples.

---

## OBD

### GET /api/obd/config

Get persisted OBD configuration.

**Response**

```json
{
  "enabled": true,
  "minRssi": -80
}
```

### GET /api/obd/status

Get OBD-II adapter connection status and speed data.

**Response**

```json
{
  "enabled": true,
  "connected": true,
  "speedValid": true,
  "speedMph": 37.3,
  "speedAgeMs": 412,
  "rssi": -52,
  "scanInProgress": false,
  "savedAddressValid": true,
  "connectAttempts": 1,
  "pollCount": 842,
  "pollErrors": 0,
  "consecutiveErrors": 0,
  "state": 7
}
```

| Field | Type | Description |
|-------|------|-------------|
| `enabled` | boolean | Whether OBD is enabled in settings |
| `connected` | boolean | BLE connection to adapter is active |
| `speedValid` | boolean | Current speed reading is fresh and valid |
| `speedMph` | float | Vehicle speed in mph (0 when invalid) |
| `speedAgeMs` | integer | Age of last speed sample in milliseconds |
| `rssi` | integer | BLE signal strength in dBm |
| `scanInProgress` | boolean | BLE scan is currently running |
| `savedAddressValid` | boolean | A paired adapter address is saved |
| `state` | integer | State machine state (0=Idle, 1=WaitBoot, 2=Scanning, 4=Connecting, 5=Discovering, 6=ATInit, 7=Polling, 8=ErrorBackoff, 9=Disconnected) |

### POST /api/obd/scan

Request a BLE scan for OBDLink adapters. The runtime reports whether the request was accepted immediately; the scan itself is asynchronous and auto-connects to the first matching device above the RSSI threshold once the runtime enters scanning.

**Response**

```json
{ "ok": true, "requested": true, "scanInProgress": false, "message": "OBD scan requested" }
```

### POST /api/obd/forget

Clear the saved OBD adapter address and disconnect. The adapter will need to be re-scanned to reconnect.

**Response**

```json
{ "ok": true }
```

### POST /api/obd/config

Update OBD runtime configuration.

**Request (JSON or form):**
- `enabled`: `true|false`
- `minRssi`: integer RSSI threshold from `-90` to `-40`

**Behavior**

- Persists the enabled state and minimum RSSI threshold.
- Updates the live OBD runtime and OBD speed-source gating immediately.

---

## WiFi Client

### GET /api/wifi/status

Get WiFi client (station) status.

**Response:**
```json
{
  "enabled": true,
  "savedSSID": "HomeNetwork",
  "state": "connected",
  "connectedSSID": "HomeNetwork",
  "ip": "192.168.1.100",
  "rssi": -45,
  "scanRunning": false
}
```

**Notes:** `connectedSSID`, `ip`, and `rssi` are only present when connected.

### POST /api/wifi/scan

Start scanning for WiFi networks.

**Response:**
```json
{
  "scanning": false,
  "networks": [
    {
      "ssid": "HomeNetwork",
      "rssi": -45,
      "secure": true
    }
  ]
}
```

### POST /api/wifi/connect

Connect to a WiFi network.

**Request (JSON body):**
```json
{
  "ssid": "HomeNetwork",
  "password": "mypassword123"
}
```

**Validation:**
- SSID is required (non-empty)
- No minimum password length enforced

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

**Response:** JSON object with 80+ boot-session counters. Example shows a subset of key fields:
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
  "loggingActive": false,
  "activeFile": "",
  "fileOpsBlocked": false,
  "count": 2,
  "files": [
    {
      "name": "perf_boot_9.csv",
      "sizeBytes": 14822,
      "bootId": 9,
      "active": true,
      "downloadAllowed": true,
      "deleteAllowed": true
    },
    {
      "name": "perf_boot_8.csv",
      "sizeBytes": 9621,
      "bootId": 8,
      "active": false,
      "downloadAllowed": true,
      "deleteAllowed": true
    }
  ]
}
```

### GET /api/debug/perf-files/download

Download one perf CSV file.

**Query Parameters:**
- `name` (required): file name returned by `/api/debug/perf-files`

If download is blocked, the JSON error response includes `reasonCode`, `operation`, and `retryable`.

### POST /api/debug/perf-files/delete

Delete one perf CSV file.

**Request (form data):** `name=perf_boot_8.csv`

If delete is blocked, the JSON error response includes `reasonCode`, `operation`, and `retryable`.

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
