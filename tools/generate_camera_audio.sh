#!/bin/bash
# Convert camera alert raw PCM clips into mu-law .mul assets for LittleFS.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_DIR="$PROJECT_ROOT/tools/freq_audio"
OUTPUT_DIR="$SOURCE_DIR/mulaw"
DATA_AUDIO_DIR="$PROJECT_ROOT/data/audio"

CLIPS=(
  "cam_speed"
  "cam_red_light"
  "cam_bus_lane"
  "cam_alpr"
  "cam_close"
)

mkdir -p "$OUTPUT_DIR"
mkdir -p "$DATA_AUDIO_DIR"

echo "=== Generating Camera Audio Clips ==="
echo "Source: $SOURCE_DIR"
echo "Output: $OUTPUT_DIR"
echo ""

for clip in "${CLIPS[@]}"; do
    input="$SOURCE_DIR/${clip}.raw"
    output="$OUTPUT_DIR/${clip}.mul"

    if [ ! -f "$input" ]; then
        echo "Missing source clip: $input" >&2
        exit 1
    fi

    echo "Converting: ${clip}.raw -> ${clip}.mul"
    ffmpeg -y -f s16le -ar 22050 -ac 1 -i "$input" -f mulaw -ar 22050 "$output" >/dev/null 2>&1
    cp "$output" "$DATA_AUDIO_DIR/"
done

echo ""
echo "Generated camera clips:"
for clip in "${CLIPS[@]}"; do
    ls -lh "$OUTPUT_DIR/${clip}.mul" "$DATA_AUDIO_DIR/${clip}.mul"
done
