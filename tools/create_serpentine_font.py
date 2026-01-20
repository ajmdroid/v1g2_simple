#!/usr/bin/env python3
"""
Create a subset of Serpentine Bold Oblique font for embedded use.
Includes only the characters needed for the display: 0-9, -, ., L, A, S, E, R, C, N

Download from: https://fontsgeek.com/fonts/Serpentine-BoldOblique
The font needs to be downloaded manually due to website restrictions.
Place the .ttf file in /tmp/serpentine/ before running this script.
"""

import subprocess
import sys
import os
from pathlib import Path
import shutil

# Characters needed for the display
CHARS = "0123456789-.LASERCN"

def find_font():
    """Find Serpentine font file - user must download manually"""
    
    search_dirs = [
        Path("/tmp/serpentine"),
        Path("~/Downloads").expanduser(),
        Path("."),
    ]
    
    for search_dir in search_dirs:
        if not search_dir.exists():
            continue
        ttf_files = list(search_dir.glob("*[Ss]erpentine*.ttf")) + \
                    list(search_dir.glob("*[Ss]erpentine*.TTF")) + \
                    list(search_dir.glob("*[Ss]erpentine*.otf")) + \
                    list(search_dir.glob("*[Ss]erpentine*.OTF"))
        if ttf_files:
            # Prefer bold/oblique variants
            for f in ttf_files:
                name_lower = f.name.lower()
                if 'bold' in name_lower and 'oblique' in name_lower:
                    print(f"Found Serpentine Bold Oblique: {f}")
                    return f
                if 'boldoblique' in name_lower:
                    print(f"Found Serpentine BoldOblique: {f}")
                    return f
            # Fall back to first found
            print(f"Found Serpentine font: {ttf_files[0]}")
            return ttf_files[0]
    
    print("ERROR: Serpentine font not found!")
    print("\nPlease download Serpentine Bold Oblique from:")
    print("  https://fontsgeek.com/fonts/Serpentine-BoldOblique")
    print("\nThen place the .ttf file in /tmp/serpentine/ and run this script again.")
    print("\nOr place it in ~/Downloads/")
    sys.exit(1)

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
    verify_font = TTFont(str(output_ttf))
    if 'glyf' in verify_font:
        print("Output format: TrueType (glyf table present) ✓")
    elif 'CFF ' in verify_font:
        print("WARNING: Output format is still CFF/OTF!")
    
    return output_ttf

def create_header(ttf_path, output_path):
    """Convert TTF file to C header with byte array"""
    
    with open(ttf_path, 'rb') as f:
        data = f.read()
    
    print(f"Creating header file ({len(data)} bytes)...")
    
    with open(output_path, 'w') as f:
        f.write("// Serpentine Bold Oblique font subset (0-9, -, ., LASERCN)\n")
        f.write("// Auto-generated by create_serpentine_font.py\n")
        f.write(f"// Size: {len(data)} bytes\n\n")
        f.write("#pragma once\n\n")
        f.write("#include <stdint.h>\n\n")
        f.write(f"const uint8_t Serpentine[{len(data)}] PROGMEM = {{\n")
        
        # Write bytes in rows of 16
        for i in range(0, len(data), 16):
            row = data[i:i+16]
            hex_str = ', '.join(f'0x{b:02X}' for b in row)
            f.write(f"    {hex_str},\n")
        
        f.write("};\n")
    
    print(f"Header saved to {output_path}")

def main():
    # Check for fontTools
    try:
        import fontTools
    except ImportError:
        print("Installing fontTools...")
        subprocess.run([sys.executable, "-m", "pip", "install", "fonttools", "brotli"], check=True)
        import fontTools
    
    # Find the font file
    font_path = find_font()
    
    # Create subset
    subset_path = Path("/tmp/Serpentine-subset.ttf")
    create_subset(font_path, subset_path, CHARS)
    
    # Create header file
    script_dir = Path(__file__).parent.parent
    header_path = script_dir / "include" / "Serpentine.h"
    create_header(subset_path, header_path)
    
    print(f"\n✓ Done! Font header created at: {header_path}")
    print(f"  Font size: {subset_path.stat().st_size} bytes")
    print("\nNext steps:")
    print("  1. Add #include \"../include/Serpentine.h\" to display.cpp")
    print("  2. Add DISPLAY_STYLE_SERPENTINE = 3 to settings.h")
    print("  3. Add ofrSerpentine initialization to display.cpp")
    print("  4. Add drawFrequencySerpentine() function")
    print("  5. Update web UI dropdown")

if __name__ == "__main__":
    main()
