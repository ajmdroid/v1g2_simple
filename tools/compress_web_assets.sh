#!/bin/bash
# Compress all web assets with gzip after build

echo "Compressing web assets..."

# Run from interface directory
cd "$(dirname "$0")/../interface" || exit 1

BUILD_DIR="build"

# Find and compress all JS, CSS, HTML files
find "$BUILD_DIR" -type f \( -name "*.js" -o -name "*.css" -o -name "*.html" -o -name "*.json" \) | while read file; do
    echo "  Compressing: $file"
    gzip -9 -k "$file"  # -9 = best compression, -k = keep original
done

echo "✅ Web assets compressed!"
echo "📦 Original + .gz files are both in $BUILD_DIR"
