#!/bin/bash
# Authoritative repo gate used locally and by GitHub workflows.
# Run this before pushing to catch the same failures CI enforces.

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

# Step 0: API contract guard
echo -e "${YELLOW}🔒 Checking WiFi API contracts...${NC}"
python3 scripts/check_wifi_api_contract.py
echo -e "${GREEN}✅ WiFi API contracts match${NC}"

# Step 0b: BLE hot-path contract guard
echo -e "${YELLOW}🔒 Checking BLE hot-path contract...${NC}"
python3 scripts/check_ble_hot_path_contract.py
echo -e "${GREEN}✅ BLE hot-path contract matches${NC}"

# Step 0b1: BLE deletion safety contract guard
echo -e "${YELLOW}🔒 Checking BLE deletion contract...${NC}"
python3 scripts/check_ble_deletion_contract.py
echo -e "${GREEN}✅ BLE deletion contract matches${NC}"

# Step 0c: Perf CSV column contract guard
echo -e "${YELLOW}🔒 Checking perf CSV column contract...${NC}"
python3 scripts/check_perf_csv_column_contract.py
echo -e "${GREEN}✅ Perf CSV column contract matches${NC}"

# Step 0c1: Perf SLO doc/json contract guard
echo -e "${YELLOW}🔒 Checking perf SLO contract...${NC}"
python3 scripts/check_perf_slo_contract.py
echo -e "${GREEN}✅ Perf SLO contract matches${NC}"

# Step 0d: Display flush discipline contract guard
echo -e "${YELLOW}🔒 Checking display flush discipline contract...${NC}"
python3 scripts/check_display_flush_discipline_contract.py
echo -e "${GREEN}✅ Display flush discipline contract matches${NC}"

# Step 0e: SD lock discipline contract guard
echo -e "${YELLOW}🔒 Checking SD lock discipline contract...${NC}"
python3 scripts/check_sd_lock_discipline_contract.py
echo -e "${GREEN}✅ SD lock discipline contract matches${NC}"

# Step 0f: Main loop call order contract guard
echo -e "${YELLOW}🔒 Checking main loop call-order contract...${NC}"
python3 scripts/check_main_loop_call_order_contract.py
echo -e "${GREEN}✅ Main loop call-order contract matches${NC}"

# Step 0g: Extern/global usage contract guard
echo -e "${YELLOW}🔒 Checking extern/global usage contract...${NC}"
python3 scripts/check_extern_usage_contract.py
echo -e "${GREEN}✅ Extern/global usage contract matches${NC}"

# Step 0h: Native unit tests
echo -e "${YELLOW}🧪 Running native unit tests...${NC}"
python3 scripts/run_native_tests_serial.py
echo -e "${GREEN}✅ Native unit tests passed${NC}"

# Step 0i: Frontend HTTP resilience contract guard
echo -e "${YELLOW}🔒 Checking frontend HTTP resilience contract...${NC}"
python3 scripts/check_frontend_http_resilience_contract.py
echo -e "${GREEN}✅ Frontend HTTP resilience contract matches${NC}"

# Step 1: Install frontend dependencies
echo -e "${YELLOW}📦 Installing frontend dependencies...${NC}"
cd interface
npm ci --silent 2>/dev/null || npm install --silent
echo -e "${GREEN}✅ Frontend dependencies installed${NC}"

# Step 1b: Frontend lint/type checks
echo -e "${YELLOW}🔎 Running frontend lint/type checks...${NC}"
npm run lint
echo -e "${GREEN}✅ Frontend lint/type checks passed${NC}"

# Step 1c: Frontend unit tests + coverage
echo -e "${YELLOW}🧪 Running frontend unit tests with coverage...${NC}"
npm run test:coverage
echo -e "${GREEN}✅ Frontend unit tests and coverage passed${NC}"

# Step 1d: Build web interface
echo -e "${YELLOW}📦 Building web interface...${NC}"
npm run build
echo -e "${GREEN}✅ Web interface built${NC}"

# Step 2: Deploy to data folder
echo -e "${YELLOW}📁 Deploying to data/ folder...${NC}"
npm run deploy
echo -e "${GREEN}✅ Web files deployed${NC}"

cd "$ROOT_DIR"

# Step 2b: Web packaging guardrails
echo -e "${YELLOW}🔒 Checking web asset guardrails...${NC}"
python3 scripts/check_web_asset_budget.py
echo -e "${GREEN}✅ Web asset guardrails pass${NC}"

# Step 2c: Audio manifest guardrail
echo -e "${YELLOW}🔒 Checking deployed audio manifest...${NC}"
python3 scripts/check_audio_asset_manifest.py
echo -e "${GREEN}✅ Deployed audio manifest matches${NC}"

# Step 3: Firmware static analysis
echo -e "${YELLOW}🔎 Running firmware static analysis...${NC}"
./scripts/pio-check.sh
echo -e "${GREEN}✅ Firmware static analysis passed${NC}"

# Step 4: Build firmware
echo -e "${YELLOW}🏗️  Building firmware (waveshare-349)...${NC}"
pio run -e waveshare-349 -j 1
echo -e "${GREEN}✅ Firmware built${NC}"

# Step 4a: Build filesystem image
echo -e "${YELLOW}💾 Building LittleFS image...${NC}"
pio run -e waveshare-349 -t buildfs -j 1
echo -e "${GREEN}✅ LittleFS image built${NC}"

# Step 4b: Flash package truth report
echo -e "${YELLOW}📏 Checking flash package truth...${NC}"
python3 scripts/report_flash_package_size.py \
  --max-firmware-bytes 5570560 \
  --expect-littlefs-bytes 2424832
echo -e "${GREEN}✅ Flash package truth report complete${NC}"

# Step 5: Size report
echo ""
echo "╔════════════════════════════════════════════════════╗"
echo "║               Build Size Report                    ║"
echo "╚════════════════════════════════════════════════════╝"
echo ""
echo "waveshare-349:"
pio run -e waveshare-349 -t size -j 1 2>/dev/null | grep -E "(RAM|Flash|used|bytes)"

# Calculate elapsed time
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "╔════════════════════════════════════════════════════╗"
echo -e "║  ${GREEN}✅ All CI checks passed!${NC}                         ║"
echo "║  Elapsed time: ${ELAPSED}s                               ║"
echo "║  Safe to push to GitHub                            ║"
echo "╚════════════════════════════════════════════════════╝"
