#!/bin/bash
# Local CI test - mimics GitHub Actions build workflow
# Run this before pushing to catch build failures early

set -e  # Exit on any error

echo "╔════════════════════════════════════════════════════╗"
echo "║           Local CI Build Test                      ║"
echo "╚════════════════════════════════════════════════════╝"
echo ""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

cd "$(dirname "$0")/.."
ROOT_DIR=$(pwd)

# Track timing
START_TIME=$(date +%s)

# Step 1: Build web interface
echo -e "${YELLOW}📦 Building web interface...${NC}"
cd interface
npm ci --silent 2>/dev/null || npm install --silent
npm run build
echo -e "${GREEN}✅ Web interface built${NC}"

# Step 2: Deploy to data folder
echo -e "${YELLOW}📁 Deploying to data/ folder...${NC}"
npm run deploy
echo -e "${GREEN}✅ Web files deployed${NC}"

cd "$ROOT_DIR"

# Step 3: Build Mac/Linux firmware
echo -e "${YELLOW}🏗️  Building firmware (waveshare-349)...${NC}"
pio run -e waveshare-349
echo -e "${GREEN}✅ Mac/Linux firmware built${NC}"

# Step 4: Build Windows firmware
echo -e "${YELLOW}🏗️  Building firmware (waveshare-349-windows)...${NC}"
pio run -e waveshare-349-windows
echo -e "${GREEN}✅ Windows firmware built${NC}"

# Step 5: Size report
echo ""
echo "╔════════════════════════════════════════════════════╗"
echo "║               Build Size Report                    ║"
echo "╚════════════════════════════════════════════════════╝"
echo ""
echo "Mac/Linux (waveshare-349):"
pio run -e waveshare-349 -t size 2>/dev/null | grep -E "(RAM|Flash|used|bytes)"
echo ""
echo "Windows (waveshare-349-windows):"
pio run -e waveshare-349-windows -t size 2>/dev/null | grep -E "(RAM|Flash|used|bytes)"

# Calculate elapsed time
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "╔════════════════════════════════════════════════════╗"
echo -e "║  ${GREEN}✅ All CI checks passed!${NC}                         ║"
echo "║  Elapsed time: ${ELAPSED}s                               ║"
echo "║  Safe to push to GitHub                            ║"
echo "╚════════════════════════════════════════════════════╝"
