#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
make_icns.py - renders resources/macos/HdrHint.icns (the macOS app icon).

The same mark as scripts/make_icon.ps1 (a purple -> blue diagonal gradient tile
with a bold white "H"), drawn on Apple's macOS 11+ icon grid: a 1024 px canvas
with an 824 px rounded-square body, 100 px margins and a soft drop shadow, so
the icon sits in the Dock and Finder at the same optical size as Apple's own.

Every size is rendered from the 1024 px master with Lanczos filtering and the
.icns container holds PNG-compressed entries (16 ... 1024 px, 1x and 2x).

Usage:
    python scripts/make_icns.py [--out resources/macos/HdrHint.icns] [--png preview.png]

Needs Pillow (pip install pillow). Works the same on Windows and macOS.
"""

from __future__ import annotations

import argparse
import io
import os
import struct
import sys
from pathlib import Path

# Windows consoles default to a legacy code page: keep every print UTF-8 safe.
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    except (OSError, ValueError):
        pass

try:
    from PIL import Image, ImageDraw, ImageFilter, ImageFont
except ImportError:  # pragma: no cover - reported to the user
    print("Pillow is required: pip install pillow", file=sys.stderr)
    sys.exit(2)

# ---------------------------------------------------------------------------
# Design constants (Apple's macOS app icon grid, 1024 px canvas)
# ---------------------------------------------------------------------------
CANVAS = 1024
BODY = 824                      # rounded-square body
MARGIN = (CANVAS - BODY) // 2   # 100 px on every side
RADIUS = 185                    # corner radius of the body
SHADOW_BLUR = 22                # drop shadow under the body
SHADOW_OFFSET_Y = 12
SHADOW_ALPHA = 90               # out of 255

# Gradient ends: identical to make_icon.ps1 (Windows .ico, CEP panel icons).
GRADIENT_FROM = (176, 90, 255)  # top-left
GRADIENT_TO = (10, 132, 255)    # bottom-right

# Fonts tried in order for the "H" (heavy, geometric sans).
FONT_CANDIDATES = [
    "/System/Library/Fonts/SFNS.ttf",                      # macOS system font (variable)
    "/System/Library/Fonts/SFNSDisplay.ttf",
    "/Library/Fonts/SF-Pro-Display-Bold.otf",
    os.path.expandvars(r"%WINDIR%\Fonts\SFPRODISPLAYBOLD.OTF"),
    os.path.expandvars(r"%WINDIR%\Fonts\segoeuib.ttf"),
    "/System/Library/Fonts/Helvetica.ttc",
    os.path.expandvars(r"%WINDIR%\Fonts\arialbd.ttf"),
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]

# .icns entry types -> pixel size (PNG payloads).
ICNS_ENTRIES = [
    ("icp4", 16), ("icp5", 32), ("icp6", 64), ("ic07", 128), ("ic08", 256), ("ic09", 512),
    ("ic10", 1024),  # 512@2x
    ("ic11", 32),    # 16@2x
    ("ic12", 64),    # 32@2x
    ("ic13", 256),   # 128@2x
    ("ic14", 512),   # 256@2x
]


def load_font(size: int) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    """First available bold font at @size px; Pillow's default as a last resort."""
    for candidate in FONT_CANDIDATES:
        if candidate and os.path.isfile(candidate):
            try:
                font = ImageFont.truetype(candidate, size)
                # Variable system fonts: ask for the Bold instance when offered.
                try:
                    font.set_variation_by_name("Bold")
                except (OSError, ValueError, AttributeError):
                    pass
                return font
            except OSError:
                continue
    print("warning: no bold system font found; using Pillow's default", file=sys.stderr)
    return ImageFont.load_default()


def diagonal_gradient(size: int) -> Image.Image:
    """Square RGBA image with the top-left -> bottom-right gradient."""
    # A 2-pixel ramp stretched diagonally is cheap and perfectly smooth.
    ramp = Image.new("RGBA", (256, 1))
    for x in range(256):
        t = x / 255.0
        ramp.putpixel((x, 0), tuple(int(round(a + (b - a) * t)) for a, b in zip(GRADIENT_FROM, GRADIENT_TO)) + (255,))
    # Rotate a horizontal ramp by 45 degrees and crop the centre square.
    big = ramp.resize((size * 2, size * 2), Image.BICUBIC).rotate(-45, resample=Image.BICUBIC, expand=False)
    offset = size // 2
    return big.crop((offset, offset, offset + size, offset + size))


def render_master() -> Image.Image:
    """The 1024 px icon: shadow, gradient body, white H."""
    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))

    # ---- body mask (rounded square on the grid) -------------------------------
    mask = Image.new("L", (CANVAS, CANVAS), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (MARGIN, MARGIN, MARGIN + BODY - 1, MARGIN + BODY - 1), radius=RADIUS, fill=255)

    # ---- soft drop shadow ---------------------------------------------------------------
    shadow = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    shadow_mask = Image.new("L", (CANVAS, CANVAS), 0)
    shadow_mask.paste(mask, (0, SHADOW_OFFSET_Y))
    shadow_mask = shadow_mask.filter(ImageFilter.GaussianBlur(SHADOW_BLUR))
    shadow.putalpha(shadow_mask.point(lambda v: v * SHADOW_ALPHA // 255))
    canvas = Image.alpha_composite(canvas, shadow)

    # ---- gradient body ------------------------------------------------------------------
    body = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    body.paste(diagonal_gradient(BODY), (MARGIN, MARGIN))
    body.putalpha(mask)
    canvas = Image.alpha_composite(canvas, body)

    # ---- the letter (same proportions as the Windows mark: 58 % of the tile) ---------
    font = load_font(int(BODY * 0.58))
    glyph = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(glyph)
    left, top, right, bottom = draw.textbbox((0, 0), "H", font=font)
    x = (CANVAS - (right - left)) / 2 - left
    y = (CANVAS - (bottom - top)) / 2 - top + BODY * 0.01
    # A faint shadow lifts the letter off the gradient.
    letter_shadow = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    ImageDraw.Draw(letter_shadow).text((x, y + 8), "H", font=font, fill=(20, 0, 60, 70))
    letter_shadow = letter_shadow.filter(ImageFilter.GaussianBlur(10))
    canvas = Image.alpha_composite(canvas, letter_shadow)
    draw.text((x, y), "H", font=font, fill=(255, 255, 255, 255))
    return Image.alpha_composite(canvas, glyph)


def png_bytes(image: Image.Image) -> bytes:
    """PNG-encoded RGBA."""
    buffer = io.BytesIO()
    image.save(buffer, format="PNG", optimize=True)
    return buffer.getvalue()


def build_icns(master: Image.Image) -> bytes:
    """The .icns container with PNG entries for every size."""
    cache: dict[int, bytes] = {}
    chunks = []
    for kind, size in ICNS_ENTRIES:
        if size not in cache:
            scaled = master if size == CANVAS else master.resize((size, size), Image.LANCZOS)
            cache[size] = png_bytes(scaled)
        data = cache[size]
        chunks.append(kind.encode("ascii") + struct.pack(">I", len(data) + 8) + data)
    body = b"".join(chunks)
    return b"icns" + struct.pack(">I", len(body) + 8) + body


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Render the macOS app icon (.icns).")
    parser.add_argument("--out", type=Path, default=root / "resources" / "macos" / "HdrHint.icns")
    parser.add_argument("--png", type=Path, default=None, help="also write the 1024 px master as PNG")
    args = parser.parse_args()

    master = render_master()
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(build_icns(master))
    print(f"wrote {args.out} ({args.out.stat().st_size:,} bytes)")
    if args.png is not None:
        args.png.parent.mkdir(parents=True, exist_ok=True)
        master.save(args.png)
        print(f"wrote {args.png}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
