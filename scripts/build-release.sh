#!/bin/bash
# Build release binaries for web installer
# Usage: ./scripts/build-release.sh [version]

set -e

VERSION="${1:-$(git describe --tags --abbrev=0 2>/dev/null || echo 'dev')}"
RELEASE_DIR="docs/install"

echo "🔨 Building V1-Simple $VERSION for web installer..."

# Build web interface
echo "📦 Building web interface..."
cd interface
npm ci --silent 2>/dev/null || npm install --silent
npm run build
cd ..

# Build firmware
echo "🔧 Building firmware..."
pio run -e waveshare-349

# Build filesystem
echo "💾 Building filesystem..."
pio run -e waveshare-349 -t buildfs

# Copy binaries to release directory
echo "📁 Copying binaries to $RELEASE_DIR..."
mkdir -p "$RELEASE_DIR"
cp .pio/build/waveshare-349/bootloader.bin "$RELEASE_DIR/"
cp .pio/build/waveshare-349/partitions.bin "$RELEASE_DIR/"
cp .pio/build/waveshare-349/firmware.bin "$RELEASE_DIR/"
cp .pio/build/waveshare-349/littlefs.bin "$RELEASE_DIR/"

# Update manifest version
echo "📝 Updating manifest to version $VERSION..."
if [[ "$OSTYPE" == "darwin"* ]]; then
  sed -i '' "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" "$RELEASE_DIR/manifest.json"
else
  sed -i "s/\"version\": \".*\"/\"version\": \"$VERSION\"/" "$RELEASE_DIR/manifest.json"
fi

echo ""
echo "✅ Release binaries ready in $RELEASE_DIR/"
echo ""
echo "Files:"
ls -lh "$RELEASE_DIR"/*.bin "$RELEASE_DIR"/manifest.json 2>/dev/null
echo ""
echo "To test locally:"
echo "  cd $RELEASE_DIR && python3 -m http.server 8080"
echo "  Open https://localhost:8080 in Chrome (needs HTTPS for Web Serial)"
echo ""
echo "For local testing, use:"
echo "  npx serve $RELEASE_DIR"
