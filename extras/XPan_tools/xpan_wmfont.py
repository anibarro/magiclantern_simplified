#!/usr/bin/env python3
"""
XPan watermark font converter: TTF -> ML/XPAN/WMFONT.BIN

The camera writes the LUT name next to the watermark logo on the saved JPEG.
It cannot render TrueType, so this script renders the printable ASCII
characters (32..126) once, anti-aliased, at the two watermark sizes:

    Small: capital height 43 px, next to the 386 px logo
    Big:   capital height 86 px, next to the 776 px logo

(measured on watermark_sample1.jpg / watermark_sample2.jpg, scaled to the
5184 px wide JPEG). The gap between logo and text is stored per size too.
The text colour is not stored: the camera uses the logo's colour.

File format (little-endian):
    header 64 bytes: "XPWF", version 1, size count 2, first char 32, char count 95,
                     then per size (small, big): font px, advance in 1/64 px,
                     capital height, logo-text gap, glyph table offset
    glyph table per size: 95 entries of 12 bytes:
                     int16 x offset (from the pen), int16 y offset (top, from the
                     baseline, negative = above), uint16 width, uint16 height,
                     uint32 offset of the alpha bytes in the file
    alpha bytes:     width x height, 0 = transparent, 255 = opaque

Usage:
    python xpan_wmfont.py                                  -> ../../data/XPAN/WMFONT.BIN
    python xpan_wmfont.py --font OneSlot.ttf --out WMFONT.BIN --preview
Copy WMFONT.BIN to ML/XPAN/ on the card and restart the camera
(the camera reads it once, the first time the LUT name is needed).

Requires: pip install pillow numpy
"""
import argparse
import os
import struct
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_FONT = os.path.join(HERE, "..", "LUT_tools", "OneSlot.ttf")
DEFAULT_OUT = os.path.join(HERE, "..", "..", "data", "XPAN", "WMFONT.BIN")
# (name, capital height, logo-text gap), in pixels of the 5184 px JPEG.
SIZES = (("small", 43, 34), ("big", 86, 67))
FIRST, COUNT = 32, 95
VERSION = 1
MAGIC = b"XPWF"


def font_for_cap_height(path, cap):
    """Font size whose capital 'H' is 'cap' pixels tall."""
    probe = ImageFont.truetype(path, 200)
    l, t, r, b = probe.getbbox("H", anchor="ls")
    px = max(4, round(200 * cap / (b - t)))
    return ImageFont.truetype(path, px), px


def render(font, ch):
    """Tight alpha box of one glyph, relative to the pen on the baseline."""
    l, t, r, b = font.getbbox(ch, anchor="ls")
    if r <= l or b <= t:
        return 0, 0, np.zeros((0, 0), np.uint8)
    img = Image.new("L", (r - l + 4, b - t + 4), 0)
    ImageDraw.Draw(img).text((2 - l, 2 - t), ch, font=font, fill=255, anchor="ls")
    a = np.asarray(img)
    ys, xs = np.nonzero(a)
    if not len(xs):
        return 0, 0, np.zeros((0, 0), np.uint8)
    a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    return l - 2 + xs.min(), t - 2 + ys.min(), a


def preview(path, entries, text="Kodak Film  Cine FUJI  B&W 2383"):
    rows = []
    for name, (px, advance, cap, gap, glyphs) in entries.items():
        width = int(len(text) * advance) + 40
        h = cap * 3
        canvas = np.full((h, width), 40.0)
        base = h // 2 + cap // 2
        for i, ch in enumerate(text):
            x0, y0, a = glyphs[ord(ch) - FIRST]
            if not a.size:
                continue
            x = 20 + round(i * advance) + x0
            y = base + y0
            canvas[y:y + a.shape[0], x:x + a.shape[1]] += (255 - canvas[y:y + a.shape[0], x:x + a.shape[1]]) * a / 255
        rows.append(np.uint8(canvas))
    width = max(r.shape[1] for r in rows)
    out = np.full((sum(r.shape[0] for r in rows), width), 255, np.uint8)
    y = 0
    for r in rows:
        out[y:y + r.shape[0], :r.shape[1]] = r
        y += r.shape[0]
    Image.fromarray(out).save(path)


def main():
    ap = argparse.ArgumentParser(description="Convert a TTF font into XPan's WMFONT.BIN")
    ap.add_argument("--font", default=DEFAULT_FONT, help="TrueType font (default ../LUT_tools/OneSlot.ttf)")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output file (default ../../data/XPAN/WMFONT.BIN)")
    ap.add_argument("--preview", action="store_true", help="also write WMFONT_preview.png")
    args = ap.parse_args()

    entries = {}
    for name, cap, gap in SIZES:
        font, px = font_for_cap_height(args.font, cap)
        advance = font.getlength("M")
        glyphs = [render(font, chr(c)) for c in range(FIRST, FIRST + COUNT)]
        entries[name] = (px, advance, cap, gap, glyphs)

    header = bytearray(64)
    header[0:4] = MAGIC
    struct.pack_into("<IIII", header, 4, VERSION, len(SIZES), FIRST, COUNT)
    tables = bytearray()
    data = bytearray()
    table_bytes = len(SIZES) * COUNT * 12
    for i, (name, _, _) in enumerate(SIZES):
        px, advance, cap, gap, glyphs = entries[name]
        struct.pack_into("<IIIII", header, 20 + i * 20, px, round(advance * 64), cap, gap,
                         64 + len(tables))
        for x0, y0, a in glyphs:
            h, w = a.shape
            tables += struct.pack("<hhHHI", x0, y0, w, h, 64 + table_bytes + len(data))
            data += a.tobytes()
    with open(args.out, "wb") as f:
        f.write(header)
        f.write(tables)
        f.write(data)
    for name, _, _ in SIZES:
        px, advance, cap, gap, glyphs = entries[name]
        print(f"{name:6s}: {px} px font, capital {cap} px, advance {advance:.2f} px, gap {gap} px")
    print(f"wrote {args.out} ({(64 + len(tables) + len(data)) / 1024:.0f} KiB). "
          "Copy it to ML/XPAN/ on the card and restart the camera.")
    if args.preview:
        prev = args.out.rsplit(".", 1)[0] + "_preview.png"
        preview(prev, entries)
        print(f"wrote {prev}")


if __name__ == "__main__":
    sys.exit(main())
