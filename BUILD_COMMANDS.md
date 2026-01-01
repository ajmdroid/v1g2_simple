# Complete Build & Deploy Commands

## ⚡ Quick Start - Use the Build Script!

### One command to rule them all:
```bash
# Build everything and upload (recommended!)
./build.sh --all

# Just build (no upload)
./build.sh

# Clean build and upload everything
./build.sh --clean --all

# Build firmware only, skip web (faster)
./build.sh --skip-web --upload

# Show all options
./build.sh --help
```

### Build script options:
- `--clean` or `-c` - Clean build artifacts first
- `--upload` or `-u` - Upload firmware after build
- `--upload-fs` or `-f` - Upload filesystem (web interface)
- `--monitor` or `-m` - Open serial monitor after upload
- `--all` or `-a` - Do everything (upload fs + firmware + monitor)
- `--skip-web` or `-s` - Skip web interface build (faster firmware-only builds)

---

## 🌐 Full Web + Firmware Deployment (Manual Steps)

### Complete from-scratch deployment:
```bash
# 1. Install web dependencies (first time only)
cd interface && npm install && cd ..

# 2. Build Svelte web interface
cd interface && npm run build && cd ..

# 3. Deploy web files to data/ folder for LittleFS
cd interface && npm run deploy && cd ..

# 4. Clean firmware build
pio run -t clean

# 5. Build firmware
pio run

# 6. Upload filesystem (web interface)
pio run -t uploadfs

# 7. Upload firmware
pio run -t upload

# 8. Monitor serial output
pio device monitor
```

### One-liner for complete rebuild and deploy:
```bash
cd interface && npm run deploy && cd .. && pio run -t clean && pio run && pio run -t uploadfs && pio run -t upload && pio device monitor
```

### Quick web update (no firmware changes):
```bash
cd interface && npm run deploy && cd .. && pio run -t uploadfs
```

### Quick firmware update (no web changes):
```bash
pio run -t upload
```

---

## 🎨 Web Interface Development

```bash
# Install dependencies (first time or after package.json changes)
cd interface && npm install

# Development mode with hot reload
cd interface && npm run dev

# Build for production
cd interface && npm run build

# Build + compress + deploy to data/
cd interface && npm run deploy
```

**Web Build Output:**
- Built files go to `interface/build/`
- Compressed with gzip for ESP32 serving
- Deployed to `data/` for LittleFS upload

---

## 🔧 Firmware Build Commands

### Clean & Build
```bash
# Clean all build artifacts (removes .pio/build/)
pio run -t clean

# Full rebuild from scratch
pio run

# Clean + Build in one command
pio run -t clean && pio run
```

### Upload to Device
```bash
# Upload firmware to connected device
pio run -t upload

# Upload and monitor serial output
pio run -t upload && pio device monitor

# Monitor only (no upload)
pio device monitor
```

### Advanced Options
```bash
# Verbose build output (shows all compiler commands)
pio run -v

# Build specific environment (if multiple in platformio.ini)
pio run -e waveshare-349

# Upload to specific port
pio run -t upload --upload-port /dev/ttyUSB0
```

---

## 💾 Filesystem Operations

```bash
# Upload filesystem image (LittleFS) - includes web interface
pio run -t uploadfs

# Build filesystem image only (no upload)
pio run -t buildfs

# Upload both filesystem AND firmware
pio run -t uploadfs && pio run -t upload
```

**Important:** Always run `npm run deploy` before `uploadfs` to ensure latest web files are included!

---

## 📋 Common Workflows

### Initial Setup
```bash
cd interface && npm install && cd ..
```

### Daily Development - Web Only
```bash
cd interface && npm run dev
# Make changes, test at http://localhost:5173
# When ready to deploy:
npm run deploy && cd .. && pio run -t uploadfs
```

### Daily Development - Firmware Only
```bash
pio run -t upload && pio device monitor
```

### Daily Development - Both Web + Firmware
```bash
cd interface && npm run deploy && cd .. && pio run && pio run -t uploadfs && pio run -t upload && pio device monitor
```

### Full Clean Rebuild Everything
```bash
# Clean web build
cd interface && rm -rf build .svelte-kit && npm run build && npm run deploy && cd ..

# Clean firmware and data
rm -rf .pio/build

# Rebuild and upload everything
pio run && pio run -t uploadfs && pio run -t upload && pio device monitor
```

### Production Release Build
```bash
# Complete from-scratch production build
cd interface && npm ci && npm run deploy && cd .. && \
pio run -t clean && pio run && \
pio run -t uploadfs && pio run -t upload
```

---

## 🐛 Troubleshooting

### Web Interface Issues
```bash
# Clear all caches and rebuild
cd interface
rm -rf node_modules package-lock.json build .svelte-kit
npm install
npm run build
npm run deploy
cd ..
```

### Firmware Issues
```bash
# Update PlatformIO core
pio upgrade

# Update all libraries
pio pkg update

# List installed packages
pio pkg list

# Check for compilation errors only
pio check

# Full clean rebuild
pio run -t clean && pio run
```

### Filesystem Upload Issues
```bash
# Verify data folder has files
ls -la data/

# Re-deploy web interface
cd interface && npm run deploy && cd ..

# Try uploadfs again
pio run -t uploadfs
```

---

## 📊 Build Output Locations

- **Web Build:** `interface/build/` → copied to → `data/`
- **Firmware Binary:** `.pio/build/waveshare-349/firmware.bin`
- **Filesystem Image:** `.pio/build/waveshare-349/littlefs.bin`

---

## ⌨️ Monitor Shortcuts

- `Ctrl+C` - Exit monitor
- `Ctrl+T` then `Ctrl+H` - Help menu
- `Ctrl+T` then `Ctrl+R` - Reset ESP32

---

## 🚀 Quick Reference

| Task | Command |
|------|---------|
| **Build web only** | `cd interface && npm run build` |
| **Deploy web to data/** | `cd interface && npm run deploy` |
| **Build firmware only** | `pio run` |
| **Upload firmware** | `pio run -t upload` |
| **Upload filesystem** | `pio run -t uploadfs` |
| **Upload both** | `pio run -t uploadfs && pio run -t upload` |
| **Complete rebuild** | `cd interface && npm run deploy && cd .. && pio run -t clean && pio run && pio run -t uploadfs && pio run -t upload` |
| **Monitor serial** | `pio device monitor` |
