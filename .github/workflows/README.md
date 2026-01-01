# GitHub Actions Build Pipeline

## 🚀 How to Use

### Manual Build (One Button!)

1. Go to your repo on GitHub
2. Click **Actions** tab
3. Click **Build Firmware & Web Interface** workflow
4. Click **Run workflow** button (right side)
5. Select build type (release/debug)
6. Click green **Run workflow** button

### Download Build Artifacts

1. After workflow completes, click on the workflow run
2. Scroll to **Artifacts** section at bottom
3. Download `firmware-<commit>` zip file
4. Extract and flash to device

---

## 🔄 Auto-Triggers

The workflow automatically runs when:
- You push to `main` or `master` branch
- You create a pull request
- Changes are made to:
  - `src/**` - Source code
  - `include/**` - Headers
  - `interface/**` - Web UI
  - `platformio.ini` - Config

---

## 📦 Build Artifacts

Each build produces:
- `firmware.bin` - Main firmware (flash at 0x10000)
- `bootloader.bin` - ESP32-S3 bootloader (flash at 0x0000)
- `partitions.bin` - Partition table (flash at 0x8000)
- `littlefs.bin` - Filesystem with web interface (flash at 0x3D0000)
- `BUILD_INFO.txt` - Build metadata

Artifacts are kept for **30 days**.

---

## 🏷️ Creating Releases

To create a GitHub release:

```bash
# Tag your commit
git tag -a v1.0.0 -m "Release v1.0.0"
git push origin v1.0.0
```

This automatically:
1. Builds firmware
2. Creates GitHub release
3. Attaches all binaries
4. Generates release notes

---

## 🔧 Flash Downloaded Artifacts

### Using esptool.py:
```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 write_flash \
  0x0000 bootloader.bin \
  0x8000 partitions.bin \
  0x10000 firmware.bin \
  0x3D0000 littlefs.bin
```

### Using PlatformIO (easier):
```bash
# Just put firmware.bin in .pio/build/waveshare-349/
pio run -t upload
```

---

## 📊 Build Status Badge

Add this to your README.md:

```markdown
![Build Status](https://github.com/USERNAME/REPO/actions/workflows/build.yml/badge.svg)
```

Replace `USERNAME/REPO` with your GitHub username and repo name.

---

## 🐛 Troubleshooting

### Build fails on web interface
- Check `interface/package.json` has correct scripts
- Ensure `compress_web_assets.sh` is executable
- Verify Node.js version compatibility

### Build fails on firmware
- Check `platformio.ini` configuration
- Verify all dependencies in `lib_deps`
- Check for syntax errors in src/

### Artifacts not created
- Ensure workflow completed successfully (green checkmark)
- Check workflow logs for errors
- Verify artifact paths exist in build

---

## ⚡ Speed Optimizations

The workflow includes caching for:
- **Node modules** - Speeds up web builds
- **PlatformIO packages** - Speeds up firmware builds
- **Build artifacts** - Speeds up subsequent builds

First build: ~5-10 minutes  
Cached builds: ~2-3 minutes

---

## 🔒 Security

The workflow:
- ✅ Uses official GitHub Actions
- ✅ Pins action versions (@v4)
- ✅ No secrets required for basic builds
- ✅ Runs in isolated Ubuntu container
- ✅ Artifacts require GitHub login to download

---

## 📝 Customization

Edit `.github/workflows/build.yml` to:
- Change trigger conditions
- Modify build steps
- Add testing/linting
- Deploy to specific environments
- Send notifications (Slack, Discord, etc.)

---

## 🆚 vs Ansible

| Feature | GitHub Actions | Ansible |
|---------|---------------|---------|
| **Trigger** | Git events, manual button | Manual command |
| **Environment** | Cloud runners (free!) | Local/remote machines |
| **Artifacts** | Auto-hosted, downloadable | Manual storage |
| **UI** | GitHub web interface | CLI only |
| **Cost** | Free for public repos | Local compute |
| **Best for** | CI/CD, releases | Config management, deployment |

For building firmware: **GitHub Actions** wins! 🏆  
For deploying to multiple devices: **Ansible** might still be useful!

---

## 🎯 Quick Commands

```bash
# Trigger build from CLI (requires gh CLI)
gh workflow run build.yml

# List recent workflow runs
gh run list --workflow=build.yml

# Download latest artifacts
gh run download

# View workflow logs
gh run view --log
```

Install GitHub CLI: `brew install gh` (macOS) or see https://cli.github.com
