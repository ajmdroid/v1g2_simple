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

def create_subset(input_font, output_ttf, chars):
    """Create a subset font with only the specified characters"""
    from fontTools import subset
    from fontTools.ttLib import TTFont
    
    print(f"Creating subset with characters: {chars}")
    
    # Load the font
    font = TTFont(str(input_font))
    
    # First, subset the font (while still in original format)
    options = subset.Options()
    options.flavor = None
    options.desubroutinize = True
    
    subsetter = subset.Subsetter(options)
    subsetter.populate(text=chars)
    subsetter.subset(font)
    
    # Now check if this is a CFF/OTF font and convert to TTF
    if 'CFF ' in font:
        print("Source font is CFF/OTF format - converting to TrueType...")
        try:
            from fontTools.pens.cu2quPen import Cu2QuPen
            from fontTools.pens.ttGlyphPen import TTGlyphPen
            from fontTools.ttLib import newTable
            from fontTools.ttLib.tables._g_l_y_f import Glyph
            
            glyphOrder = font.getGlyphOrder()
            glyphSet = font.getGlyphSet()
            
            # Create new glyf table
            glyf = newTable('glyf')
            glyf.glyphs = {}
            glyf.glyphOrder = glyphOrder
            
            for glyphName in glyphOrder:
                pen = TTGlyphPen(None)
                try:
                    # Draw glyph through Cu2Qu pen to convert cubic curves to quadratic
                    cu2quPen = Cu2QuPen(pen, max_err=1.0, reverse_direction=True)
                    glyphSet[glyphName].draw(cu2quPen)
                    glyf[glyphName] = pen.glyph()
                except Exception as e:
                    # Create empty glyph on error
                    print(f"  Warning: Failed to convert glyph '{glyphName}': {e}")
                    glyf[glyphName] = Glyph()
            
            font['glyf'] = glyf
            
            # Remove CFF table
            del font['CFF ']
            
            # Add loca table (required for TrueType, will be computed on save)
            font['loca'] = newTable('loca')
            
            # IMPORTANT: Change sfntVersion from 'OTTO' to TrueType
            font.sfntVersion = '\x00\x01\x00\x00'
            
            print("Converted CFF outlines to TrueType quadratic curves")
        except Exception as e:
            print(f"WARNING: CFF->TTF conversion failed: {e}")
            print("Proceeding with OTF format - OpenFontRender may not render it correctly!")
    
    # Save the font
    font.save(str(output_ttf))
    print(f"Subset saved to {output_ttf}")
    
    # Verify output format
    with open(output_ttf, 'rb') as f:
        magic = f.read(4)
        if magic == b'OTTO':
            print("WARNING: Output is still CFF/OTF format!")
        else:
            print("Output is TrueType format - good!")
    
    # Print size comparison
    orig_size = os.path.getsize(input_font)
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
    
    # Check for command-line arguments
    chars_to_use = CHARS
    if len(sys.argv) > 1:
        chars_to_use = sys.argv[1]
        print(f"Using custom character set: {chars_to_use}")
    
    # Download the original font
    original_ttf = download_font()
    
    # Create subset
    subset_ttf = Path("/tmp/HemiHead-subset.ttf")
    create_subset(original_ttf, subset_ttf, chars_to_use)
    
    # Convert to header
    header_path = include_dir / "HemiHead.h"
    ttf_to_header(subset_ttf, header_path, "HemiHead")
    
    print("\nDone! Hemi Head font header created.")
    print("This retro speedometer-style font will be used for the third display style.")

if __name__ == "__main__":
    main()
