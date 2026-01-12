#!/bin/bash
# Generate frequency announcement audio clips using macOS Samantha voice
# Output: 22050Hz mono 16-bit PCM raw files for ESP32

VOICE="Samantha"
RATE=210  # Slightly faster speech rate
OUTPUT_DIR="freq_audio"

mkdir -p "$OUTPUT_DIR"

generate() {
    local filename="$1"
    local text="$2"
    
    echo "Generating: $filename -> '$text'"
    
    # Generate AIFF with say, convert to raw PCM with ffmpeg
    say -v "$VOICE" -r "$RATE" -o "$OUTPUT_DIR/${filename}.aiff" "$text"
    ffmpeg -y -i "$OUTPUT_DIR/${filename}.aiff" -ar 22050 -ac 1 -f s16le -acodec pcm_s16le "$OUTPUT_DIR/${filename}.raw" 2>/dev/null
    rm "$OUTPUT_DIR/${filename}.aiff"
}

echo "=== Generating Frequency Announcement Audio ==="
echo "Voice: $VOICE, Rate: $RATE"
echo ""

# Bands (spell out letters for clarity)
echo "--- Bands ---"
generate "band_ka" "K A"
generate "band_k" "K"
generate "band_x" "X"
generate "band_laser" "Laser"

# Directions
echo "--- Directions ---"
generate "dir_ahead" "ahead"
generate "dir_behind" "behind"
generate "dir_side" "side"

# GHz integers (spoken naturally)
echo "--- GHz ---"
generate "ghz_10" "ten"
generate "ghz_24" "twenty four"
generate "ghz_33" "thirty three"
generate "ghz_34" "thirty four"
generate "ghz_35" "thirty five"
generate "ghz_36" "thirty six"

# Hundreds digit (first digit of MHz like 7 in 749)
echo "--- Hundreds Digit ---"
generate "digit_0" "oh"
generate "digit_1" "one"
generate "digit_2" "two"
generate "digit_3" "three"
generate "digit_4" "four"
generate "digit_5" "five"
generate "digit_6" "six"
generate "digit_7" "seven"
generate "digit_8" "eight"
generate "digit_9" "nine"

# Two-digit endings (last 2 digits of MHz)
echo "--- Two-Digit Endings (00-09) ---"
generate "tens_00" "oh oh"
generate "tens_01" "oh one"
generate "tens_02" "oh two"
generate "tens_03" "oh three"
generate "tens_04" "oh four"
generate "tens_05" "oh five"
generate "tens_06" "oh six"
generate "tens_07" "oh seven"
generate "tens_08" "oh eight"
generate "tens_09" "oh nine"

echo "--- Two-Digit Endings (10-19) ---"
generate "tens_10" "ten"
generate "tens_11" "eleven"
generate "tens_12" "twelve"
generate "tens_13" "thirteen"
generate "tens_14" "fourteen"
generate "tens_15" "fifteen"
generate "tens_16" "sixteen"
generate "tens_17" "seventeen"
generate "tens_18" "eighteen"
generate "tens_19" "nineteen"

echo "--- Two-Digit Endings (20-90) ---"
generate "tens_20" "twenty"
generate "tens_30" "thirty"
generate "tens_40" "forty"
generate "tens_50" "fifty"
generate "tens_60" "sixty"
generate "tens_70" "seventy"
generate "tens_80" "eighty"
generate "tens_90" "ninety"

# Compound numbers (21-29, 31-39, etc.) - only commonly seen frequencies
echo "--- Common Compound Numbers ---"
# Ka band commonly sees frequencies like X49, X50, X70, etc.
for tens in 20 30 40 50 60 70 80 90; do
    tens_word=""
    case $tens in
        20) tens_word="twenty" ;;
        30) tens_word="thirty" ;;
        40) tens_word="forty" ;;
        50) tens_word="fifty" ;;
        60) tens_word="sixty" ;;
        70) tens_word="seventy" ;;
        80) tens_word="eighty" ;;
        90) tens_word="ninety" ;;
    esac
    
    for ones in 1 2 3 4 5 6 7 8 9; do
        num=$((tens + ones))
        ones_word=""
        case $ones in
            1) ones_word="one" ;;
            2) ones_word="two" ;;
            3) ones_word="three" ;;
            4) ones_word="four" ;;
            5) ones_word="five" ;;
            6) ones_word="six" ;;
            7) ones_word="seven" ;;
            8) ones_word="eight" ;;
            9) ones_word="nine" ;;
        esac
        generate "tens_${num}" "${tens_word} ${ones_word}"
    done
done

echo ""
echo "=== Done ==="
echo "Files generated in: $OUTPUT_DIR/"
ls -la "$OUTPUT_DIR"/*.raw | wc -l
echo "raw files created"
