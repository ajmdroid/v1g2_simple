#!/usr/bin/env python3
# Generate TTS audio for camera alerts using macOS 'say' command
# Usage: python generate_camera_tts.py
# Output: .mul files ready for SD card /audio/ folder

import subprocess
import os

# Output directory (script location)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
OUTPUT_DIR = os.path.join(SCRIPT_DIR, "camera_audio")


def generate_tts(text: str, output_base: str, voice: str = "Samantha"):
    """Generate TTS audio using macOS say command, convert to mu-law."""
    aiff_file = os.path.join(OUTPUT_DIR, f"{output_base}.aiff")
    mul_file = os.path.join(OUTPUT_DIR, f"{output_base}.mul")
    
    # Generate AIFF with macOS say
    cmd_say = ["say", "-v", voice, "-o", aiff_file, text]
    result = subprocess.run(cmd_say, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  ❌ say failed: {result.stderr}")
        return False
    
    # Convert to mu-law with ffmpeg
    cmd_ffmpeg = [
        "ffmpeg", "-y", "-i", aiff_file,
        "-ar", "22050", "-ac", "1", "-f", "mulaw", mul_file
    ]
    result = subprocess.run(cmd_ffmpeg, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"  ❌ ffmpeg failed: {result.stderr}")
        return False
    
    # Clean up intermediate file
    os.remove(aiff_file)
    
    # Get file size
    size = os.path.getsize(mul_file)
    print(f"  ✅ {mul_file} ({size} bytes)")
    return True


def main():
    # Create output directory
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Camera alert phrases
    phrases = [
        ("Red light camera", "cam_redlight"),
        ("Speed camera", "cam_speed"),
        ("A. L. P. R.", "cam_alpr"),
        ("Red light and speed camera", "cam_both"),
    ]
    
    print("Generating camera TTS audio files...")
    print("=" * 50)
    
    for text, basename in phrases:
        print(f"\n{text} -> {basename}.mul")
        generate_tts(text, basename)
    
    print("\n" + "=" * 50)
    print(f"✅ Files ready in: {OUTPUT_DIR}/")
    print("\nCopy the .mul files to your SD card /audio/ folder")


if __name__ == "__main__":
    main()
