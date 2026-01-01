# Web UI Diagnostics - Quick Reference

## What Was Added
Minimal logging to identify why Web UI fails to load or is slow.

## Where to Look in Serial Log

### 1. **Startup (enter Setup Mode)**
```
[SetupMode] Starting AP: V1-Simple (pass: setupv1g2)
[SetupMode] LittleFS mounted
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   <files listed here>
```

**What it tells you:**
- ✓ LittleFS mounted = Good
- ✗ ERROR: LittleFS mount failed = Filesystem issue
- ✓ Files listed = Assets are on device
- ✓ (empty) = No files uploaded (PlatformIO issue)

### 2. **Request Root `/`**
```
[HTTP] 200 / -> /index.html
```
OR
```
[HTTP] / -> inline failsafe dashboard
```

**Meaning:**
- `200 / -> /index.html` = Svelte build found, loading it
- `/ -> inline failsafe dashboard` = No index.html, using embedded fallback

### 3. **Asset Requests**
```
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] 200 /_app/immutable/chunks/index-abc123.js (12345 bytes)
```

**Meaning:**
- `200` = Asset loaded successfully
- `MISS` = Asset not in LittleFS (means it wasn't uploaded)
- `404` = No handler found for this path

### 4. **Missing Assets**
```
[HTTP] MISS /_app/immutable/chunks/style.css (file not found)
[HTTP] 404 /_app/immutable/chunks/style.css
```

**Meaning:** Some assets built but not uploaded to device

---

## Diagnosis Flowchart

### Does webpage load at all?
```
NO:
  └─ Check [SetupMode] Dumping LittleFS root...
     ├─ (empty) → Files not uploaded, check PlatformIO build
     └─ index.html present?
        ├─ No → upload missing
        └─ Yes → Check next step

YES:
  ├─ Full page + styling visible → All assets loaded
  └─ Page broken/unstyled → Some assets missing (check 404s below)
```

### Which assets are missing?
```
Search logs for: [HTTP] 404 or [HTTP] MISS

Examples:
  [HTTP] MISS /_app/immutable/chunks/Layout.js
    → This JS file is missing
  
  [HTTP] MISS /_app/immutable/chunks/page.svelte
    → This Svelte component is missing
  
  [HTTP] MISS /_app/immutable/chunks/style.css
    → CSS missing → page shows unstyled
```

### Why isn't web UI uploaded?

**Check these in order:**

1. **Did you build the Svelte UI?**
   ```bash
   cd interface/
   npm run build
   ```
   Should create `dist/` folder

2. **Did PlatformIO upload it?**
   ```bash
   pio run --target littlefs
   pio run --target uploadfs
   ```
   Should upload `data/` folder to device

3. **Is data/ folder present?**
   ```bash
   ls -la data/
   ```
   Should have: `data/index.html`, `data/_app/`, etc.

4. **Try manual rebuild:**
   ```bash
   cd /Users/ajmedford/v1g2_simple
   rm -rf data/
   cd interface && npm run build
   cp -r dist/* ../data/
   cd ..
   ./build.sh --all
   ```

---

## Common Log Patterns

### ✓ Everything Working
```
[SetupMode] LittleFS mounted
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html (4521 bytes)
[SetupMode]   _app/version.json (156 bytes)
[SetupMode]   _app/immutable/chunks/Layout.js (8901 bytes)
[HTTP] 200 / -> /index.html
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] 200 /_app/immutable/chunks/Layout.js (8901 bytes)
```

### ⚠️ Using Fallback (No LittleFS files)
```
[SetupMode] LittleFS mounted
[SetupMode] Dumping LittleFS root...
[SetupMode] Files in LittleFS root:
[SetupMode]   (empty)
[HTTP] / -> inline failsafe dashboard
[HTTP] 200 / (650+ bytes inline HTML)
```

### ✗ Partial Files (Some assets missing)
```
[SetupMode] Files in LittleFS root:
[SetupMode]   index.html (4521 bytes)
[SetupMode]   _app/version.json (156 bytes)
[HTTP] 200 / -> /index.html
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] MISS /_app/immutable/chunks/Layout.js (file not found)
[HTTP] 404 /_app/immutable/chunks/Layout.js
[HTTP] MISS /_app/immutable/chunks/page.svelte (file not found)
[HTTP] 404 /_app/immutable/chunks/page.svelte
```
(Page loads but styling/functionality broken)

### ✗ LittleFS Mount Failed
```
[SetupMode] ERROR: LittleFS mount failed!
[HTTP] / -> inline failsafe dashboard
```
(Falls back to embedded dashboard, but can't serve Svelte UI)

---

## Quick Checklist

When diagnosing Web UI issues:

- [ ] Monitor serial log at 115200 or 921600 baud
- [ ] Enter Setup Mode (BOOT+PWR ~2.5s)
- [ ] Look for `[SetupMode] Dumping LittleFS root...`
- [ ] Check if files are listed (not empty)
- [ ] Connect to `V1-Simple` AP, visit `http://192.168.35.5/`
- [ ] Watch serial for `[HTTP]` lines during page load
- [ ] Count 200s (found) vs 404s/MISSes (missing)
- [ ] If all 200s → files are correct
- [ ] If 404s appear → note which files missing
- [ ] If using fallback → LittleFS empty, rebuild/upload Svelte

---

## No Behavior Changed

These are **logging only**—the Web UI works exactly the same:
- ✓ All files still served correctly
- ✓ Gzip compression still works
- ✓ Fallback dashboard still works
- ✓ Same 404 handling
- ✓ Same cache headers

You can safely leave this in production or remove if unneeded.

---

## Example: Debugging Missing CSS

**Scenario:** Page loads but has no styling (broken layout)

**Logs show:**
```
[HTTP] 200 / -> /index.html
[HTTP] 200 /_app/version.json (156 bytes)
[HTTP] MISS /_app/immutable/chunks/style-abc123.css (file not found)
[HTTP] 404 /_app/immutable/chunks/style-abc123.css
```

**Root cause:** CSS file wasn't built or uploaded  
**Solution:** 
```bash
cd interface && npm run build    # Rebuild Svelte
cp -r dist/* ../data/            # Copy to PlatformIO data dir
cd .. && ./build.sh --all        # Rebuild firmware with new files
```

**Verify fix:** Log shows `[HTTP] 200 /_app/immutable/chunks/style-abc123.css`

---

## Questions?

Check `LOGGING_DIAGNOSTICS.md` for detailed explanation of each log line.
