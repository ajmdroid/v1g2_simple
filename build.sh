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

# Parse arguments
CLEAN=false
UPLOAD_FS=false
UPLOAD_FW=false
MONITOR=false
SKIP_WEB=false

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
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -c, --clean        Clean build (remove .pio/build/)"
            echo "  -f, --upload-fs    Upload filesystem after build"
            echo "  -u, --upload       Upload firmware after build"
            echo "  -m, --monitor      Open serial monitor after upload"
            echo "  -a, --all          Upload filesystem, firmware, and monitor"
            echo "  -s, --skip-web     Skip web interface build"
            echo "  -h, --help         Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                 # Build everything (no upload)"
            echo "  $0 --clean --all   # Clean build and upload everything"
            echo "  $0 -u -m           # Build firmware, upload, and monitor"
            echo "  $0 -f              # Build and upload filesystem only"
            echo "  $0 -s -u           # Skip web build, just build and upload firmware"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

echo -e "${BLUE}╔════════════════════════════════════════════════════╗${NC}"
echo -e "${BLUE}║         V1G2 Simple Complete Build Script         ║${NC}"
echo -e "${BLUE}╚════════════════════════════════════════════════════╝${NC}"
echo ""

# Step 1: Clean if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}🧹 Cleaning build artifacts...${NC}"
    pio run -t clean
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
    echo ""
else
    echo -e "${YELLOW}⏭️  Skipping web interface build${NC}"
    echo ""
fi

# Step 3: Build firmware
echo -e "${YELLOW}🔧 Building firmware...${NC}"
pio run

if [ $? -ne 0 ]; then
    echo -e "${RED}❌ Firmware build failed!${NC}"
    exit 1
fi

echo -e "${GREEN}✅ Firmware built successfully${NC}"

# Show build size
echo -e "${BLUE}📊 Build size:${NC}"
pio run -t size | grep -E "RAM:|Flash:" || true
echo ""

# Step 4: Upload filesystem if requested
if [ "$UPLOAD_FS" = true ]; then
    echo -e "${YELLOW}📤 Uploading filesystem (LittleFS)...${NC}"
    pio run -t uploadfs
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Filesystem upload failed!${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✅ Filesystem uploaded${NC}"
    echo ""
fi

# Step 5: Upload firmware if requested
if [ "$UPLOAD_FW" = true ]; then
    echo -e "${YELLOW}📤 Uploading firmware...${NC}"
    pio run -t upload
    
    if [ $? -ne 0 ]; then
        echo -e "${RED}❌ Firmware upload failed!${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}✅ Firmware uploaded${NC}"
    echo ""
fi

# Step 6: Monitor if requested
if [ "$MONITOR" = true ]; then
    echo -e "${GREEN}📡 Opening serial monitor...${NC}"
    echo -e "${BLUE}(Press Ctrl+C to exit)${NC}"
    echo ""
    sleep 1
    pio device monitor
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
