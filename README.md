# V1 Gen2 Simple Display

A touchscreen display for the Valentine One Gen2 radar detector.

**Hardware:** [Waveshare ESP32-S3-Touch-LCD-3.49](https://www.amazon.com/dp/B0FQM41PGX)

---

## Quick Install

> ⚠️ **Before updating firmware:** Download a backup of your settings from the Settings page (`/settings` → Download Backup). This preserves your colors, profiles, and configuration.

### Option 1: Web Installer (Easiest)

No tools needed — just a Chrome browser and USB cable:

👉 **[Install via Web](https://ajmdroid.github.io/v1g2_simple/)**

1. Put device in bootloader mode (hold POWER + GEAR while plugging in USB)
2. Click "Install V1-Simple" and select your device
3. Wait for install to complete, then press RESET

### Option 2: Build from Source

#### Prerequisites
- Visual Studio Code with [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- Node.js 18+ (for building web UI)
- USB-C data cable (not charge-only)

#### Build & Flash

```bash
git clone https://github.com/ajmdroid/v1g2_simple
cd v1g2_simple
./build.sh --all
```

The script auto-detects your OS. First build takes 2-5 minutes to download libraries.

**Windows users:** If `./build.sh` fails, use manual commands:
```bash
cd interface && npm install && npm run build && npm run deploy && cd ..
pio run -e waveshare-349-windows -t uploadfs
pio run -e waveshare-349-windows -t upload
```

**Mac/Linux manual commands:**
```bash
cd interface && npm install && npm run build && npm run deploy && cd ..
pio run -e waveshare-349 -t uploadfs
pio run -e waveshare-349 -t upload
```

---

## Button Controls

The **BOOT** button (top right, looking at the display) controls WiFi and settings:

| Action | Function |
|--------|----------|
| **Short press** | Enter settings mode (brightness + voice volume) |
| **Long press (~4s)** | Toggle WiFi AP on/off |

### Settings Mode
1. Short press BOOT → settings sliders appear
2. **Top slider (green):** Display brightness (left = dim, right = bright)
3. **Bottom slider (blue):** Voice alert volume (left = quiet, right = loud)
   - Releases the slider to hear a test voice ("Ka ahead")
4. Short press BOOT again to save and exit

### WiFi Access Point
WiFi is **off by default** to save power. To access web settings:

1. Long press BOOT (~4s) → WiFi icon appears on display
2. Connect your phone/computer to:
   - **Network:** `V1-Simple`
   - **Password:** `setupv1g2`
3. Open browser: `http://192.168.35.5`
4. Long press BOOT again to turn WiFi off when done

---

## Touch Controls

| Gesture | When | Function |
|---------|------|----------|
| **Single tap** | Alert active | Mute/unmute the alert |
| **Triple tap** | No alert | Cycle profile slots (0→1→2→0) |

---

## Voice Alerts

The display has a built-in speaker that announces alerts:

### Priority Alerts
- **New alert:** Full announcement with band, frequency, and direction (e.g., "Ka 34.712 ahead")
- **Direction change:** Direction-only announcement when same alert moves (e.g., "behind")
- **Bogey count:** Optionally append "2 bogeys", "3 bogeys" when multiple alerts active
- **Laser:** Always includes direction ("Laser ahead") since there's no frequency to announce

### Secondary Alerts (Optional)
When enabled, non-priority alerts are announced after the priority stabilizes:
- **Per-band filters:** Choose which bands (Ka, K, X, Laser) to announce as secondary
- **Threat escalation:** When a secondary alert ramps up to 3+ bars, announces direction breakdown (e.g., "2 bogeys, 1 ahead, 1 behind")

### General
- **Auto-disable:** Voice alerts mute when a phone app (JBV1) is connected
- **5-second cooldown:** Prevents rapid-fire announcements

**Volume:** Adjust via the **blue slider** in settings mode (short press BOOT).

**Configure:** Go to `http://192.168.35.5/audio` to customize voice content, direction, bogey count, and secondary alerts.

---

## Web Interface Setup

### Profiles (/profiles)

Create V1 settings profiles to push to your detector:

1. Go to `http://192.168.35.5/profiles`
2. Click **"Pull from V1"** to capture current V1 settings
3. Review settings, then click **Save**
4. Name it (e.g., "Highway", "City", "Stealth")
5. Repeat to create additional profiles with different V1 configurations

### Auto-Push (/autopush)

Set up 3 quick-switch slots that automatically configure your V1:

| Slot | Suggested Use |
|------|---------------|
| 0 | Default / everyday |
| 1 | Highway / max sensitivity |
| 2 | Comfort / quieter urban |

**Setup each slot:**
1. Go to `http://192.168.35.5/autopush`
2. Select a profile from dropdown (created above)
3. Set V1 mode (All Bogeys / Logic / Advanced Logic)
4. Optional: Set main volume (0-9) and mute volume (0-9)
5. Optional: Enable **Dark Mode** (dims V1's display)
6. Optional: Enable **Mute to Zero** (complete silence when muted)
7. Optional: Set **Alert Persistence** (0-5 sec ghost after alert clears)
8. Optional: Enable **Priority Arrow Only** (shows only strongest alert direction)
9. Click **Save**

**Enable Auto-Push:** Toggle on "Auto-Push on Connect" to apply active slot when V1 connects.

**Switch slots:** Triple-tap the display (when no alert) to cycle 0→1→2→0.

### Colors (/colors)

Customize every color on the display:

1. Go to `http://192.168.35.5/colors`
2. **Display Style:** Classic (7-segment) or Modern font
3. **Custom colors:** Click any color swatch to open the color picker:
   - Band indicators (L, Ka, K, X)
   - Direction arrows (Front, Side, Rear)
   - Signal bars (6 levels, weak to strong)
   - Bogey counter, frequency, muted/persisted states
   - WiFi icons (AP mode, client connected)
   - BLE icons (connected, disconnected)
   - RSSI labels (V1 signal, Proxy signal)
4. **Test:** Click "Test" to preview colors on display
5. **Hide icons:** Toggle off WiFi, battery, BLE, RSSI, or profile indicator
6. Click **Save**

### Settings (/settings)

General configuration:

- **AP Name/Password:** Change WiFi network name and password
- **BLE Proxy:** Enable to relay V1 data to JBV1 app (advertises as "V1C-LE-S3")
- **Proxy Name:** Change BLE advertised name
- **Backup & Restore:** Download all settings to JSON file, or restore from a previous backup

### Audio (/audio)

Voice alert options:

- **Enable Voice Alerts:** Toggle spoken announcements on/off
- **Mute Voice at Volume 0:** Silence alerts when V1 volume is 0 (warning still plays)
- **Volume Fade:** Reduce V1 volume after initial alert, restore for new threats
- **Speed-Based Volume:** Boost V1 volume at highway speeds (requires OBD or GPS)

### GPS & OBD (/gps)

Speed and location sources:

- **GPS Status:** View current position, speed, heading, and satellite info
- **OBD-II:** Connect to ELM327 BLE adapter for accurate vehicle speed
- **Lockout Management:** View/clear learned lockout zones

---

## Troubleshooting

**V1 won't connect:**
- Disconnect JBV1 or other apps from V1 first
- Power cycle both devices

**Can't find WiFi network:**
- Long-press BOOT to start AP (look for WiFi icon on display)
- WiFi is off by default

**Upload fails:**
- Try different USB cable (must support data, not charge-only)
- Try different USB port
- Hold BOOT button while connecting USB
- Run `pio device list` to verify detection

**Factory reset:**
```bash
# Erase all settings (Windows):
"$HOME/.platformio/penv/Scripts/python.exe" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --port COM4 erase_flash

# Mac/Linux:
~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/cu.usbmodem* erase_flash

# Then re-upload firmware
./build.sh --all
```

---

## Documentation

- **Full Manual:** [docs/MANUAL.md](docs/MANUAL.md)
- **Windows Setup:** [docs/WINDOWS_SETUP.md](docs/WINDOWS_SETUP.md)

---

## Credits

Built on [Kenny Garreau's V1G2-T4S3](https://github.com/kennygarreau/v1g2-t4s3) - go star his repo!

**MIT License** - Use at your own risk. No warranty.
