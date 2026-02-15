#!/bin/bash
# Complete build script for V1G2 Simple
# Builds web interface, deploys to data/, builds firmware, and optionally uploads

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Detect Windows and set PIO command accordingly
# Check multiple indicators: OSTYPE, WINDIR, OS env var, or /c/ path exists
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$OSTYPE" == "win32" ]] || \
   [[ -n "$WINDIR" ]] || [[ "$OS" == "Windows_NT" ]] || [[ -d "/c/Windows" ]]; then
    IS_WINDOWS=true
    # Check if pio is in PATH first (e.g., pip install), then fall back to .platformio path
    if command -v pio &> /dev/null; then
        PIO_CMD="pio"
    elif [[ -f "$HOME/.platformio/penv/Scripts/pio.exe" ]]; then
        PIO_CMD="$HOME/.platformio/penv/Scripts/pio.exe"
    else
        echo -e "${RED}❌ PlatformIO not found! Please install it first.${NC}"
        echo "   Run: pip install platformio"
        exit 1
    fi
    DEFAULT_ENV="waveshare-349-windows"
    echo -e "${BLUE}🪟 Detected Windows - using waveshare-349-windows${NC}"
else
    IS_WINDOWS=false
    if command -v pio &> /dev/null; then
        PIO_CMD="pio"
    else
        echo -e "${RED}❌ PlatformIO not found in PATH.${NC}"
        echo "   Install PlatformIO CLI or use VS Code PlatformIO extension terminal."
        exit 1
    fi
    DEFAULT_ENV="waveshare-349"
fi

# Parse arguments
CLEAN=false
UPLOAD_FS=false
UPLOAD_FW=false
MONITOR=false
SKIP_WEB=false
RUN_TESTS=false
PIO_ENV="$DEFAULT_ENV"
UPLOAD_PORT=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean|-c)
            CLEAN=true
            shift
            ;;
        --upload-fs|-f)
            UPLOAD_FS=true
            shift
            ;;
        --upload|-u)
            UPLOAD_FW=true
            shift
            ;;
        --monitor|-m)
            MONITOR=true
            shift
            ;;
        --all|-a)
            UPLOAD_FS=true
            UPLOAD_FW=true
            MONITOR=true
            shift
            ;;
        --skip-web|-s)
            SKIP_WEB=true
            shift
            ;;
        --test|-t)
            RUN_TESTS=true
            shift
            ;;
        --env|-e)
            PIO_ENV="$2"
            shift 2
            ;;
        --upload-port)
            UPLOAD_PORT="$2"
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -c, --clean        Clean build (remove .pio/build/)"
            echo "  -f, --upload-fs    Upload filesystem after build (runs tests first)"
            echo "  -u, --upload       Upload firmware after build (runs tests first)"
            echo "  -m, --monitor      Open serial monitor after upload"
            echo "  -a, --all          Upload filesystem, firmware, and monitor"
            echo "  -s, --skip-web     Skip web interface build"
            echo "  -t, --test         Run unit tests before upload (native environment)"
            echo "  -e, --env ENV      PlatformIO environment (default: waveshare-349)"
            echo "                     Windows users: use --env waveshare-349-windows"
            echo "  --upload-port PORT COM port for upload (e.g., COM6)"
            echo "  -h, --help         Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                 # Build everything (no upload)"
            echo "  $0 --clean --all   # Clean build and upload everything"
            echo "  $0 -u -m           # Build firmware, upload, and monitor"
            echo "  $0 -f              # Build and upload filesystem only"
            echo "  $0 -s -u           # Skip web build, just build and upload firmware"
            echo "  $0 --all --test    # Build, test, upload everything"
            echo "  $0 --all --env waveshare-349-windows  # Windows build"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

# Build PIO arguments
PIO_ARGS="-e $PIO_ENV"
if [ -n "$UPLOAD_PORT" ]; then
    PIO_ARGS="$PIO_ARGS --upload-port $UPLOAD_PORT"
fi

echo -e "${BLUE}╔════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║         V1G2 Simple Complete Build Script         ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════╝${NC}"
echo ""

# Step 1: Clean if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}🧹 Cleaning build artifacts...${NC}"
    "$PIO_CMD" run $PIO_ARGS -t clean
    rm -rf interface/build interface/.svelte-kit
    echo -e "${GREEN}✅ Clean complete${NC}"
    echo ""
fi

# Step 2: Build web interface
if [ "$SKIP_WEB" = false ]; then
    echo -e "${YELLOW}🌐 Building web interface...${NC}"
    
    # Check if node_modules exists
    if [ ! -d "interface/node_modules" ]; then
        echo -e "${YELLOW}📦 Installing npm dependencies (first time)...${NC}"
        cd interface
        npm install
        cd ..
    fi
    
    cd interface
    npm run build
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Web build failed!${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✅ Web interface built${NC}"
    
    # Deploy to data/
    echo -e "${YELLOW}📁 Deploying to data/ folder...${NC}"
    npm run deploy
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Web deploy failed!${NC}"
        exit 1
    fi
    
    cd ..
    echo -e "${GREEN}✅ Web files deployed to data/${NC}"
    
    # Copy audio files (deploy script clears data/, so we restore audio)
    if [ -d "tools/freq_audio/mulaw" ] || [ -d "tools/camera_audio" ]; then
        echo -e "${YELLOW}🔊 Copying audio files to data/audio/...${NC}"
        mkdir -p data/audio

        FREQ_AUDIO_COPIED=0
        CAMERA_AUDIO_COPIED=0

        if compgen -G "tools/freq_audio/mulaw/*.mul" > /dev/null; then
            cp tools/freq_audio/mulaw/*.mul data/audio/
            FREQ_AUDIO_COPIED=$(ls -1 tools/freq_audio/mulaw/*.mul 2>/dev/null | wc -l | tr -d ' ')
        fi

        if compgen -G "tools/camera_audio/cam_*.mul" > /dev/null; then
            cp tools/camera_audio/cam_*.mul data/audio/
            CAMERA_AUDIO_COPIED=$(ls -1 tools/camera_audio/cam_*.mul 2>/dev/null | wc -l | tr -d ' ')
        fi

        AUDIO_COUNT=$(ls -1 data/audio/*.mul 2>/dev/null | wc -l | tr -d ' ')
        echo -e "${GREEN}✅ Copied $AUDIO_COUNT audio clips (freq=$FREQ_AUDIO_COPIED camera=$CAMERA_AUDIO_COPIED)${NC}"
    fi
    
    echo ""
else
    echo -e "${YELLOW}⏭️  Skipping web interface build${NC}"
    # Ensure data directory exists with at least one file for LittleFS build
    if [ ! -d "data" ] || [ -z "$(ls -A data 2>/dev/null)" ]; then
        echo -e "${YELLOW}   Creating minimal data/ directory for filesystem build...${NC}"
        mkdir -p data
        echo '{"placeholder":true}' > data/.placeholder.json
    fi
    echo ""
fi

# Step 3: Build firmware
echo -e "${YELLOW}🔧 Building firmware (env: $PIO_ENV)...${NC}"
"$PIO_CMD" run $PIO_ARGS

if [ $? -ne 0 ]; then
    echo -e "${RED}❌ Firmware build failed!${NC}"
    exit 1
fi

echo -e "${GREEN}✅ Firmware built successfully${NC}"

# Show build size
echo -e "${BLUE}📊 Build size:${NC}"
"$PIO_CMD" run $PIO_ARGS -t size | grep -E "RAM:|Flash:" || true
echo ""

# Step 4: Run tests if requested (requires gcc/g++ on host)
if [ "$RUN_TESTS" = true ]; then
    echo -e "${YELLOW}🧪 Running unit tests...${NC}"
    "$PIO_CMD" test -e native
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Tests failed!${NC}"
        echo -e "${YELLOW}💡 Tip: Install gcc/g++ if tests fail to compile${NC}"
        if [ "$UPLOAD_FS" = true ] || [ "$UPLOAD_FW" = true ]; then
            echo -e "${RED}   Aborting upload due to test failure.${NC}"
            exit 1
        fi
    else
        echo -e "${GREEN}✅ All tests passed${NC}"
    fi
    
    # Also check firmware compilation (catches platform-specific issues)
    echo -e "${YELLOW}🔍 Checking firmware compilation...${NC}"
    "$PIO_CMD" run $PIO_ARGS --target buildprog
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Firmware compilation check failed!${NC}"
        if [ "$UPLOAD_FS" = true ] || [ "$UPLOAD_FW" = true ]; then
            echo -e "${RED}   Aborting upload due to compilation failure.${NC}"
            exit 1
        fi
    else
        echo -e "${GREEN}✅ Firmware compiles without errors${NC}"
    fi
    echo ""
fi

# Step 5: Upload filesystem if requested
if [ "$UPLOAD_FS" = true ]; then
    echo -e "${YELLOW}⚠️  uploadfs overwrites internal LittleFS data${NC}"
    echo -e "${YELLOW}   If profile storage ever fell back to LittleFS, those profiles will be erased.${NC}"
    echo -e "${YELLOW}   Confirm SD is mounted in boot logs before relying on profile persistence.${NC}"
    echo -e "${YELLOW}📤 Uploading filesystem (LittleFS)...${NC}"
    "$PIO_CMD" run $PIO_ARGS -t uploadfs
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Filesystem upload failed!${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✅ Filesystem uploaded${NC}"
    echo ""
fi

# Step 6: Upload firmware if requested
if [ "$UPLOAD_FW" = true ]; then
    echo -e "${YELLOW}📤 Uploading firmware...${NC}"
    "$PIO_CMD" run $PIO_ARGS -t upload
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Firmware upload failed!${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✅ Firmware uploaded${NC}"
    
    # Extra reset after upload to ensure clean BLE state
    echo -e "${YELLOW}🔄 Resetting device for clean start...${NC}"
    sleep 1
    # Use esptool to hard reset via RTS
    if [ -n "$UPLOAD_PORT" ]; then
        python3 -c "import serial; s=serial.Serial('$UPLOAD_PORT', 115200); s.setRTS(True); s.setRTS(False); s.close()" 2>/dev/null || true
    else
        # Auto-detect port like PlatformIO does
        PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
        if [ -n "$PORT" ]; then
            python3 -c "import serial; s=serial.Serial('$PORT', 115200); s.setRTS(True); s.setRTS(False); s.close()" 2>/dev/null || true
        fi
    fi
    echo -e "${GREEN}✅ Device reset${NC}"
    echo ""
fi

# Step 7: Monitor if requested
if [ "$MONITOR" = true ]; then
    echo -e "${GREEN}📡 Opening serial monitor...${NC}"
    echo -e "${BLUE}(Press Ctrl+C to exit)${NC}"
    echo ""
    sleep 1
    # Remap upload port to monitor port and strip upload-only flags
    MONITOR_ARGS="$PIO_ARGS"
    if [[ "$MONITOR_ARGS" =~ --upload-port[[:space:]]+([^[:space:]]+) ]]; then
        PORT_VAL="${BASH_REMATCH[1]}"
        MONITOR_ARGS="${MONITOR_ARGS/--upload-port $PORT_VAL/}"
        MONITOR_ARGS="$(echo "$MONITOR_ARGS" | xargs)"
        MONITOR_ARGS="$MONITOR_ARGS --port $PORT_VAL"
    fi
    "$PIO_CMD" device monitor $MONITOR_ARGS
fi

# Final summary
echo ""
echo -e "${GREEN}╔════════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║                  Build Complete! 🎉                ║${NC}"
echo -e "${GREEN}╚════════════════════════════════════════════════════╝${NC}"
echo ""

if [ "$UPLOAD_FW" = false ] && [ "$UPLOAD_FS" = false ]; then
    echo -e "${YELLOW}💡 Tip: Use --all to build and upload everything${NC}"
    echo -e "${YELLOW}   Example: ./build.sh --all${NC}"
fi
