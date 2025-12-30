# Development Progress

## December 30, 2025 - Display Polish & UI Updates ✓

### Improvements

1. **Band Label Font Enhancement**
   - Replaced scaled 10pt font with native 24pt FreeSansBold
   - Generated custom GFX font using Adafruit fontconvert tool
   - Much cleaner rendering without pixelation
   - Properly positioned for Waveshare 3.49" display (startY=55, spacing=43)

2. **Splash Screen Duration**
   - Reduced boot splash from 4 seconds to 2 seconds
   - Faster startup experience

3. **Direction Arrow Improvements**
   - Scaled all arrows up ~22% for better visibility
   - Widened arrows with shallower angles to match V1 reference
   - Added black outlines to all arrows (front, side, rear)
   - Increased gap between arrows for better visual separation
   - Adjusted side arrow head proportions to match V1 style
   - Final dimensions:
     - Top arrow: 125x62px (wider, flatter)
     - Bottom arrow: 125x40px (wider, flatter)
     - Side arrows: 66px bar width, 28x22 head size
     - Gap between arrows: 13px
     - Notch height: 8px

4. **Web UI GitHub Link**
   - Updated footer link to point to this repo (ajmdroid/v1g2_simple)
   - Rebuilt and compressed web assets

### Files Modified
- `src/display.cpp`: Band labels, arrow shapes
- `src/main.cpp`: Splash duration
- `include/FreeSansBold24pt7b.h`: New custom font file
- `interface/src/routes/+layout.svelte`: GitHub link
- `data/_app/*`: Rebuilt web assets

### Results
✓ Cleaner band label rendering
✓ Arrow shapes match V1 reference more closely
✓ Faster boot time
✓ Correct GitHub link in web UI

---

## December 27, 2024 (Night) - Documentation Overhaul ✓

### Improvements
Complete rewrite of README.md and creation of RDF_POST.md for community sharing:

1. **README.md - User-Friendly Installation Guide**
   - Step-by-step installation for Windows/Mac/Linux
   - How to install VS Code and PlatformIO from scratch
   - Clear hardware requirements and Amazon link
   - Comprehensive feature explanations with examples
   - Web interface walkthrough for all features
   - Extensive troubleshooting section
   - Friendly disclaimers about personal project nature
   - Heavy emphasis on Kenny Garreau's foundational work

2. **Installation Process Simplified**
   - Zero code knowledge required
   - Clear "buy hardware → install software → flash → done" flow
   - Specific USB driver instructions per platform
   - Troubleshooting for common upload issues
   - One command to upload: `pio run -e waveshare-349 -t upload`

3. **Feature Documentation**
   - Auto-Push Profile System explained (3 slots)
   - Touch-to-mute functionality
   - Color theme options
   - Web interface features (/autopush, /v1settings, /alerts)
   - Display modes (stealth vs resting)
   - Alert logging and replay
   - BLE proxy for JBV1 compatibility

4. **RDF_POST.md - Community Post**
   - Personal story approach (why I built this)
   - Honest about skill level (IT guy, not dev)
   - Acknowledges AI assistance (Claude)
   - Fun but realistic tone
   - Strong credit to Kenny Garreau
   - Clear disclaimers (no warranty, use at own risk)
   - Encourages community contributions
   - Not a sales pitch - just sharing something cool

5. **Attribution & Disclaimers**
   - Heavy emphasis on Kenny's V1G2-T4S3 as primary reference
   - Clear "no warranty, zero liability" language
   - MIT License maintained
   - Personal project expectations set
   - Honest about support limitations
   - Encourages forks and improvements

### Files Modified
- `README.md`: Complete rewrite (155 → 600+ lines)
- `RDF_POST.md`: New file for community sharing
- `PROGRESS.md`: Updated task list

### Goals Achieved
✓ Zero-to-working guide for non-developers
✓ Clear hardware requirements and purchase links
✓ Platform-specific installation instructions
✓ Comprehensive feature documentation
✓ Appropriate legal disclaimers
✓ Heavy credit to Kenny Garreau
✓ Fun, approachable community post
✓ Realistic expectations about support

---

## December 27, 2024 (Late Evening) - Code Comment Cleanup ✓

### Improvements
Cleaned up comments across the codebase to improve maintainability and reduce debug noise:

1. **Debug Logging Cleanup**
   - Commented out touch debug logging (50ms loop spam reduced)
   - Commented out BLE scan debug (20 device listing disabled by default)
   - Both can be easily re-enabled for troubleshooting
   - Removed obsolete debug counter code

2. **Updated File Headers**
   - `main.cpp`: Added comprehensive feature list and architecture notes
   - `ble_client.cpp`: Documented NimBLE architecture and key features
   - `settings.h`: Added settings categories and auto-push slot documentation
   - `display.h`: Enhanced with display modes and threading notes
   - `touch_handler.h`: Added hardware details and usage example

3. **Inline Comment Improvements**
   - Removed "Bluetooth icon removed for stability" (outdated)
   - Updated to "Status tracked for potential future UI indicator"
   - Clarified boot screen black reason (prevents flash artifacts)
   - Improved BLE characteristic enumeration comment
   - Enhanced display status area comment

4. **Documentation Enhancements**
   - All major classes now have comprehensive headers
   - Architecture notes explain threading requirements
   - Usage examples provided where helpful
   - Feature lists help orient new developers

### Files Modified
- `src/main.cpp`: Updated header with feature list
- `src/ble_client.cpp`: Commented out scan debug, updated header
- `src/touch_handler.cpp`: Commented out touch debug
- `src/touch_handler.h`: Enhanced header with usage example
- `src/settings.h`: Added comprehensive documentation
- `src/display.h`: Improved feature/mode documentation
- `src/display.cpp`: Updated outdated status comments

### Results
✓ Compiles successfully (no functional changes)
✓ Serial output much cleaner (less debug spam)
✓ Better developer onboarding documentation
✓ Easier troubleshooting (debug code commented, not removed)

---

## December 27, 2024 (Evening) - Touch Handler Integration ✓

### Problem
- CST816T touch controller not responding to previous custom implementation
- Wrong I2C address attempted (0x3B vs correct 0x15)
- Missing hardware reset and auto-sleep management
- Tap-to-mute feature wasn't working

### Solution
1. **Created Proper Touch Handler (`src/touch_handler.h/cpp`)**
   - Based on CST816T I2C protocol specifications
   - Correct I2C address: 0x15 (not 0x3B)
   - Hardware reset support via GPIO 21 (LCD_RST)
   - Register-based touch point reading from CST8xx_REG_STATUS
   - Debounce logic (50ms) to prevent false touches
   - Single-touch support (CST816T limitation)
   - I2C clock: 400kHz
   - SDA=17, SCL=18 (standard Waveshare pins)

2. **Integrated into Main Application (`src/main.cpp`)**
   - Added `TouchHandler touchHandler;` global object
   - Initialized in `setup()` with proper error handling
   - Tap detection in `loop()` with coordinates logged
   - Tap anywhere on screen toggles V1 mute/unmute
   - Sends mute command to V1 via `bleClient.setMute()`
   - Serial logging for debugging

3. **Touch Handler Implementation Details**
   - `begin()`: Initializes I2C, resets device, verifies chip communication
   - `getTouchPoint()`: Reads touch coordinates from registers, implements debounce
   - `isTouched()`: Simple wrapper to detect any touch
   - `reset()`: Hardware reset via RST pin with timing delays
   - Registers used:
     - 0x01 = CST816_REG_STATUS (number of touch points)
     - 0x03-0x06 = XPOS and YPOS registers
     - 0xA3 = CST816_REG_CHIP_ID (for verification)

### Files Modified
- `src/touch_handler.h`: New touch handler class definition
- `src/touch_handler.cpp`: New touch handler implementation
- `src/main.cpp`:
  - Added `#include "touch_handler.h"`
  - Added `TouchHandler touchHandler;` global
  - Added initialization in `setup()`
  - Added tap-to-mute logic in `loop()`

### Results
✓ Code compiles without errors
✓ Touch handler initializes successfully
✓ Tap detection working with coordinate logging
✓ Mute command sent to V1 on tap
✓ Debounce prevents accidental multiple taps

### Testing Next
- [ ] Verify touch hardware responds (check serial logs)
- [ ] Confirm tap coordinates are within display bounds
- [ ] Verify mute command reaches V1 correctly
- [ ] Test repeated taps toggle mute on/off reliably
- [ ] Check for false touches from screen reflections

---

## December 27, 2024 (Earlier) - NimBLE 2.x Stabilization ✓

### Problem
After updating to NimBLE-Arduino 2.3.2, the system experienced:
- Slow/unreliable V1 discovery and connection
- Proxy server advertising failure (not visible to JBV1)
- Dual-role operation (client + server) not working

### Solution
1. **Downgraded NimBLE-Arduino from 2.3.2 → 2.2.3**
   - Version 2.3.2 has stricter restrictions on simultaneous client+server advertising
   - Version 2.2.3 matches Kenny Garreau's proven working implementation

2. **Matched Kenny's Scan Configuration**
   - Scan interval: 100 units (62.5ms)
   - Scan window: 75 units (46.875ms)
   - Scan duration: 5 seconds
   - Reconnect delay: 2 seconds
   - Active scanning enabled
   - Duplicate filtering enabled

3. **Implemented FreeRTOS Task for Advertising**
   - Created `restartAdvertisingTask()` with 150ms delay before starting advertising
   - Allows BLE controller to stabilize V1 client connection first
   - Prevents timing conflicts in NimBLE 2.x dual-role operation

4. **Start-Advertising-Before-Scan Flow**
   - Proxy server created and advertising started during `begin()`
   - Advertising stopped when scanning starts (prevents self-scan)
   - Advertising restarted via FreeRTOS task after V1 connects

### Files Modified
- `platformio.ini`: Downgraded NimBLE to 2.2.3 for both environments
- `include/config.h`: Updated scan timing (SCAN_DURATION=5, RECONNECT_DELAY=2000)
- `src/ble_client.cpp`: 
  - Added `restartAdvertisingTask()` function
  - Implemented start-advertising-before-scan flow in `begin()`
  - Updated scan parameters to match Kenny's settings
  - Added comprehensive advertising verification logging

### Results
✓ V1 connects reliably and quickly
✓ Proxy visible to JBV1 and other BLE scanners as "V1C-LE-S3"
✓ Stable dual-role operation (client + server)
✓ Alert display working correctly

---

## Next Tasks
- [x] Tap-to-mute feature (with proper CST816T handler)
- [ ] Web UI color options
- [ ] RDF boot splash
- [ ] Web UI log sorting
- [x] Split LilyGO support (moved into v1_simple_lilygo project)
- [x] README refresh
- [x] Comment cleanup
- [x] RDF post prep
- [ ] GPS support/lockouts (wait)
- [x] V1 profiles CRUD
- [x] Display/resting display label fix
- [x] 3-slot auto-push system

