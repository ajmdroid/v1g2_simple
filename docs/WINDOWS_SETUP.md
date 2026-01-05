# Windows Setup and Flash Guide (Waveshare ESP32-S3-Touch-LCD-3.49)

This guide is a step-by-step, Windows-specific checklist to build the firmware, web assets, and flash the Waveshare ESP32-S3-Touch-LCD-3.49 using this repository. Follow each numbered step in order without skipping. Commands are intended for **Git Bash** unless otherwise noted.

## 0) Hardware You Need
- Waveshare ESP32-S3-Touch-LCD-3.49
- USB-C data-capable cable (swap if the device is not detected)
- Windows PC with administrator rights

## 1) Install Required Software (Windows)
1. **Git for Windows (with Git Bash + Unix tools):** Download https://git-scm.com/download/win and run the installer. Use these settings during installation:
   
   **Components to install:**
   - ✅ Windows Explorer integration → **Open Git Bash here**
   - ✅ **Associate .sh files to be run with Bash** (critical for build scripts)
   - ✅ Git LFS (Large File Support)
   - ✅ Add a Git Bash Profile to Windows Terminal (optional but useful)
   
   **Configuration screens (accept these defaults):**
   - Default editor: **Use Visual Studio Code** (or leave as Vim if VS Code not installed yet)
   - Initial branch name: **Let Git decide**
   - PATH environment: **Git from the command line and also from 3rd-party software** (recommended)
   - SSH executable: **Use bundled OpenSSH**
   - HTTPS backend: **Use the native Windows Secure Channel library**
   - Line endings: **Checkout Windows-style, commit Unix-style line endings**
   - Terminal emulator: **Use MinTTY** (the default terminal of MSYS2)
   - Default pull behavior: **Fast-forward or merge** (default)
   - Credential helper: **Git Credential Manager**
   - Extra options: ✅ **Enable file system caching**, ✅ **Enable symbolic links**
   
   After install completes, open **Git Bash** and verify:
   ```bash
   git --version
   bash --version
   gzip --version
   ```
2. **Node.js 18 LTS or newer:** Download the Windows installer from https://nodejs.org/ and install. During installation:
   - Accept all default options
   - **Do NOT check** "Automatically install the necessary tools" (the checkbox about native modules/Python/Chocolatey)
   
   After installation completes, **restart your computer** (or at minimum, close ALL terminals and VS Code). This is required for the PATH changes to take effect.
   
   After restart, open a **new** Git Bash or PowerShell window and verify:
   ```bash
   node -v
   npm -v
   ```
   
   If `node: command not found`, the installer didn't update PATH correctly. Fix it manually:
   - Press `Win + X` → **System** → **Advanced system settings** → **Environment Variables**
   - Under "System variables", select **Path** → **Edit**
   - Click **New** and add: `C:\Program Files\nodejs`
   - Click **OK** on all dialogs
   - **Close and reopen all terminals**
   
   **Temporary workaround (if you can't restart right now):** Run this in Git Bash:
   ```bash
   export PATH="$PATH:/c/Program Files/nodejs"
   node -v
   npm -v
   ```
   This only lasts for the current terminal session.
3. **Visual Studio Code:** Download https://code.visualstudio.com/ and install with default options.
4. **PlatformIO IDE extension for VS Code:** Open VS Code → Extensions (`Ctrl+Shift+X`) → search **PlatformIO IDE** → Install. 
   
   **Note:** PlatformIO installation takes several minutes (5-10 minutes) as it downloads Python, compilers, and toolchains in the background. Wait for the "PlatformIO: Home" button to appear in the bottom status bar before proceeding. Restart VS Code after install completes.

## 2) Prepare a Workspace
1. Open **Git Bash**.
2. Choose a parent folder (example: `C:\Users\<you>\src`).
3. Clone the repo and open it in VS Code:
   ```bash
   git clone https://github.com/ajmdroid/v1g2_simple
   cd v1g2_simple
   code .
   ```

## 3) Install Web UI Dependencies (SvelteKit)
**Note:** You already installed Node.js and npm in step 1.2. This step uses npm to download the project's JavaScript packages (SvelteKit, Vite, Tailwind, etc.) listed in `interface/package.json`.

1. In VS Code, open a new terminal and select **Git Bash** as the shell (Terminal → Select Default Profile → Git Bash, then `+` to open one).
2. From the repo root, install interface deps (only once, or after `node_modules` removal):
   ```bash
   cd interface
   npm install
   cd ..
   ```
3. Confirm Node scripts run (sanity check):
   ```bash
   cd interface
   npm run build
   cd ..
   ```
   This runs Vite and the `tools/compress_web_assets.sh` gzip step; it should finish without errors. If it succeeds once, later runs will be triggered automatically by the main build script.

## 4) Verify PlatformIO CLI Availability
1. Still in VS Code, open a **PlatformIO Core CLI** shell (Command Palette `Ctrl+Shift+P` → “PlatformIO: New Terminal”). PlatformIO initializes environment variables for `pio`.
2. Check that `pio` is on PATH:
   ```bash
   pio --version
   ```
   If not found, close/reopen the PlatformIO terminal or restart VS Code after the extension install completes.

## 5) Connect the Device
1. Plug the Waveshare board to your PC with the USB-C cable.
2. Open **Device Manager** → Ports (COM & LPT). You should see **USB Serial Device (COMx)**. Note the COM port (e.g., `COM6`).
3. If no COM port appears, try a different cable/port. The ESP32-S3 uses native USB; no extra driver is usually required. If Windows still does not create a COM port, install the Espressif USB driver from the ESP-IDF link: https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/establish-serial-connection.html.

**To list all connected serial devices:**
```bash
pio device list
```
This shows all COM ports with device descriptions. Example output:
```
COM4
----
Hardware ID: USB VID:PID=303A:1001 SER=20:6E:F1:9B:D4:80
Description: USB Serial Device (COM4)
```

## 6) One-Command Build + Flash
**IMPORTANT:** The build script needs BOTH `pio` (from PlatformIO) AND `bash`/`gzip` (from Git). Configure PlatformIO terminals to use Git Bash:

**One-time setup: Change PlatformIO terminal to Git Bash:**
1. Open any terminal in VS Code
2. Click the dropdown (⌄) next to the `+` in the terminal panel
3. Select **"Select Default Profile"**
4. Choose **"Git Bash"**
5. Close any open terminals

**Now run the build:**
1. Open a **new PlatformIO Core CLI** terminal (`Ctrl+Shift+P` → "PlatformIO: New Terminal"). It should now be Git Bash. From the repo root:
   ```bash
   ./build.sh --all
   ```
   
   **Note:** The script auto-detects Windows and uses the correct environment (`waveshare-349-windows`) and PlatformIO path automatically. No `--env` flag needed!
   
   **If you have multiple USB devices connected**, specify the COM port:
   ```bash
   ./build.sh --all --upload-port COM6
   ```
   
   **Manual commands (without build.sh):** If the build script has issues, you can run PlatformIO directly:
   ```bash
   export PATH="$PATH:$HOME/.platformio/penv/Scripts"
   pio run -e waveshare-349-windows -t upload
   pio run -e waveshare-349-windows -t uploadfs
   ```
   
   What this does:
   - Builds the web UI (reuses `interface/node_modules`, rebuilds, gzips assets, deploys to `data/`).
   - Builds firmware via PlatformIO.
   - Uploads filesystem (LittleFS) to the device.
   - Uploads firmware to the device.
   - Opens the serial monitor.

   **FIRST BUILD ONLY:** The first time you run this, it will download the ESP32 Arduino framework (~500MB) and all toolchain packages. This can take 5-15 minutes depending on your internet speed and **feels like forever**. You'll see "Configuring toolchain packages from a remote source..." for several minutes. Be patient - this only happens once. After the first build, everything is cached in the `.pio/` directory and future builds take only 30-60 seconds.

2. Wait for `======== [SUCCESS] Took ... seconds ========` in the terminal.
3. Leave the monitor open to confirm boot logs; press `Ctrl+C` to exit when done.

## 7) Manual Commands (if you prefer PlatformIO buttons)
**Note:** On Windows, use the `waveshare-349-windows` environment. The `waveshare-349` environment is for Mac/Linux.

- Build only (firmware):
  ```bash
  pio run -e waveshare-349-windows
  ```
- Upload firmware only:
  ```bash
  pio run -e waveshare-349-windows -t upload
  ```
- Upload filesystem (after `npm run build && npm run deploy`):
  ```bash
  pio run -e waveshare-349-windows -t uploadfs
  ```
- Upload to specific port (if you have multiple devices):
  ```bash
  pio run -e waveshare-349-windows -t upload --upload-port COM6
  pio run -e waveshare-349-windows -t uploadfs --upload-port COM6
  ```
- Serial monitor:
  ```bash
  pio device monitor -b 115200
  ```

## 8) First-Boot Verification
1. After flashing, the display should boot and show the idle “SCAN” animation.
2. On your phone/PC, connect to WiFi SSID **V1-Simple**, password **setupv1g2**.
3. Browse to `http://192.168.35.5`; the web UI should load.

## 9) Common Windows Pitfalls
- **Don't click `.sh` files directly in Windows Explorer** → Always run them from the command line with `./build.sh`. If Windows asks what app to open .sh files with, just cancel.
- **`./build.sh`: command not found** → You are in PowerShell/CMD. Switch to **Git Bash** or run inside WSL. The script needs bash, find, and gzip (provided by Git Bash install).
- **`gzip` not found** during `npm run build` → Reinstall Git for Windows with default components, then reopen Git Bash so `gzip` is on PATH.
- **`pio: command not found`** → Open the PlatformIO terminal from VS Code after the extension finishes installing, or restart VS Code.
- **"Windows Subsystem for Linux has no installed distributions"** → Don't run `bash ./build.sh` in PowerShell. Instead, change the PlatformIO terminal default profile to Git Bash (see section 6).
- **`npm: command not found` in PlatformIO terminal** → Restart your PC after installing Node.js. Or use the temporary PATH workaround in step 1.2.
- **No COM port** → Try another USB cable/port; if still missing, install the Espressif USB driver linked above.
- **Build hangs downloading libs** → First build fetches dependencies; allow a few minutes. Re-run `./build.sh --all` if interrupted.

## 10) Re-run After Making Changes
- Firmware-only change: `./build.sh -u -m`
- Web UI change only: `./build.sh -f -m` (skips full firmware rebuild)
- Clean build: `./build.sh --clean --all`

**Note:** The script auto-detects Windows—no need for `--env waveshare-349-windows`.

You now have a repeatable Windows workflow: Git Bash for scripts, PlatformIO CLI for flashing, and Node 18+ for the web UI.

## 11) Factory Reset (Full Flash Erase)

Settings are stored in NVS (non-volatile storage) and persist across firmware updates. The `pio run -t erase` target only erases firmware, not NVS settings.

**To fully reset all settings to defaults:**
```bash
# Use PlatformIO's Python (has all dependencies):
"$HOME/.platformio/penv/Scripts/python.exe" "$HOME/.platformio/packages/tool-esptoolpy/esptool.py" --port COM4 erase_flash

# Then re-upload firmware and filesystem (from a PlatformIO terminal):
export PATH="$PATH:$HOME/.platformio/penv/Scripts"
pio run -e waveshare-349-windows -t upload
pio run -e waveshare-349-windows -t uploadfs
```

**Note:** Replace `COM4` with your actual port (use `pio device list` to find it). After erase, the device boots with factory defaults (WiFi: V1-Simple/setupv1g2).

## 12) Why Windows Uses a Different Environment
Windows PlatformIO doesn't fully support the newer Arduino ESP32 3.x framework required by GFX Library 1.6.4. The `waveshare-349-windows` environment uses:
- **GFX Library 1.4.9** (older, stable, ESP32 2.x compatible)
- **8MB flash partition table** (auto-detected by esptool)
- Conditional compilation via `WINDOWS_BUILD=1` flag

**Flash size note:** The Waveshare ESP32-S3-Touch-LCD-3.49 is advertised as 16MB flash, but esptool detects some units as 8MB. The Windows build uses 8MB partitions to ensure compatibility with all units.

The source code includes `#ifdef WINDOWS_BUILD` blocks to handle API differences between ESP32 Arduino 2.x (Windows) and 3.x (Mac/Linux). Both environments produce functionally identical firmware.
