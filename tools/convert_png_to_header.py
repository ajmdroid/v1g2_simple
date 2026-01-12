#!/usr/bin/env python3
"""
Convert PNG image to C header file with RGB565 pixel data.

Usage:
    python3 convert_png_to_header.py input.png output.h LOGO_NAME [--target-width W --target-height H --background #000000]

Options:
    --target-width / --target-height: Resize to the given size (preserve aspect ratio)
    --mode: letterbox (default), fit-width-crop, fit-height-crop
    --background: Background color for letterboxing (hex, default black)
"""

import argparse
import os
import sys
from PIL import Image


def rgb888_to_rgb565(r, g, b):
    """Convert 24-bit RGB to 16-bit RGB565."""
    r5 = (r >> 3) & 0x1F
    g6 = (g >> 2) & 0x3F
    b5 = (b >> 3) & 0x1F
    return (r5 << 11) | (g6 << 5) | b5


def parse_hex_color(value):
    """Parse hex color strings like '#RRGGBB' or 'RRGGBB' to (r,g,b)."""
    value = value.lstrip("#")
    if len(value) != 6:
        raise argparse.ArgumentTypeError("Background must be in #RRGGBB format")
    r = int(value[0:2], 16)
    g = int(value[2:4], 16)
    b = int(value[4:6], 16)
    return (r, g, b)


def prepare_image(input_png, target_width=None, target_height=None, background=(0, 0, 0), mode="letterbox", trim=False):
    """Load image, optionally trim transparent borders, then resize."""
    img = Image.open(input_png).convert("RGBA")

    if trim:
        alpha = img.split()[-1]
        bbox = alpha.getbbox()
        if bbox:
            img = img.crop(bbox)

    width, height = img.size

    # If no target size given, return original
    if not target_width or not target_height:
        return img, width, height

    if mode == "fit-width-crop":
        # Scale to target width, preserve aspect, then center-crop height
        scale = target_width / width
        new_size = (target_width, int(height * scale))
        resized = img.resize(new_size, Image.LANCZOS)
        top = max(0, (resized.height - target_height) // 2)
        bottom = top + target_height
        cropped = resized.crop((0, top, target_width, bottom))
        return cropped, target_width, target_height

    if mode == "fit-width-crop-top":
        # Scale to target width, preserve aspect, crop from top (keep top visible)
        scale = target_width / width
        new_size = (target_width, int(height * scale))
        resized = img.resize(new_size, Image.LANCZOS)
        top = 0  # Start from top
        bottom = target_height
        cropped = resized.crop((0, top, target_width, bottom))
        return cropped, target_width, target_height

    if mode == "fit-width-crop-bottom":
        # Scale to target width, preserve aspect, crop from bottom (keep bottom visible)
        scale = target_width / width
        new_size = (target_width, int(height * scale))
        resized = img.resize(new_size, Image.LANCZOS)
        top = max(0, resized.height - target_height)
        bottom = resized.height
        cropped = resized.crop((0, top, target_width, bottom))
        return cropped, target_width, target_height

    if mode == "fit-height-crop":
        # Scale to target height, preserve aspect, then center-crop width
        scale = target_height / height
        new_size = (int(width * scale), target_height)
        resized = img.resize(new_size, Image.LANCZOS)
        left = max(0, (resized.width - target_width) // 2)
        right = left + target_width
        cropped = resized.crop((left, 0, right, target_height))
        return cropped, target_width, target_height

    # Default: letterbox
    scale = min(target_width / width, target_height / height)
    new_size = (int(width * scale), int(height * scale))
    resized = img.resize(new_size, Image.LANCZOS)

    canvas = Image.new("RGBA", (target_width, target_height), background + (255,))
    offset_x = (target_width - new_size[0]) // 2
    offset_y = (target_height - new_size[1]) // 2
    canvas.paste(resized, (offset_x, offset_y), resized)
    return canvas, target_width, target_height


def convert_png_to_header(input_png, output_header, logo_name, target_width=None, target_height=None, background=(0, 0, 0), mode="letterbox", trim=False):
    """Convert PNG image to C header file with RGB565 data."""

    img, width, height = prepare_image(input_png, target_width, target_height, background, mode, trim)
    pixels = img.load()

    print(f"Converting {input_png} ({img.width}x{img.height}) to {output_header}")

    header_guard = logo_name.upper() + "_H"

    with open(output_header, "w") as f:
        f.write(f"#ifndef {header_guard}\n")
        f.write(f"#define {header_guard}\n\n")
        f.write("#include <stdint.h>\n")
        f.write("#include <pgmspace.h>\n\n")
        f.write(f"// Auto-generated from {os.path.basename(input_png)}\n")
        f.write(f"// Image size: {width}x{height}\n\n")

        f.write(f"const uint16_t {logo_name.upper()}_WIDTH = {width};\n")
        f.write(f"const uint16_t {logo_name.upper()}_HEIGHT = {height};\n\n")

        f.write(f"const uint16_t {logo_name.lower()}_rgb565[] PROGMEM = {{\n")

        pixel_count = 0
        for y in range(height):
            f.write("    ")
            for x in range(width):
                r, g, b, a = pixels[x, y]

                if a < 128:
                    rgb565 = 0x0000
                else:
                    rgb565 = rgb888_to_rgb565(r, g, b)

                f.write(f"0x{rgb565:04X}")

                pixel_count += 1
                if pixel_count < width * height:
                    f.write(", ")
                    if pixel_count % 12 == 0:
                        f.write("\n    ")

            if y < height - 1:
                f.write("\n")

        f.write("\n};\n\n")
        f.write(f"#endif // {header_guard}\n")

    print(f"Successfully created {output_header}")
    print(f"Total pixels: {pixel_count}")
    print(f"Array size: {pixel_count * 2} bytes ({pixel_count * 2 / 1024:.2f} KB)")


def main():
    parser = argparse.ArgumentParser(description="Convert PNG to RGB565 header")
    parser.add_argument("input_png", help="Input PNG file")
    parser.add_argument("output_header", help="Output header file")
    parser.add_argument("logo_name", help="Base name for symbols (e.g. rdf_logo)")
    parser.add_argument("--target-width", type=int, default=None, help="Target width in pixels")
    parser.add_argument("--target-height", type=int, default=None, help="Target height in pixels")
    parser.add_argument("--mode", choices=["letterbox", "fit-width-crop", "fit-width-crop-top", "fit-width-crop-bottom", "fit-height-crop"], default="letterbox", help="Resize mode")
    parser.add_argument("--background", type=parse_hex_color, default=(0, 0, 0), help="Background color for letterboxing (hex)")
    parser.add_argument("--trim", action="store_true", help="Trim transparent borders before resizing")

    args = parser.parse_args()

    if not os.path.exists(args.input_png):
        print(f"Error: Input file '{args.input_png}' not found")
        sys.exit(1)

    convert_png_to_header(
        args.input_png,
        args.output_header,
        args.logo_name,
        target_width=args.target_width,
        target_height=args.target_height,
        background=args.background,
        mode=args.mode,
        trim=args.trim,
    )


if __name__ == "__main__":
    main()
