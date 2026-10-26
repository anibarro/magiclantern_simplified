#!/usr/bin/env python3
"""
XPan watermark converter: PNG (with transparency) -> ML/XPAN/WMARK.BIN

The camera blends the logo into one corner of the saved JPEG (finished YUV
image, before JPEG encoding). It cannot read PNG, so this script converts the
PNG once, at the two sizes offered by XPan > Watermark size:

    Small = 386 px wide
    Big   = 776 px wide
    (even widths: the camera blends pixel pairs)

Height follows the PNG's aspect ratio. Keep the logo wide rather than tall:
in 65:24 the frame is only 1920 px high (minus 2 x 128 px margin).
A separate, simpler design can be given for the small size with --small.

File format (little-endian):
    header 64 bytes: "XPWM", version 2, count 2 (small, big),
                     then per size: width, height, data offset, data bytes
    data per size:   rows of pixel pairs, 6 bytes each:
                     Y0 A0 Y1 A1 Cb Cr   (full-range BT.601, Cb/Cr signed int8)
    Chroma is shared by the two pixels of a pair (4:2:2), weighted by alpha.

Usage:
    python xpan_watermark.py logo.png                 -> WMARK.BIN
    python xpan_watermark.py logo.png --small logo_small.png
    python xpan_watermark.py logo.png --out P:/ML/XPAN/WMARK.BIN
    python xpan_watermark.py logo.png --preview       -> also WMARK_preview.png
Copy WMARK.BIN to ML/XPAN/ on the card and restart the camera
(the camera reads it once, the first time the watermark is ON).

Requires: pip install pillow numpy
"""
import argparse
import struct
import sys

import numpy as np
from PIL import Image

SIZES = (("small", 386), ("big", 776))
VERSION = 2
MARGIN = 128
FRAME_65_24 = 1920
MAGIC = b"XPWM"


def scale_premultiplied(rgba, width):
    """Resize with premultiplied alpha, so transparent edges get no dark fringe."""
    w0, h0 = rgba.size
    height = max(1, round(h0 * width / w0))
    a = np.asarray(rgba, dtype=np.float64) / 255.0
    pre = a.copy()
    pre[..., :3] *= pre[..., 3:4]
    chans = []
    for c in range(4):
        # Resample each premultiplied channel in float for accuracy.
        img =Image.fromarray((pre[..., c] * 255.0).astype(np.float32), "F")
        chans.append(np.asarray(img.resize((width, height), Image.LANCZOS), dtype=np.float64) / 255.0)
    out = np.clip(np.stack(chans, axis=-1), 0.0, 1.0)
    alpha = out[..., 3:4]
    rgb = np.where(alpha > 1e-6, out[..., :3] / np.maximum(alpha, 1e-6), 0.0)
    return np.clip(rgb, 0, 1), np.clip(alpha[..., 0], 0, 1)


def pad_even(rgb, alpha):
    """The camera blends pixel pairs: add one transparent column if needed."""
    if alpha.shape[1] % 2:
        rgb = np.concatenate([rgb, np.zeros((rgb.shape[0], 1, 3))], axis=1)
        alpha = np.concatenate([alpha, np.zeros((alpha.shape[0], 1))], axis=1)
    return rgb, alpha


def encode(rgb, alpha):
    """Pixel pairs -> bytes Y0 A0 Y1 A1 Cb Cr (BT.601 full range, signed chroma)."""
    r, g, b = rgb[..., 0] * 255, rgb[..., 1] * 255, rgb[..., 2] * 255
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = -0.168736 * r - 0.331264 * g + 0.5 * b
    cr = 0.5 * r - 0.418688 * g - 0.081312 * b
    a = alpha * 255.0
    h, w = y.shape
    a0, a1 = a[:, 0::2], a[:, 1::2]
    wsum = a0 + a1
    safe = np.where(wsum > 0, wsum, 1)
    cb_pair = np.where(wsum > 0, (cb[:, 0::2] * a0 + cb[:, 1::2] * a1) / safe, 0)
    cr_pair = np.where(wsum > 0, (cr[:, 0::2] * a0 + cr[:, 1::2] * a1) / safe, 0)
    out = np.zeros((h, w // 2, 6), dtype=np.uint8)
    out[..., 0] = np.clip(np.round(y[:, 0::2]), 0, 255)
    out[..., 1] = np.clip(np.round(a0), 0, 255)
    out[..., 2] = np.clip(np.round(y[:, 1::2]), 0, 255)
    out[..., 3] = np.clip(np.round(a1), 0, 255)
    out[..., 4] = np.clip(np.round(cb_pair), -128, 127).astype(np.int8).view(np.uint8)
    out[..., 5] = np.clip(np.round(cr_pair), -128, 127).astype(np.int8).view(np.uint8)
    return out.tobytes()


def preview(path, entries):
    """Composite each size on grey and on a dark tone, as the camera would."""
    tiles = []
    for name, (w, h, data) in entries.items():
        d = np.frombuffer(data, dtype=np.uint8).reshape(h, w // 2, 6)
        for base in (150, 40):
            Y = np.full((h, w), float(base))
            Cb = np.zeros((h, w // 2))
            Cr = np.zeros((h, w // 2))
            a0, a1 = d[..., 1] / 255.0, d[..., 3] / 255.0
            Y[:, 0::2] += (d[..., 0] - Y[:, 0::2]) * a0
            Y[:, 1::2] += (d[..., 2] - Y[:, 1::2]) * a1
            ac = (a0 + a1) / 2
            Cb += (d[..., 4].view(np.int8) - Cb) * ac
            Cr += (d[..., 5].view(np.int8) - Cr) * ac
            cb = np.repeat(Cb, 2, axis=1)
            cr = np.repeat(Cr, 2, axis=1)
            R = Y + 1.402 * cr
            G = Y - 0.344136 * cb - 0.714136 * cr
            B = Y + 1.772 * cb
            tiles.append(np.uint8(np.clip(np.stack([R, G, B], -1), 0, 255)))
    width = max(t.shape[1] for t in tiles)
    canvas = np.full((sum(t.shape[0] + 20 for t in tiles), width, 3), 255, np.uint8)
    yy = 0
    for t in tiles:
        canvas[yy:yy + t.shape[0], :t.shape[1]] = t
        yy += t.shape[0] + 20
    Image.fromarray(canvas).save(path)


def main():
    ap = argparse.ArgumentParser(description="Convert a PNG logo into XPan's WMARK.BIN")
    ap.add_argument("png", help="logo PNG (transparency is kept); used for both sizes")
    ap.add_argument("--small", help="optional separate PNG for the small size")
    ap.add_argument("--out", default="WMARK.BIN", help="output file (default WMARK.BIN)")
    ap.add_argument("--preview", action="store_true", help="also write WMARK_preview.png")
    args = ap.parse_args()

    def load(path):
        img = Image.open(path).convert("RGBA")
        # Trim fully transparent borders so the margin is measured from the logo itself.
        bbox = img.getchannel("A").getbbox()
        if not bbox:
            sys.exit(f"{path} is fully transparent.")
        return img.crop(bbox)

    sources = {"big": load(args.png)}
    sources["small"] = load(args.small) if args.small else sources["big"]

    entries = {}
    for name, width in SIZES:
        rgb, alpha = pad_even(*scale_premultiplied(sources[name], width))
        h, w = alpha.shape
        if h + 2 * MARGIN > FRAME_65_24:
            print(f"warning: {name} logo is {h} px high; it will be skipped in 65:24 "
                  f"(max {FRAME_65_24 - 2 * MARGIN} px). Use a wider logo.")
        entries[name] = (w, h, encode(rgb, alpha))

    header = bytearray(64)
    header[0:4] = MAGIC
    struct.pack_into("<II", header, 4, VERSION, len(SIZES))
    offset = 64
    for i, name in enumerate(n for n, _ in SIZES):
        w, h, data = entries[name]
        struct.pack_into("<IIII", header, 12 + i * 16, w, h, offset, len(data))
        offset += len(data)
    with open(args.out, "wb") as f:
        f.write(header)
        for name, _ in SIZES:
            f.write(entries[name][2])
    for name, _ in SIZES:
        w, h, data = entries[name]
        print(f"{name:6s}: {w} x {h} px, {len(data) / 1024:.0f} KiB")
    print(f"wrote {args.out} ({offset / 1024:.0f} KiB). Copy it to ML/XPAN/ on the card and restart the camera.")
    if args.preview:
        prev = args.out.rsplit(".", 1)[0] + "_preview.png"
        preview(prev, entries)
        print(f"wrote {prev}")


if __name__ == "__main__":
    main()
