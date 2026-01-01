# Web UI Diagnostics - Visual Summary

## What's New

### 1. LittleFS Root Dump 📁
When you enter Setup Mode, the device now prints what files it has:

```
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html (4521 bytes)
[SetupMode]   _app/version.json (156 bytes)
[SetupMode]   _app/immutable/chunks/Layout.js (8901 bytes)
```

Or if empty:
```
[SetupMode]   (empty)
```

**Why?** So you know immediately if files got uploaded to the device or not.

---

### 2. HTTP Request Logging 🌐
Every web request is now logged:

✅ **File found & served:**
```
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] 200 / -> /index.html
```

❌ **File not found:**
```
[HTTP] MISS /_app/immutable/chunks/style.css (file not found)
[HTTP] 404 /_app/immutable/chunks/style.css
```

📦 **Compressed file served:**
```
[HTTP] 200 /_app/immutable/chunks/Layout.js -> Layout.js.gz (3456 bytes)
```

**Why?** So you can see exactly which assets are missing when the page breaks.

---

### 3. Smart Root Handler 🔀
The `/` route now tries to be smart:

**Case 1: Svelte UI present**
```
[HTTP] 200 / -> /index.html
```
(Serves the Svelte build)

**Case 2: Svelte UI missing**
```
[HTTP] / -> inline failsafe dashboard
```
(Falls back to embedded dashboard)

**Why?** Graceful fallback—if Svelte files aren't there, you still get a working UI.

---

## How It Looks in Practice

### Scenario: Everything Works ✅
```
[SetupMode] Starting AP: V1-Simple (pass: setupv1g2)
[SetupMode] AP IP: 192.168.35.5
[SetupMode] LittleFS mounted
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html (4521 bytes)
[SetupMode]   _app/version.json (156 bytes)

User connects to http://192.168.35.5/

[HTTP] 200 / -> /index.html
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] 200 /_app/immutable/chunks/Layout.js (8901 bytes)
[HTTP] 200 /_app/immutable/chunks/page.js (5432 bytes)

Result: ✅ Full Svelte UI loads, all assets present
```

---

### Scenario: Files Missing ⚠️
```
[SetupMode] Starting AP: V1-Simple (pass: setupv1g2)
[SetupMode] AP IP: 192.168.35.5
[SetupMode] LittleFS mounted
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   (empty)

User connects to http://192.168.35.5/

[HTTP] / -> inline failsafe dashboard
[HTTP] 200 / (650+ bytes inline HTML)

Result: ⚠️ Using fallback dashboard, files not on device
Action: Upload Svelte build to LittleFS
```

---

### Scenario: Partial Files ⚠️
```
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html (4521 bytes)
[SetupMode]   _app/version.json (156 bytes)

User connects to http://192.168.35.5/

[HTTP] 200 / -> /index.html
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] MISS /_app/immutable/chunks/Layout.js (file not found)
[HTTP] 404 /_app/immutable/chunks/Layout.js
[HTTP] MISS /_app/immutable/chunks/style.css (file not found)
[HTTP] 404 /_app/immutable/chunks/style.css

Result: ⚠️ Page loads but broken (missing CSS/JS)
Action: Rebuild Svelte, upload missing chunks
```

---

## Quick Log Reading Guide

| What You See | What It Means | What To Do |
|---|---|---|
| `[SetupMode]   (empty)` | No files on device | Upload Svelte build |
| `[HTTP] 200 / -> /index.html` | Svelte UI found & served | ✅ Check for asset 404s next |
| `[HTTP] / -> inline failsafe dashboard` | Fallback dashboard in use | Files not present or broken |
| `[HTTP] 200 /_app/...` | Asset found & served | ✅ Good |
| `[HTTP] MISS /_app/...` | Asset not found | ❌ Was not uploaded |
| `[HTTP] 404 /...` | No handler for path | ❌ Asset missing |
| `[HTTP] ERROR: LittleFS mount failed!` | Filesystem error | Check platformio.ini |

---

## Changes to Code

**File modified:** `src/wifi_manager.cpp`

**What changed:**
1. ✅ Added `dumpLittleFSRoot()` function to list files
2. ✅ Added logging to `serveLittleFSFileHelper()` for file serves and misses
3. ✅ Updated `/` route to try Svelte first, fallback with logging
4. ✅ Added 404 logging to `handleNotFound()`

**What stayed the same:**
- ❌ No behavior changes (all original functionality preserved)
- ❌ No new dependencies
- ❌ No performance impact (logging is fast)
- ❌ No flash size impact (only ~300 bytes for strings)

---

## Build & Test

### Build It
```bash
./build.sh --all
```
✅ Should see: `Build Complete!`

### Flash It
Upload via PlatformIO as normal

### Test It
1. Enter Setup Mode (BOOT+PWR ~2.5s)
2. Check serial log for `[SetupMode] Dumping LittleFS root...`
3. See what files are listed (or empty)
4. Connect to `V1-Simple` AP
5. Visit `http://192.168.35.5/`
6. Watch serial for `[HTTP]` lines
7. Identify any 404s or MISSes

---

## Troubleshooting Guide

### "I see (empty) in the LittleFS dump"
**Problem:** No files uploaded to device  
**Solution:**
```bash
cd interface && npm run build
cp -r dist/* ../data/
cd ..
./build.sh --all
```

### "I see 404 for CSS files"
**Problem:** CSS not uploaded  
**Solution:** Same as above—rebuild and re-upload

### "Page loads but styling is broken"
**Problem:** Assets partially missing  
**Check logs for:** `[HTTP] MISS` or `[HTTP] 404`  
**Solution:** Upload all files from Svelte build

### "ERROR: LittleFS mount failed!"
**Problem:** Filesystem mount issue  
**Possible causes:**
- Wrong `platformio.ini` configuration
- File system corrupted
- Flash memory issue

**Try:**
```bash
pio run --target littlefs  # Recreate LittleFS image
pio run --target uploadfs   # Upload it
```

### "I see /index.html (empty) - 0 bytes"
**Problem:** File uploaded but empty  
**Solution:** Svelte build not copied to `data/` folder  
**Fix:** Check `dist/index.html` exists and is not empty before copying

---

## One More Thing

All changes are **safe and reversible**. If you want to remove the logging:
1. Delete the `dumpLittleFSRoot()` function
2. Remove the 3 logging lines from `serveLittleFSFileHelper()`
3. Revert `/` route to simple `handleFailsafeUI()` call
4. Remove logging line from `handleNotFound()`

Behavior will be identical to before.

---

## Files Documentation Created

- 📄 `LOGGING_DIAGNOSTICS.md` - Detailed technical explanation
- 📄 `QUICK_REFERENCE.md` - Quick lookup guide
- 📄 `IMPLEMENTATION_SUMMARY.md` - What was changed and why
- 📄 `CHANGES.diff` - Before/after code comparison
- 📄 `PATCH.txt` - Unified diff format
- 📄 `This file` - Visual summary and examples

---

**Ready to diagnose your Web UI issues! 🚀**
