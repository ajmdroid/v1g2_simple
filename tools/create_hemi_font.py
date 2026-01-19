#!/usr/bin/env python3
"""
Create a subset of Hemi Head font for embedded use.
Includes only the characters needed for the display: 0-9, -, ., L, A, S, E, R, C, N

Download from: https://www.dafont.com/hemi-head.font
Direct link: https://dl.dafont.com/dl/?f=hemi_head
"""

import subprocess
import sys
import os
import zipfile
from pathlib import Path
import urllib.request
import shutil

# Characters needed for the display
CHARS = "0123456789-.LASERCN"

def download_font():
    """Download Hemi Head font from dafont.com"""
    
    # DaFont download URL
    url = "https://dl.dafont.com/dl/?f=hemi_head"
    
    zip_path = Path("/tmp/hemi_head.zip")
    extract_dir = Path("/tmp/hemi_head")
    
    if extract_dir.exists():
        print(f"Font directory already exists: {extract_dir}")
    else:
        print(f"Downloading Hemi Head from {url}...")
        urllib.request.urlretrieve(url, zip_path)
        print(f"Downloaded to {zip_path}")
        
        # Extract the zip
        print("Extracting...")
        extract_dir.mkdir(exist_ok=True)
        with zipfile.ZipFile(zip_path, 'r') as zip_ref:
            zip_ref.extractall(extract_dir)
        print(f"Extracted to {extract_dir}")
    
    # Find the TTF/OTF file (usually named "hemi head bd it.otf" or similar)
    ttf_files = list(extract_dir.glob("*.ttf")) + list(extract_dir.glob("*.TTF")) + \
                list(extract_dir.glob("*.otf")) + list(extract_dir.glob("*.OTF"))
    
    if not ttf_files:
        print(f"ERROR: No TTF/OTF files found in {extract_dir}")
        sys.exit(1)
    
    # Use the first TTF file found (or pick bold/italic variant if multiple)
    ttf_path = ttf_files[0]
    for f in ttf_files:
        if 'bd' in f.name.lower() or 'bold' in f.name.lower():
            ttf_path = f
            break
    
    print(f"Using font: {ttf_path}")
    return ttf_path

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
        " * Hemi Head Font (Subset) - Binary TTF data",
        " * Auto-generated - do not edit manually",
        " * ",
        " * Font: Hemi Head by Typodermic Fonts",
        " * License: Free for personal and commercial use",
        " * URL: https://www.dafont.com/hemi-head.font",
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
    # Check for fontTools
    try:
        from fontTools import subset
    except ImportError:
        print("ERROR: fontTools not installed")
        print("Install with: pip install fonttools")
        sys.exit(1)
    
    script_dir = Path(__file__).parent
    project_root = script_dir.parent
    include_dir = project_root / "include"
    
    # Download the original font
    original_ttf = download_font()
    
    # Create subset
    subset_ttf = Path("/tmp/HemiHead-subset.ttf")
    create_subset(original_ttf, subset_ttf, CHARS)
    
    # Convert to header
    header_path = include_dir / "HemiHead.h"
    ttf_to_header(subset_ttf, header_path, "HemiHead")
    
    print("\nDone! Hemi Head font header created.")
    print("This retro speedometer-style font will be used for the third display style.")

if __name__ == "__main__":
    main()
