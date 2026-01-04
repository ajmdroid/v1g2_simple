# V1 Gen2 Simple Display

> ⚠️ **Documentation is a constant work in progress.** For the most accurate information, view the source code directly.

A configurable touchscreen display for the Valentine One Gen2 radar detector, built on the Waveshare ESP32‑S3‑Touch‑LCD‑3.49.

**Version:** 1.1.26

**Features:**
- Wireless BLE connection with fast reconnection
- Real‑time alerts: bands, direction, signal strength, frequency
- Modern SvelteKit web UI for configuration
- Customizable colors for all display elements
- Auto‑push profile slots with volume settings
- Profile indicator with slot names on display
- V1 profile manager (create, edit, push)
- BLE proxy for simultaneous JBV1 app use
- 4 color themes (Standard, High Contrast, Stealth, Business)
- Touch‑to‑mute and triple‑tap profile cycling

**Disclaimer:** This is provided as‑is, with no warranty. Use at your own risk.

📖 **For comprehensive documentation, see [docs/MANUAL.md](docs/MANUAL.md)**

---

## Recent Updates

**January 2026 (v1.1.26):**
- Hide battery icon option in Colors settings
- Battery voltage calibration fix (4.1V max)
- Comprehensive MANUAL.md technical documentation
- Profile write verification with retry logic
- Bug fixes for display redraw and profile indicator

**Earlier (v1.1.x):**
- SvelteKit web interface with daisyUI
- V1 profile manager (create/edit/push profiles)
- Per‑slot volume settings (main + mute)
- Customizable slot names and colors
- Touch‑to‑mute
- Auto‑push profile system
- BLE proxy compatibility

---

## Requirements

**Hardware:**
- Waveshare ESP32‑S3‑Touch‑LCD‑3.49
- USB‑C cable (must support data, not charge-only)
- Valentine One Gen2 (BLE enabled)

**Software:**
- Visual Studio Code + PlatformIO
- Git (for cloning the repo)
- Node.js 18+ (for building the web UI)

**Windows users:** See [docs/WINDOWS_SETUP.md](docs/WINDOWS_SETUP.md) for detailed step-by-step instructions.

---

## Installation

### Step 1: Install Visual Studio Code

**Windows:**
1. Go to [code.visualstudio.com](https://code.visualstudio.com/)
2. Download and run the installer
3. Follow the prompts (default settings are fine)

**Mac:**
1. Go to [code.visualstudio.com](https://code.visualstudio.com/)
2. Download the `.zip` file
3. Unzip and drag "Visual Studio Code" to Applications folder
4. Open VS Code from Applications

**Linux (Ubuntu/Debian):**
```bash
sudo apt update
sudo apt install code
```

Or download from [code.visualstudio.com](https://code.visualstudio.com/) for other distros.

---

### Step 2: Install PlatformIO

1. Open Visual Studio Code
2. Click the **Extensions** icon in the left sidebar (or press `Ctrl+Shift+X` / `Cmd+Shift+X`)
3. Search for **"PlatformIO IDE"**
4. Click **Install** on the official extension (by PlatformIO)
5. Wait for installation to complete (may take a few minutes)
6. Restart VS Code when prompted

![PlatformIO Extension](https://docs.platformio.org/en/latest/_images/platformio-ide-vscode-pkg-installer.png)

---

### Step 3: Download This Project

**Option A: Using Git (Recommended)**
```bash
# Open a terminal and run:
git clone https://github.com/ajmdroid/v1g2_simple
cd v1g2_simple
code .
```

**Option B: Download ZIP**
1. Click the green **Code** button on this GitHub page
2. Click **Download ZIP**
3. Unzip the file to a folder you'll remember
4. Open VS Code
5. File → Open Folder → Select the `v1g2_simple` folder

---

### Step 4: Upload to the Display

1. **Connect your Waveshare display** to your computer with a USB-C cable
2. **Open the terminal** in VS Code:
   - Menu: Terminal → New Terminal
   - Or press: `Ctrl+` ` (backtick) on Windows/Linux, `Cmd+` ` on Mac

3. **Run the build script:**
   ```bash
   # Build and upload everything (recommended for first install)
   ./build.sh --all                                    # Mac/Linux
   ./build.sh --all --env waveshare-349-windows        # Windows
   ```

   **Windows users:** See [docs/WINDOWS_SETUP.md](docs/WINDOWS_SETUP.md) for detailed setup.

   This builds the web interface, uploads the filesystem, uploads firmware, and opens the serial monitor.

4. **Wait for it to finish** (first time takes 2-5 minutes to download libraries)

5. **Look for this message:**
   ```
   ======== [SUCCESS] Took X seconds ========
   ```

After upload, the display boots to the main UI.

**Build Script Options:**
```bash
./build.sh              # Build only (no upload)
./build.sh -u           # Build and upload firmware
./build.sh -f           # Build and upload filesystem only
./build.sh -u -m        # Build, upload firmware, open monitor
./build.sh --all        # Full build + upload filesystem + firmware + monitor
./build.sh --clean -a   # Clean build and upload everything
./build.sh --skip-web   # Skip web interface rebuild
./build.sh --env waveshare-349-windows  # Use Windows environment
./build.sh --help       # Show all options
```

**Manual PlatformIO commands** (alternative to build.sh):
```bash
# Mac/Linux:
pio run -e waveshare-349 -t upload      # Upload firmware only
pio run -e waveshare-349 -t uploadfs    # Upload web filesystem

# Windows:
pio run -e waveshare-349-windows -t upload      # Upload firmware only
pio run -e waveshare-349-windows -t uploadfs    # Upload web filesystem

pio device monitor                       # Open serial monitor
```

### Additional build helpers

The helper scripts `scripts/pio-size.sh` and `scripts/pio-check.sh` wrap the `pio run -e waveshare-349 -t size` and `pio check -e waveshare-349` commands respectively.

---

### Upload Troubleshooting

**"No device found" or "Permission denied":**

> **Note:** The Waveshare ESP32-S3 board uses native USB (built into the ESP32-S3 chip), so no external USB-to-serial driver is typically needed. However, some systems may require configuration.

**Windows:**
- The device should appear automatically as a COM port
- If not detected, check Device Manager for "USB Serial Device"
- Try installing [ESP32-S3 USB drivers](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/establish-serial-connection.html) if needed

**Mac:**
- Usually works out of the box with macOS 10.15+
- Device appears as `/dev/cu.usbmodem*`
- If not detected, try a different USB port or cable

**Linux:**
- Add yourself to the dialout group:
  ```bash
  sudo usermod -a -G dialout $USER
  ```
- Log out and back in, or run:
  ```bash
  newgrp dialout
  ```

**Still not working?**
- Try a different USB cable (some are charge-only)
- Try a different USB port
- Press and hold the BOOT button on the display while connecting USB

---

## Usage

### First Boot and WiFi

When you first power on the display, it creates a WiFi access point:

| Setting | Value |
|---------|-------|
| **WiFi Name (SSID)** | `V1-Display` |
| **Password** | `valentine1` |
| **Web Interface** | `http://192.168.35.5` |

**Security note:** Change the default AP password after setup via Settings → AP Password.

1. **Connect your phone or computer** to the `V1-Display` WiFi network
2. **Open a browser** and go to `http://192.168.35.5`
3. **Configure your settings:**
   - Set display brightness
   - Enable BLE proxy for JBV1 app compatibility
   - Adjust color theme
   - Configure auto-push profiles

### Main Screen

The display shows real-time alerts from your V1 (640×172 AMOLED, landscape orientation):

```
┌────────────────────────────────────────────────────────────────┐
│ 1.             HIGHWAY                                         │
│(bogey)       (profile)                                         │
│    L      ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓                  ↑   │
│   Ka                                                       ←→  │
│    K               34.728                                  ↓   │
│    X                                                           │
│ 📶🔋                                                           │
└────────────────────────────────────────────────────────────────┘
```

**Layout:**
- **Top-left**: Bogey counter (7-segment digit)
- **Top-center**: Active profile name (e.g., "HIGHWAY")
- **Left**: Band indicators (L/Ka/K/X) - light up when detected
- **Center**: Signal strength bars and frequency in GHz (7-segment style)
- **Right**: Direction arrows (↑ front, ←→ side, ↓ rear)
- **Bottom-left**: WiFi + battery icons (can be hidden in settings)

**Touch Controls:**
- **Single tap**: Mute/unmute active alert
- **Triple tap**: Cycle through profile slots (0→1→2→0)
- **Long press**: Also cycles profiles

### Web Interface

Access via `http://192.168.35.5`:

**Home** (`/`):
- V1 connection status
- Quick access to all settings pages
- System info (firmware version)

**Settings** (`/settings`):
- WiFi AP name and password
- BLE proxy toggle for JBV1 app
- Display on/off and resting mode
- Brightness control (0-255)

**Colors** (`/colors`):
- Color theme selection (Standard, High Contrast, Stealth, Business)
- Custom colors for all display elements
- Per-band color customization (L/Ka/K/X)
- Signal bar gradient colors
- Hide WiFi/profile/battery indicators

**Auto-Push** (`/autopush`):
- Three configurable slots (Default, Highway, Comfort)
- Profile + V1 mode per slot
- Per-slot volume settings (main + mute volume)
- Custom slot names and colors
- Quick-push buttons
- Auto-push on V1 connection

**V1 Profiles** (`/profiles`):
- Create and edit V1 profiles
- Configure bands, sensitivity, filters
- Push profiles directly to V1
- Pull current settings from V1

**Devices** (`/devices`):
- Known V1 device management
- Friendly names for devices
---

## Features

**Color Themes:**
- Standard, High Contrast, Stealth, Business
- Change via Settings → Color Theme
- Full per-element color customization available

**Auto‑Push Profiles:**

Set up 3 profile slots for different driving scenarios:

| Slot | Default Name | Purpose |
|------|--------------|---------|
| 0 | Default | Everyday driving |
| 1 | Highway | High sensitivity |
| 2 | Comfort | Quieter urban driving |

**How to use:**
1. **Create profiles first** in the V1 Profiles page (`/profiles`)
2. Go to Auto-Push (`/autopush`) in the web interface
3. Configure each slot with a saved profile and V1 mode
4. Set volume overrides if desired
5. Enable **Auto-Push** to apply on connection
6. Use **triple-tap** on display to cycle slots

**Display Modes:**

- **Display On/Off:** Full stealth mode (screen black even with alerts)
- **Resting Display:** Shows idle animation or stays blank when no alerts

**BLE Proxy (JBV1 Compatibility):**
- Device advertises as "V1C-LE-S3" when connected to V1
- JBV1 or V1 Companion app can connect through display
- Simultaneous display + app use

---

## Customization

Don't like something? The code is designed to be hackable:

**Change WiFi defaults:** Edit `include/config.h`
**Adjust signal bar thresholds:** Edit `include/config.h`  
**Modify color themes:** Edit `include/color_themes.h`
**Customize web UI:** Edit files in `interface/src/routes/`

After changes:

```bash
./build.sh --all
```

Or for firmware-only changes:
```bash
./build.sh -u -m
```

---

## Troubleshooting

### V1 Connection

1. **Check V1 BLE is enabled** (V1 menu: Setup → Bluetooth)
2. **Ensure V1 isn't connected to JBV1** or another device
3. **Power cycle both devices**
4. **Check serial monitor:**
   ```bash
   pio device monitor
   ```
   Look for: `*** FOUND V1: 'V1Gxxxxx'`

### Display

**Screen stays black:**
- Check brightness setting (lower = brighter, 0 = full brightness)
- Verify "Display On" is enabled in settings
- Check serial monitor for errors

**Touch not working:**
- Some displays have plastic film over screen - remove it
- Touch requires initial calibration - just tap firmly

**Colors look weird:**
- Try different color themes in settings
- Check display isn't in direct sunlight (AMOLED)

### Reset to Factory Defaults

Settings are stored in NVS (non-volatile storage) and persist across firmware updates. To fully reset:

**Full flash erase (erases ALL data including saved settings):**
```bash
# Windows (Git Bash) - use PlatformIO's Python:
"$HOME/.platformio/penv/Scripts/python.exe" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --port COM4 erase_flash

# Mac/Linux:
~/.platformio/packages/tool-esptoolpy/esptool.py --port /dev/cu.usbmodem* erase_flash

# Then re-upload firmware and filesystem (Windows - add PATH first):
export PATH="$PATH:$HOME/.platformio/penv/Scripts"
pio run -e waveshare-349-windows -t upload
pio run -e waveshare-349-windows -t uploadfs

# Mac/Linux:
pio run -e waveshare-349 -t upload
pio run -e waveshare-349 -t uploadfs
```

**Note:** Replace `COM4` with your actual port. Use `pio device list` to find it. (Mac/Linux: `/dev/cu.usbmodem*` or `/dev/ttyACM0`)

### WiFi

**Can't find V1-Display network:**
- Wait 30 seconds after boot
- Power cycle the display

**Can't access web interface:**
- Verify you're connected to V1-Display WiFi
- Use `http://192.168.35.5` (not https)
- Clear browser cache

---

## Technical Details

### Hardware

**Waveshare ESP32-S3-Touch-LCD-3.49:**
- **CPU**: ESP32-S3 @ 240MHz, 8-16MB Flash, 8MB PSRAM
- **Display**: 640×172 AMOLED (AXS15231B controller, QSPI interface)
- **Touch**: AXS15231B integrated capacitive touch (single-touch only)
- **Storage**: LittleFS (internal flash)
- **Battery**: Optional LiPo via TCA9554 power management

**Note:** Waveshare advertises 16MB flash, but esptool detects some units as 8MB. The Windows build uses 8MB partitions for compatibility.

Pin mapping is defined in [platformio.ini](platformio.ini) via build flags (LCD_CS, LCD_SCLK, LCD_DATA0-3, LCD_RST, LCD_BL).

### BLE Protocol

**V1 Gen2 Service:**
- Service UUID: `92A0AFF4-9E05-11E2-AA59-F23C91AEC05E`
- Display Data: `92A0B2CE-9E05-11E2-AA59-F23C91AEC05E` (notify)
- Commands: `92A0B6D4-9E05-11E2-AA59-F23C91AEC05E` (write)

**BLE Proxy for JBV1:**
- Advertises as: `V1C-LE-S3`
- Forwards data bidirectionally between V1 and JBV1
- Enables simultaneous display + app use

### Software Architecture

- **NimBLE 2.3.7**: Stable dual-role BLE (client + server)
- **Arduino_GFX**: Hardware-accelerated display driver
- **Preferences API**: Persistent settings in flash
- **FreeRTOS**: Task queue for BLE data handling

---

## 🤝 Contributing & Support

### This is a Personal Project

I built this for myself because I wanted a customizable V1 display. I'm sharing it in case it helps others, but:

- ✅ **Pull requests welcome** - I'll review them when I can
- ✅ **Issues/suggestions appreciated** - I might implement them
- ⚠️ **No guarantees** - I work on this in my spare time
- ⚠️ **No support obligations** - I'll help when I can, but can't promise anything
- ✅ **Fork freely** - Build something better! I might even use it

### I'm Not a Developer

I'm an IT guy and a nerd at heart, but software development isn't my day job. This project has been a learning experience with help from:
- Kenny Garreau's excellent V1G2-T4S3 project (my primary reference)
- Valentine Research's ESP library documentation
- AI assistance (Claude) for code structure and debugging
- The community for suggestions and bug reports

If you see something that could be better, PRs are welcome. If you want to take this and make something amazing, go for it!

---

## 📜 License & Attribution

**MIT License** - See [LICENSE](LICENSE) file for full text.

**NO WARRANTY:** This software is provided "as-is" without warranty of any kind, express or implied, including but not limited to the warranties of merchantability, fitness for a particular purpose and noninfringement. In no event shall the authors be liable for any claim, damages or other liability, whether in an action of contract, tort or otherwise, arising from, out of or in connection with the software or the use or other dealings in the software.

**USE AT YOUR OWN RISK:** You are solely responsible for any damage to your equipment, your V1, your car, or anything else. The author has ZERO liability.

### Acknowledgments

This project would not exist without:

**🌟 [Kenny Garreau's V1G2-T4S3 Project](https://github.com/kennygarreau/v1g2-t4s3) 🌟**
- Primary inspiration and reference
- BLE protocol implementation guidance
- Display logic and UI design concepts
- This project is essentially a port/adaptation of Kenny's excellent work
- **Seriously, go star Kenny's repo - his work made this possible**

**Valentine Research:**
- [ESP library reference](https://github.com/valentineresearch)
- Protocol documentation
- Excellent hardware and support

**Waveshare:**
- ESP32-S3-Touch-LCD-3.49 hardware and documentation
- Sample code and driver libraries

**Community:**
- RadarDetector.net forum members
- GitHub contributors and testers
- Everyone who provided feedback and bug reports

---

## 🔗 Links

- **Documentation**: [docs/MANUAL.md](docs/MANUAL.md) - Comprehensive technical manual
- **Setup Guides**: [docs/SETUP.md](docs/SETUP.md) (General) | [docs/WINDOWS_SETUP.md](docs/WINDOWS_SETUP.md) (Windows)
- **Hardware**: [Waveshare ESP32-S3-Touch-LCD-3.49 on Amazon](https://www.amazon.com/dp/B0FQM41PGX)
- **Kenny's Project**: [V1G2-T4S3](https://github.com/kennygarreau/v1g2-t4s3)
- **Valentine Research**: [valentineresearch.com](https://www.valentineresearch.com/)

---

## 📸 Gallery

[Add photos/screenshots of your display in action]

---

**Questions?** Open an issue on GitHub. I'll help when I can, but remember - personal project, no guarantees! 😊

**Enjoy your customizable V1 display!** 🎉
