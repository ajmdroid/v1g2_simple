# V1 Gen2 Simple Display

A customizable touchscreen display for the Valentine1 Gen2 radar detector. Built for the [Waveshare ESP32-S3-Touch-LCD-3.49](https://www.amazon.com/dp/B0FQM41PGX) - an affordable, bright AMOLED display that turns your V1 into a modern tech setup.

**What You Get:**
- 🔵 **Wireless Connection**: Connects to your V1 via Bluetooth LE with fast reconnection
- 📊 **Real-time Alerts**: Shows radar bands (Ka, K, X, Laser), direction, and signal strength
- 🎨 **Custom Themes**: Multiple color schemes (Standard, High Contrast, Stealth)
- 👆 **Touch Control**: Tap screen to mute/unmute
- 📱 **WiFi Setup**: Web interface for all settings - no code editing needed
- 🌐 **Multi-Network WiFi**: Auto-switch between up to 3 saved networks with encrypted credentials
- 🔄 **Internet Passthrough**: Built-in NAT/NAPT router - share your WiFi connection via AP mode
- 🎯 **Auto-Push Profiles**: 3 quick-access profile slots (Default, Highway, Passenger Comfort)
- 💾 **Alert Logging**: SD card logging with web-based replay
- 🔧 **V1 Profile Manager**: Create, edit, and push settings to your V1
- 🔒 **Security Hardened**: XSS protection, password obfuscation, secure web interface

**⚠️ Disclaimer:**  
This is a personal project I built for myself. There is NO WARRANTY of any kind. Use at your own risk. I have ZERO liability for anything that happens. If you break your V1, your ESP32, or somehow cause the apocalypse, that's on you. Seriously. No refunds, no guarantees, no support obligations. It works for me. Your mileage may vary.

---

## 🆕 Recent Updates

**Latest (Dec 2025):**
- ✅ **WiFi NAT/NAPT Router**: Internet passthrough in AP+STA mode
- ✅ **Multi-Network WiFi**: Auto-switch between up to 3 saved networks
- ✅ **Security Hardening**: Fixed XSS vulnerability, added HTML entity encoding
- ✅ **Password Obfuscation**: Encrypted WiFi credential storage
- ✅ **Time Management**: NTP sync with automatic timezone handling
- ✅ **Fast BLE Reconnection**: Aggressive scanning for quicker V1 connection

**Previous:**
- Touch-to-mute functionality
- Alert database with web replay
- Auto-push profile system
- BLE proxy for JBV1 app compatibility

---

## 📦 What You Need

**Hardware ($35-40):**
- [Waveshare ESP32-S3-Touch-LCD-3.49](https://www.amazon.com/dp/B0FQM41PGX) (~$35 on Amazon)
- USB-C cable (for programming)
- Valentine1 Gen2 radar detector (with BLE enabled)
- Optional: MicroSD card for alert logging (FAT32 formatted)

**Software (Free):**
- Visual Studio Code
- PlatformIO extension
- This code

---

## 🚀 Installation Guide

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

### Step 2: Install PlatformIO Extension

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

### Step 4: Upload to Your Display

1. **Connect your Waveshare display** to your computer with a USB-C cable
2. **Open the terminal** in VS Code:
   - Menu: Terminal → New Terminal
   - Or press: `Ctrl+` ` (backtick) on Windows/Linux, `Cmd+` ` on Mac

3. **Run the upload command:**
   ```bash
   pio run -e waveshare-349 -t upload
   ```

4. **Wait for it to finish** (first time takes 2-5 minutes to download libraries)

5. **Look for this message:**
   ```
   ======== [SUCCESS] Took X seconds ========
   ```

**That's it!** Your display should boot up with the V1 logo.

---

### Troubleshooting Upload Issues

**"No device found" or "Permission denied":**

**Windows:**
- Install [CH340 USB drivers](https://www.wch-ic.com/downloads/CH341SER_EXE.html)
- Check Device Manager to see if the device shows up

**Mac:**
- Install [CH340 drivers for macOS](https://github.com/adrianmihalko/ch340g-ch34g-ch34x-mac-os-x-driver)
- May need to approve in System Preferences → Security & Privacy

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

## 🎮 Using Your Display

### First Boot - WiFi Setup

When you first power on the display, it creates a WiFi access point:

| Setting | Value |
|---------|-------|
| **WiFi Name (SSID)** | `V1-Display` |
| **Password** | `valentine1` |
| **Web Interface** | `http://192.168.35.5` |

> ⚠️ **Security Note:** Change the default WiFi password after setup! Anyone nearby can connect to `V1-Display` with the default password and access your settings. Go to Settings → AP Password to change it.

1. **Connect your phone or computer** to the `V1-Display` WiFi network
2. **Open a browser** and go to `http://192.168.35.5`
3. **Configure your settings:**
   - Choose WiFi mode (AP mode or connect to your home WiFi)
   - Set display brightness
   - Enable BLE proxy for JBV1 app compatibility
   - Adjust color theme

### Main Screen

The display shows real-time alerts from your V1:

```
┌─────────────────────────────┐
│  Ka  K  X  LASER    [WiFi] │ ← Band indicators & status
│                             │
│        ↑  →  ←             │ ← Direction arrows
│                             │
│      ▓▓▓▓▓▓▓▓▓             │ ← Signal strength bars
│                             │
│       34.728 GHz           │ ← Frequency display
│                             │
│     [MUTED] 👁️            │ ← Mute status, display toggle
└─────────────────────────────┘
```

**Touch Controls:**
- **Tap anywhere** on the screen to **mute/unmute** your V1

### Web Interface Features

Access via `http://192.168.35.5` (or your configured IP):

**📊 Home Dashboard**
- Live V1 connection status
- Quick access to all settings
- Alert log viewer

**⚙️ Settings Page**
- WiFi configuration (AP mode or connect to network)
- Display brightness (inverted: lower number = brighter)
- Color theme selection
- Display mode (full stealth vs resting display)
- BLE proxy toggle (for JBV1 app)

**🎯 Auto-Push Profiles** (`/autopush`)
- **3 Quick-Access Slots:**
  - 🏠 **Slot 0**: Default profile
  - 🏎️ **Slot 1**: Highway profile  
  - 👥 **Slot 2**: Passenger Comfort profile
- Each slot stores: V1 profile + operating mode
- **Quick-Push Buttons**: Tap to activate and push immediately
- **Auto-Enable**: Automatically push active profile when V1 connects

**📁 V1 Profile Manager** (`/v1settings`)
- Create and edit V1 profiles
- Adjust all V1 settings (bands, sensitivity, filters)
- Push profiles to V1 wirelessly
- Save multiple profiles for different situations

**📜 Alert Logs** (`/alerts`)
- View logged alerts from SD card
- Filter by date, band, frequency
- Replay mode to see historical data
- Export to CSV

---

## 🎨 Features in Detail

### Color Themes

Choose from 3 built-in themes (matches firmware):
- **Standard**: Classic red/amber warnings
- **High Contrast**: Bright palette for visibility
- **Stealth**: Muted, low-light friendly

Change via web interface: Settings → Color Theme

### Auto-Push Profile System

Set up 3 profiles for different driving scenarios:

1. **Default Profile** (🏠): Your everyday settings
2. **Highway Profile** (🏎️): Max sensitivity for long trips
3. **Passenger Comfort** (👥): Quieter settings with passengers

**How to use:**
1. **Create profiles first** in the V1 Profile Manager (`/v1settings`)
2. Go to `/autopush` in the web interface
3. Configure each slot with a saved profile and mode
4. Click a **Quick-Push** button to activate and send to V1
5. Enable **Auto-Push** to apply on connection

### Display Modes

**Display On (Full Stealth):**
- Display OFF = Everything black (even with alerts)
- Display ON = Normal operation

**Resting Display:**
- Resting ON = Shows logo when idle
- Resting OFF = Blank screen until alert

### Alert Logging

With a microSD card inserted:
- Automatically logs every alert
- Stores: timestamp, band, frequency, direction, strength
- Access logs via web interface (`/alerts`)
- Format: FAT32 (not exFAT)

### WiFi Networking

**Multi-Network Support:**
- Store up to 3 WiFi networks
- Automatic failover between saved networks
- Password obfuscation for credential security
- Configure via web interface Settings page

**Internet Passthrough (NAT/NAPT Router):**
- Connect display to home/phone WiFi (Station mode)
- Enable AP mode simultaneously
- Display acts as router - shares internet to JBV1 app
- Built-in NAT/NAPT forwarding
- Useful for using JBV1 app while display is connected to external WiFi

**WiFi Modes:**
- **AP Only**: Display creates WiFi network (no internet)
- **Station Only**: Display connects to your WiFi
- **AP + Station**: Router mode with internet passthrough

---

## 🛠️ Customization

Don't like something? The code is designed to be hackable:

**Change WiFi defaults:** Edit `include/config.h`
**Adjust signal bar thresholds:** Edit `include/config.h`  
**Modify color themes:** Edit `include/color_themes.h`
**Change web UI:** Edit `src/wifi_manager.cpp`

After changes, run `pio run -e waveshare-349 -t upload` to update your display.

---

## 🐛 Troubleshooting

### V1 Won't Connect

1. **Check V1 BLE is enabled** (V1 menu: Setup → Bluetooth)
2. **Ensure V1 isn't connected to JBV1** or another device
3. **Power cycle both devices**
4. **Check serial monitor:**
   ```bash
   pio device monitor
   ```
   Look for: `*** FOUND V1: 'V1Gxxxxx'`

### Display Issues

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

### WiFi Issues

**Can't find V1-Display network:**
- Wait 30 seconds after boot
- Check WiFi mode is "AP" or "AP+STA" in settings
- Power cycle the display

**Can't connect to saved WiFi network:**
- Verify network is in range and password is correct
- Multi-network will try all 3 saved networks in order
- Check serial monitor for connection attempts
- Remove and re-add network if issues persist

**Internet passthrough not working:**
- Ensure display is connected to WiFi with internet (Station mode)
- AP mode must also be enabled (AP+STA mode)
- NAT is enabled automatically when both modes active
- Devices connected to V1-Display AP should get internet access
- Power cycle the display

**Can't access web interface:**
- Verify you're on the correct IP (default: 192.168.35.5)
- Try `http://` not `https://`
- Clear browser cache

### SD Card Issues

**Logs not saving:**
- Format card as FAT32 (not exFAT or NTFS)
- Use 32GB or smaller card
- Check card is fully inserted
- Try a different brand (SanDisk works well)

---

## 📚 Technical Details

### Hardware

**Waveshare ESP32-S3-Touch-LCD-3.49:**
- **CPU**: ESP32-S3 (240MHz, 8MB Flash, 2MB PSRAM)
- **Display**: 172×640 AMOLED (AXS15231B controller, QSPI interface)
- **Touch**: AXS15231B integrated capacitive touch (I2C 0x3B)
- **SD Card**: SDMMC 1-bit mode (pins: CLK=41, CMD=39, D0=40)
- **Backlight**: Inverted PWM on GPIO 8 (0=full bright, 255=off)

See [WAVESHARE_349.md](WAVESHARE_349.md) for complete pin mapping.

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

- **NimBLE 2.2.3**: Stable dual-role BLE (client + server)
- **Arduino_GFX**: Hardware-accelerated display driver
- **lvgl 8.4.0**: UI graphics library (lightweight config)
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

- **Hardware**: [Waveshare ESP32-S3-Touch-LCD-3.49 on Amazon](https://www.amazon.com/dp/B0FQM41PGX)
- **Kenny's Project**: [V1G2-T4S3](https://github.com/kennygarreau/v1g2-t4s3)
- **Valentine Research**: [valentineresearch.com](https://www.valentineresearch.com/)
- **Documentation**: See `docs/` folder for detailed guides

---

## 📸 Gallery

[Add photos/screenshots of your display in action]

---

**Questions?** Open an issue on GitHub. I'll help when I can, but remember - personal project, no guarantees! 😊

**Enjoy your customizable V1 display!** 🎉

- [V1 Gen2 Official Site](https://www.valentine1.com/)
- [V1 Tech Display](https://www.valentine1.com/v1-detectors/tech-display/)
- [ESP32-S3 Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/)
- [PlatformIO Documentation](https://docs.platformio.org/)
