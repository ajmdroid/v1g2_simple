# Web UI Logging & Diagnostics

## Changes Made

This document describes the minimal logging instrumentation added to diagnose Web UI loading issues.

### Files Modified
- `src/wifi_manager.cpp`

### Change Summary

#### 1. Added `dumpLittleFSRoot()` helper function (lines ~73-98)
- **Purpose**: Dump LittleFS root directory to serial log when entering Setup Mode
- **Behavior**:
  - Calls `LittleFS.begin(true)` to mount filesystem (format on fail)
  - Prints `[SetupMode] Dumping LittleFS root...`
  - Lists all files in `/` with names and sizes
  - Prints `[SetupMode] (empty)` if no files found
  - Prints error message if mount/open fails
- **When it runs**: Called immediately after LittleFS is mounted in `setupWebServer()`

#### 2. Updated `serveLittleFSFileHelper()` (lines ~141-173)
- **Added logging**:
  - When a compressed `.gz` file is served: `[HTTP] 200 <path> -> <path>.gz (<size> bytes)`
  - When an uncompressed file is served: `[HTTP] 200 <path> (<size> bytes)`
  - When a file is not found: `[HTTP] MISS <path> (file not found)`
- **No behavior change**: Same file serving logic, just added logging

#### 3. Updated `setupWebServer()` (line ~254)
- **Added call to `dumpLittleFSRoot()`** right after LittleFS mount
- Ensures filesystem inventory appears in logs before first HTTP request

#### 4. Enhanced root route handler `/` (lines ~300-313)
- **Behavior**: 
  1. First tries to serve `/index.html` from LittleFS (Svelte build)
  2. If found, logs `[HTTP] 200 / -> /index.html` and returns
  3. If not found, falls back to inline failsafe dashboard
  4. Logs `[HTTP] / -> inline failsafe dashboard` in fallback case
- **Safe routing**: No change to existing behavior, just adds fallback logging

#### 5. Updated `handleNotFound()` (lines ~1530+)
- **Added logging**: `[HTTP] 404 <uri>` for all 404 responses
- Files are tried via `serveLittleFSFileHelper()` which logs MISSes
- Final 404 response is logged before sending response

## Expected Serial Log Output

### Success Case (Files in LittleFS)

```
[SetupMode] Starting AP: V1-Simple (pass: setupv1g2)
[SetupMode] softAPConfig ok
[SetupMode] AP IP: 192.168.35.5
[SetupMode] LittleFS mounted
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html (4521 bytes)
[SetupMode]   _app/version.json (156 bytes)
[SetupMode]   _app/immutable/chunks/index-abc123.js (12345 bytes)
[SetupMode]   _app/immutable/chunks/layout-def456.js (8901 bytes)
[SetupMode] LittleFS mounted
[HTTP] 200 / -> /index.html
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] 200 /_app/immutable/chunks/index-abc123.js (12345 bytes)
```

### Failure Case (Empty LittleFS, using fallback)

```
[SetupMode] Starting AP: V1-Simple (pass: setupv1g2)
[SetupMode] softAPConfig ok
[SetupMode] AP IP: 192.168.35.5
[SetupMode] LittleFS mounted
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   (empty)
[SetupMode] LittleFS mounted
[HTTP] / -> inline failsafe dashboard
[HTTP] 200 / (650+ bytes inline HTML)
```

### Missing Assets Case (Partial files)

```
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html (4521 bytes)
[HTTP] 200 / -> /index.html
[HTTP] MISS /_app/version.json (file not found)
[HTTP] 404 /_app/version.json
[HTTP] MISS /_app/immutable/chunks/style-xyz789.css (file not found)
[HTTP] 404 /_app/immutable/chunks/style-xyz789.css
```

### Gzipped Files Case

```
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html.gz (2156 bytes)
[HTTP] 200 / -> /index.html.gz (2156 bytes)
[HTTP] 200 /_app/immutable/chunks/layout.js.gz (5000 bytes)
```

## Interpreting Logs

### 1. Check LittleFS mounting
Look for: `[SetupMode] LittleFS mounted` ✓  
Or: `[SetupMode] ERROR: LittleFS mount failed!` ✗

### 2. Check file presence
Look for file list after `[SetupMode] Files in LittleFS root:`
- **Empty list** → No files uploaded (check PlatformIO build process)
- **index.html missing** → Web UI was not deployed
- **_app/** files missing** → Svelte build artifacts missing

### 3. Check HTTP requests
Look for `[HTTP]` lines:
- `200` = File successfully served
- `MISS` = File not found in LittleFS
- `404` = No handler found, file missing

### 4. Diagnose slow loading
- **Many MISSes followed by 404s** → Assets missing from LittleFS
- **Initial 200s then later 404s** → HTML loaded but JS/CSS assets missing
- **Fallback dashboard displayed** → LittleFS empty but inline dashboard works

## Root Cause Decision Tree

```
Does setup mode start?
├─ NO → Check SerialLog output for errors
└─ YES
   ├─ LittleFS mounted?
   │  ├─ NO → Add `#include <LittleFS.h>` and `#include <FS.h>`
   │  └─ YES
   │     ├─ Files listed?
   │     │  ├─ NO (empty) → Upload files via PlatformIO LittleFS tool
   │     │  └─ YES
   │     │     ├─ index.html present?
   │     │     │  ├─ YES
   │     │     │  │  ├─ Loads? (200 status)
   │     │     │  │  │  ├─ YES → Check assets (CSS, JS, etc.)
   │     │     │  │  │  └─ NO → Check file permissions
   │     │     │  │  └─ NO
   │     │     │  └─ NO → Build and upload Svelte
   │     │     └─ Assets loading? (Check for MISS / 404)
   │     │        ├─ NO → Files missing from build
   │     │        └─ YES → Check browser console for JS errors
```

## No Behavior Changes

These changes are **diagnostic only**:
- ✓ Same file serving (with logging)
- ✓ Same failsafe dashboard (with logging on fallback)
- ✓ Same 404 handling (with logging)
- ✓ Same root route logic (with safe fallback)

All changes are **reversible**: Remove logging lines if needed, behavior unchanged.

## Testing the Diagnostics

1. **Build & flash**: `./build.sh --all` then upload to device
2. **Enter Setup Mode**: Hold BOOT + PWR ~2.5s on device
3. **Check serial log**: Monitor at 115200 or 921600 baud
4. **Look for `[SetupMode] Dumping LittleFS root...`** before any HTTP requests
5. **Connect to `V1-Simple` AP** and visit `http://192.168.35.5/`
6. **Check log for HTTP 200 or fallback dashboard message**
7. **If 404s appear**, note which files are missing

## Future: Adding More Diagnostics (Optional)

If needed later, you can add:
- Timing info: How long each file takes to serve
- Client info: User-Agent, Accept-Encoding headers
- Cache hits: How many 304 (Not Modified) responses
- Bandwidth: Total bytes served per request type
- Error codes: Distinguish between permission denied vs. not found

Keep it minimal for now—just files and HTTP status codes.
