#!/usr/bin/env python3
"""
Convert raw PCM audio to C header for ESP32 playback.
Input: 22050Hz, 16-bit signed, mono, little-endian raw PCM
Output: C header with int16_t array

Usage:
  ffmpeg -i input.wav -ar 22050 -ac 1 -f s16le -acodec pcm_s16le output.raw
  python wav_to_header.py output.raw
"""

import sys
import os

def convert_raw_to_header(input_file, output_file=None, var_name="warning_volume_zero_pcm"):
    if output_file is None:
        base = os.path.splitext(input_file)[0]
        output_file = base + ".h"
    
    with open(input_file, "rb") as f:
        raw_data = f.read()
    
    # Convert to 16-bit samples
    num_samples = len(raw_data) // 2
    samples = []
    for i in range(0, len(raw_data), 2):
        sample = int.from_bytes(raw_data[i:i+2], byteorder='little', signed=True)
        samples.append(sample)
    
    # Calculate duration
    sample_rate = 22050
    duration_ms = (num_samples * 1000) // sample_rate
    
    print(f"Input: {input_file}")
    print(f"Samples: {num_samples}")
    print(f"Duration: {duration_ms}ms ({duration_ms/1000:.2f}s)")
    print(f"Size: {len(raw_data)} bytes")
    
    # Generate header
    with open(output_file, "w") as f:
        f.write(f"// Auto-generated from {os.path.basename(input_file)}\n")
        f.write(f"// {num_samples} samples @ 22050Hz = {duration_ms}ms\n")
        f.write(f"// Size: {len(raw_data)} bytes\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"#define {var_name.upper()}_SAMPLES {num_samples}\n")
        f.write(f"#define {var_name.upper()}_SAMPLE_RATE 22050\n")
        f.write(f"#define {var_name.upper()}_DURATION_MS {duration_ms}\n\n")
        f.write(f"static const int16_t {var_name}[{num_samples}] PROGMEM = {{\n")
        
        # Write samples, 12 per line
        for i in range(0, num_samples, 12):
            chunk = samples[i:i+12]
            line = ", ".join(f"{s:6d}" for s in chunk)
            if i + 12 < num_samples:
                f.write(f"    {line},\n")
            else:
                f.write(f"    {line}\n")
        
        f.write("};\n")
    
    print(f"Output: {output_file}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python wav_to_header.py <input.raw> [output.h] [var_name]")
        print("")
        print("First convert your WAV to raw PCM:")
        print("  ffmpeg -i input.wav -ar 22050 -ac 1 -f s16le -acodec pcm_s16le output.raw")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    var_name = sys.argv[3] if len(sys.argv) > 3 else "warning_volume_zero_pcm"
    
    convert_raw_to_header(input_file, output_file, var_name)
