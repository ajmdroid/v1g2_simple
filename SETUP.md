# Setup Guide: V1 Gen2 Simple Display

This guide will walk you through installing all required tools and flashing the firmware to your Waveshare ESP32-S3-Touch-LCD-3.49.

---

## 1. Install Visual Studio Code (VS Code)

- Download and install VS Code from: https://code.visualstudio.com/

---

## 2. Install PlatformIO Extension

- Open VS Code.
- Go to the Extensions view (square icon on sidebar or `Ctrl+Shift+X`).
- Search for "PlatformIO IDE" and click **Install**.

---

## 3. Install Git

### Mac (macOS)
- Open Terminal and run:
  ```sh
  xcode-select --install
  ```
  (This installs Git and developer tools.)

### Windows
- Download Git for Windows: https://git-scm.com/download/win
- Run the installer and follow the prompts (default options are fine).

### Linux (Debian/Ubuntu)
- Open Terminal and run:
  ```sh
  sudo apt update && sudo apt install git
  ```

---

## 4. Clone the Repository

- Open a terminal (VS Code Terminal or system terminal).
- Run:
  ```sh
git clone https://github.com/ajmdroid/v1g2_simple
cd v1g2_simple
  ```

---

## 5. Connect Your Display

- Plug the device into your computer via USB-C cable.
- Make sure the cable supports data (not just charging).

**Supported display:**
- Waveshare ESP32-S3-Touch-LCD-3.49

---

## 6. Build and Flash the Firmware

- In VS Code, open the `v1g2_simple` folder.
- Open the PlatformIO sidebar (alien head icon).
- Select your environment:
  - `waveshare-349`
- Click **Build** (checkmark icon) to compile.
- Click **Upload** (right arrow icon) to flash the firmware.

**Or use the terminal:**
```sh
# For Waveshare 3.49"
pio run -e waveshare-349 --target upload
```

---

## 7. Connect to WiFi Configuration

After flashing, the display will create a WiFi Access Point:

| Setting | Value |
|---------|-------|
| SSID | `V1-Display` |
| Password | `valentine1` |
| Config URL | `http://192.168.35.5` |

Connect to this network and open the URL to configure WiFi and display settings.

---

## 8. Monitor Serial Output (Optional)

- Click **Monitor** in PlatformIO sidebar, or run:
  ```sh
  pio device monitor
  ```
- Set baud rate to 115200 if prompted.

---

## Troubleshooting
- If the device is not detected, try a different USB cable or port.
- On Windows, you may need to install the "CP210x USB to UART Bridge VCP Drivers" from Silicon Labs.
- On Linux, you may need to add your user to the `dialout` group:
  ```sh
  sudo usermod -aG dialout $USER
  # Then log out and back in
  ```
- For Waveshare-specific issues, see [WAVESHARE_349.md](WAVESHARE_349.md)

---

You’re ready to go! For advanced configuration, see the main README.
