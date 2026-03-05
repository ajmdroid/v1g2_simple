#!/bin/bash
set -euo pipefail

# Generate camera voice clips using macOS `say` and ffmpeg.
# Output clips are canonical under tools/freq_audio/mulaw.

VOICE="Samantha"
RATE=210
OUT_DIR="tools/freq_audio"
MULAW_DIR="$OUT_DIR/mulaw"

if [ ! -d "tools/freq_audio" ]; then
    # Support running from tools/ cwd.
    OUT_DIR="freq_audio"
    MULAW_DIR="$OUT_DIR/mulaw"
fi

mkdir -p "$OUT_DIR" "$MULAW_DIR"

build_clip() {
    local name="$1"
    local text="$2"
    local aiff="$OUT_DIR/${name}.aiff"
    local raw="$OUT_DIR/${name}.raw"
    local mul="$MULAW_DIR/${name}.mul"

    echo "Generating $name.mul -> \"$text\""
    say -v "$VOICE" -r "$RATE" -o "$aiff" "$text"
    ffmpeg -y -i "$aiff" -ar 22050 -ac 1 -f s16le -acodec pcm_s16le "$raw" >/dev/null 2>&1
    ffmpeg -y -f s16le -ar 22050 -ac 1 -i "$raw" -f mulaw -ar 22050 "$mul" >/dev/null 2>&1
    rm -f "$aiff"
}

build_clip "cam_speed" "speed camera"
build_clip "cam_red_light" "red light"
build_clip "cam_bus_lane" "bus lane"
build_clip "cam_alpr" "A L P R"
build_clip "cam_close" "close"

for target in "data/audio" "../data/audio"; do
    if [ -d "$target" ]; then
        cp "$MULAW_DIR"/cam_speed.mul "$target"/
        cp "$MULAW_DIR"/cam_red_light.mul "$target"/
        cp "$MULAW_DIR"/cam_bus_lane.mul "$target"/
        cp "$MULAW_DIR"/cam_alpr.mul "$target"/
        cp "$MULAW_DIR"/cam_close.mul "$target"/
        echo "Copied camera clips to $target"
    fi
done

echo "Done. Camera clips:"
ls -la "$MULAW_DIR"/cam_*.mul
