#!/usr/bin/env python3
"""
Generate a combined C header file with all alert voice samples.
"""

import os
import glob

def convert_raw_to_array(input_file, var_name):
    with open(input_file, "rb") as f:
        raw_data = f.read()
    
    num_samples = len(raw_data) // 2
    samples = []
    for i in range(0, len(raw_data), 2):
        sample = int.from_bytes(raw_data[i:i+2], byteorder='little', signed=True)
        samples.append(sample)
    
    duration_ms = (num_samples * 1000) // 22050
    return samples, num_samples, duration_ms

def main():
    output_file = "../include/alert_audio.h"
    
    # Define all alerts: (file_base, var_name)
    alerts = [
        ("laser_ahead", "laser_ahead"),
        ("laser_behind", "laser_behind"),
        ("laser_side", "laser_side"),
        ("ka_ahead", "ka_ahead"),
        ("ka_behind", "ka_behind"),
        ("ka_side", "ka_side"),
        ("k_ahead", "k_ahead"),
        ("k_behind", "k_behind"),
        ("k_side", "k_side"),
        ("x_ahead", "x_ahead"),
        ("x_behind", "x_behind"),
        ("x_side", "x_side"),
    ]
    
    with open(output_file, "w") as f:
        f.write("// Auto-generated alert voice samples\n")
        f.write("// 12 phrases: [Laser|Ka|K|X] [ahead|behind|side]\n")
        f.write("// 22050Hz mono 16-bit PCM\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")
        
        total_bytes = 0
        
        for file_base, var_name in alerts:
            raw_file = f"alert_audio/{file_base}.raw"
            if not os.path.exists(raw_file):
                print(f"Warning: {raw_file} not found, skipping")
                continue
                
            samples, num_samples, duration_ms = convert_raw_to_array(raw_file, var_name)
            total_bytes += num_samples * 2
            
            f.write(f"// {file_base}: {num_samples} samples, {duration_ms}ms\n")
            f.write(f"#define ALERT_{var_name.upper()}_SAMPLES {num_samples}\n")
            f.write(f"#define ALERT_{var_name.upper()}_DURATION_MS {duration_ms}\n")
            f.write(f"static const int16_t alert_{var_name}[{num_samples}] PROGMEM = {{\n")
            
            # Write samples, 12 per line
            for i in range(0, num_samples, 12):
                chunk = samples[i:i+12]
                line = ", ".join(f"{s:6d}" for s in chunk)
                if i + 12 < num_samples:
                    f.write(f"    {line},\n")
                else:
                    f.write(f"    {line}\n")
            
            f.write("};\n\n")
            print(f"{file_base}: {num_samples} samples, {duration_ms}ms")
        
        print(f"\nTotal: {total_bytes} bytes ({total_bytes/1024:.1f}KB)")
        print(f"Output: {output_file}")

if __name__ == "__main__":
    main()
