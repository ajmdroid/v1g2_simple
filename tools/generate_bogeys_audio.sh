#!/bin/bash
# Generate "bogeys" audio clip using macOS Samantha voice
# Output: mu-law compressed .mul file for ESP32

VOICE="Samantha"
RATE=210  # Slightly faster speech rate (matches other clips)
OUTPUT_DIR="freq_audio"

# Create output directories
mkdir -p "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR/mulaw"

echo "=== Generating 'bogeys' Audio Clip ==="
echo "Voice: $VOICE, Rate: $RATE"
echo ""

# Generate the audio
echo "Generating: bogeys.mul -> 'bogeys'"

# Generate AIFF with say, convert to raw PCM with ffmpeg
say -v "$VOICE" -r "$RATE" -o "$OUTPUT_DIR/bogeys.aiff" "bogeys"
ffmpeg -y -i "$OUTPUT_DIR/bogeys.aiff" -ar 22050 -ac 1 -f s16le -acodec pcm_s16le "$OUTPUT_DIR/bogeys.raw" 2>/dev/null
rm "$OUTPUT_DIR/bogeys.aiff"

# Convert raw PCM to mu-law using ffmpeg (same as other audio files)
ffmpeg -y -f s16le -ar 22050 -ac 1 -i "$OUTPUT_DIR/bogeys.raw" -f mulaw -ar 22050 "$OUTPUT_DIR/mulaw/bogeys.mul" 2>/dev/null

echo "Converted to mu-law: $OUTPUT_DIR/mulaw/bogeys.mul"

# Copy to data/audio if it exists
if [ -d "data/audio" ]; then
    cp "$OUTPUT_DIR/mulaw/bogeys.mul" "data/audio/"
    echo "Copied to data/audio/bogeys.mul"
fi

# Copy to ../data/audio if running from tools/
if [ -d "../data/audio" ]; then
    cp "$OUTPUT_DIR/mulaw/bogeys.mul" "../data/audio/"
    echo "Copied to ../data/audio/bogeys.mul"
fi

echo ""
echo "=== Done ==="
ls -la "$OUTPUT_DIR/mulaw/bogeys.mul"
