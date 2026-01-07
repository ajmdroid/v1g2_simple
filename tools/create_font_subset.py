#!/usr/bin/env python3
"""
Create a subset of Montserrat Bold font for embedded use.
Includes only the characters needed for the display: 0-9, -, ., L, A, S, E, R, C, N
"""

import subprocess
import sys
import os
from pathlib import Path

# Characters needed for the display
# - Digits: 0-9 for frequencies
# - Punctuation: - . for frequency display (e.g., "35.505" or "---.---")
# - Letters: L, A, S, E, R for "LASER" and C, N for "SCAN"
CHARS = "0123456789-.LASERCN"

def download_font():
    """Download Montserrat Bold from Google Fonts"""
    import urllib.request
    
    # Google Fonts CDN URL for Montserrat Bold
    url = "https://github.com/JulietaUla/Montserrat/raw/master/fonts/ttf/Montserrat-Bold.ttf"
    
    output_path = Path("/tmp/Montserrat-Bold.ttf")
    
    if output_path.exists():
        print(f"Font already downloaded: {output_path}")
        return output_path
    
    print(f"Downloading Montserrat Bold from {url}...")
    urllib.request.urlretrieve(url, output_path)
    print(f"Downloaded to {output_path}")
    return output_path

def create_subset(input_ttf, output_ttf, chars):
    """Create a subset font with only the specified characters"""
    from fontTools import subset
    
    print(f"Creating subset with characters: {chars}")
    
    # Build the subset options
    options = subset.Options()
    options.flavor = None  # Keep as TTF
    options.desubroutinize = True  # Helps with embedded use
    
    # Load the font
    font = subset.load_font(str(input_ttf), options)
    
    # Create the subset
    subsetter = subset.Subsetter(options)
    subsetter.populate(text=chars)
    subsetter.subset(font)
    
    # Save the subset
    subset.save_font(font, str(output_ttf), options)
    print(f"Subset saved to {output_ttf}")
    
    # Print size comparison
    orig_size = os.path.getsize(input_ttf)
    new_size = os.path.getsize(output_ttf)
    print(f"Original size: {orig_size:,} bytes")
    print(f"Subset size: {new_size:,} bytes ({100*new_size/orig_size:.1f}%)")

def ttf_to_header(ttf_path, header_path, array_name):
    """Convert TTF file to C header with byte array"""
    with open(ttf_path, 'rb') as f:
        data = f.read()
    
    lines = [
        "/**",
        " * Montserrat Bold Font (Subset) - Binary TTF data",
        " * Auto-generated - do not edit manually",
        " * ",
        " * License: SIL Open Font License 1.1",
        f" * Subset: {CHARS}",
        f" * Size: {len(data):,} bytes",
        " */",
        "",
        "#pragma once",
        "#include <cstdint>",
        "",
        f"const uint8_t {array_name}[] = {{",
    ]
    
    # Format bytes, 16 per line
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"    {hex_str},")
    
    lines.append("};")
    lines.append("")
    lines.append(f"const size_t {array_name}_size = sizeof({array_name});")
    lines.append("")
    
    with open(header_path, 'w') as f:
        f.write("\n".join(lines))
    
    print(f"Header written to {header_path}")

def main():
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    include_dir = project_root / "include"
    
    # Download the original font
    original_ttf = download_font()
    
    # Create subset
    subset_ttf = Path("/tmp/MontserratBold-subset.ttf")
    create_subset(original_ttf, subset_ttf, CHARS)
    
    # Convert to header
    header_path = include_dir / "MontserratBold.h"
    ttf_to_header(subset_ttf, header_path, "MontserratBold")
    
    print("\nDone! Font header updated with SCAN characters (C, N)")

if __name__ == "__main__":
    main()
