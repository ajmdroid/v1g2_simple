#!/bin/bash
# Convert camera alert raw PCM clips into mu-law .mul assets for LittleFS.
# Optional: regenerate selected raw masters with macOS Samantha TTS first.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SOURCE_DIR="$PROJECT_ROOT/tools/freq_audio"
OUTPUT_DIR="$SOURCE_DIR/mulaw"
DATA_AUDIO_DIR="$PROJECT_ROOT/data/audio"
VOICE="Samantha"
RATE=210
REGENERATE_RAW=0
SELECTED_CLIP=""

CLIPS=(
  "cam_speed"
  "cam_red_light"
  "cam_bus_lane"
  "cam_alpr"
)

clip_text() {
    case "$1" in
        cam_speed) echo "speed camera" ;;
        cam_red_light) echo "red light camera" ;;
        cam_bus_lane) echo "bus lane camera" ;;
        cam_alpr) echo "A. L. P. R." ;;
        *)
            echo "Unknown clip: $1" >&2
            return 1
            ;;
    esac
}

usage() {
    cat <<'EOF'
Usage: tools/generate_camera_audio.sh [--regenerate-raw] [--clip <clip-name>]

Options:
  --regenerate-raw   Rebuild raw masters with macOS Samantha TTS before mu-law conversion.
  --clip NAME        Restrict generation to a single clip (e.g. cam_alpr).
  -h, --help         Show help.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --regenerate-raw)
            REGENERATE_RAW=1
            ;;
        --clip)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --clip" >&2
                exit 2
            fi
            SELECTED_CLIP="$2"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

mkdir -p "$OUTPUT_DIR"
mkdir -p "$DATA_AUDIO_DIR"

echo "=== Generating Camera Audio Clips ==="
echo "Source: $SOURCE_DIR"
echo "Output: $OUTPUT_DIR"
if [[ "$REGENERATE_RAW" -eq 1 ]]; then
    echo "Raw regeneration: enabled (voice=$VOICE rate=$RATE)"
fi
echo ""

for clip in "${CLIPS[@]}"; do
    if [[ -n "$SELECTED_CLIP" && "$clip" != "$SELECTED_CLIP" ]]; then
        continue
    fi

    input="$SOURCE_DIR/${clip}.raw"
    output="$OUTPUT_DIR/${clip}.mul"

    if [[ "$REGENERATE_RAW" -eq 1 ]]; then
        if ! command -v say >/dev/null 2>&1; then
            echo "say command not found; raw regeneration requires macOS TTS" >&2
            exit 1
        fi
        text="$(clip_text "$clip")"
        aiff="$SOURCE_DIR/${clip}.aiff"
        echo "Regenerating raw: ${clip}.raw <- '$text'"
        say -v "$VOICE" -r "$RATE" -o "$aiff" "$text"
        ffmpeg -y -i "$aiff" -ar 22050 -ac 1 -f s16le -acodec pcm_s16le "$input" >/dev/null 2>&1
        rm -f "$aiff"
    elif [ ! -f "$input" ]; then
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
    if [[ -n "$SELECTED_CLIP" && "$clip" != "$SELECTED_CLIP" ]]; then
        continue
    fi
    ls -lh "$OUTPUT_DIR/${clip}.mul" "$DATA_AUDIO_DIR/${clip}.mul"
done
