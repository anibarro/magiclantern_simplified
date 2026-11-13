#!/usr/bin/env python3
"""XPan halation prototype (offline, README-XPAN-M50.md "Halation prototype").

Simulates film halation on a finished JPEG the way XPan's filter stage would
do it in the camera (YCbCr after the LUT, before the JPEG encoder):

  1. key map at 1/8 scale: mean over 8x8 blocks of key(Y), where key(Y) is a
     256-entry table that rises from the threshold to the base's white
     (a smoothstep, squared, so only the top of the range glows);
  2. two Gaussian blurs of that small map: a tight inner glow and a faint
     wide tail (radii as a fraction of the image width, as on film the
     spread is a fixed distance in the emulsion);
  3. only the glow outside the highlight counts (glow minus the pixel's own
     key: inside a full highlight halation cannot be seen, on film too);
     glow up-sampled bilinearly and added in linear light with a screen
     blend (dark surroundings get it, bright areas barely change), tinted
     orange-red inside, deeper red in the tail.

The camera version would do steps 1 and 3 in the grain worker's row passes
and step 2 on the small map; --camera uses only integer YCbCr maths of that
kind so its result can be compared with the float reference.

Usage:
  python xpan_halation_proto.py IN.JPG OUT.JPG [--strength 0.6]
      [--thr 30] [--white auto|N] [--inner 0.0025] [--outer 0.012]
      [--tail 0.35] [--camera]
"""
import argparse
import numpy as np
from PIL import Image
from scipy.ndimage import gaussian_filter

SCALE = 8


def key_table(thr_levels, white):
    """256-entry key: 0 below white-thr, 1 at white (smoothstep, squared)."""
    y = np.arange(256, dtype=np.float64)
    t = np.clip((y - (white - thr_levels)) / max(thr_levels, 1), 0, 1)
    s = t * t * (3 - 2 * t)
    return s * s


def small_key(y, table):
    h, w = y.shape
    h8, w8 = h // SCALE, w // SCALE
    k = table[y[:h8 * SCALE, :w8 * SCALE]]
    return k.reshape(h8, SCALE, w8, SCALE).mean(axis=(1, 3))


def upsample(m, h, w):
    """Bilinear, block centres at (i+0.5)*SCALE (as the camera would)."""
    h8, w8 = m.shape
    ys = (np.arange(h) + 0.5) / SCALE - 0.5
    xs = (np.arange(w) + 0.5) / SCALE - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, h8 - 1); y1 = np.clip(y0 + 1, 0, h8 - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, w8 - 1); x1 = np.clip(x0 + 1, 0, w8 - 1)
    fy = np.clip(ys - np.floor(ys), 0, 1)[:, None]
    fx = np.clip(xs - np.floor(xs), 0, 1)[None, :]
    a = m[y0][:, x0]; b = m[y0][:, x1]; c = m[y1][:, x0]; d = m[y1][:, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def glow_maps(y, args, white):
    h, w = y.shape
    k = small_key(y, key_table(args.thr, white))
    si = args.inner * w / SCALE
    so = args.outer * w / SCALE
    gi = gaussian_filter(k, si, mode='nearest')
    go = gaussian_filter(k, so, mode='nearest')
    return upsample(gi, h, w), upsample(go, h, w)


# Tints (linear RGB per unit of glow): inner orange-red, tail deeper red.
INNER = np.array([1.00, 0.30, 0.10])
OUTER = np.array([1.00, 0.12, 0.03])


def halation_float(rgb, args, white):
    ycc = np.asarray(Image.fromarray(rgb).convert('YCbCr'))
    gi, go = glow_maps(ycc[..., 0], args, white)
    kp = key_table(args.thr, white)[ycc[..., 0]]   # the pixel's own key
    gi = np.maximum(gi - kp, 0); go = np.maximum(go - kp, 0)
    lin = (rgb.astype(np.float64) / 255.0) ** 2.2
    g = args.strength * ((1 - args.tail) * gi[..., None] * INNER + args.tail * go[..., None] * OUTER)
    out = 1 - (1 - lin) * (1 - np.clip(g, 0, 1))        # screen in linear light
    return np.clip(np.round(255 * out ** (1 / 2.2)), 0, 255).astype(np.uint8), gi, go


def halation_camera(rgb, args, white):
    """The camera's integer maths. Small map: glow per channel in 1/4096
    (16-bit words, the two radii and tints already mixed: 3 x 750 x 500 x 2
    bytes = 2.2 MB). Per pixel: the glow from the map (bilinear), minus the
    pixel's own key (256-entry table on Y); pixels with no glow left are not
    touched (most of the image). The rest: YCbCr -> RGB (integer matrix),
    linear from a 256-entry 16-bit table, screen blend, back with a
    4096-entry table, RGB -> YCbCr."""
    ycc = np.asarray(Image.fromarray(rgb).convert('YCbCr')).astype(np.int32)
    Y, Cb, Cr = ycc[..., 0], ycc[..., 1], ycc[..., 2]
    gi, go = glow_maps(Y.astype(np.uint8), args, white)
    kp = key_table(args.thr, white)[Y]             # the pixel's own key (table)
    gi = np.maximum(gi - kp, 0); go = np.maximum(go - kp, 0)
    g = args.strength * ((1 - args.tail) * gi[..., None] * INNER + args.tail * go[..., None] * OUTER)
    G = np.clip(np.round(4096 * g), 0, 4096).astype(np.int32)          # 1/4096
    touched = G[..., 0] > 0
    lin_t = np.round(65535 * (np.arange(256) / 255.0) ** 2.2).astype(np.int32)
    inv_t = np.round(255 * (np.arange(4096) / 4095.0) ** (1 / 2.2)).astype(np.int32)
    # JPEG YCbCr -> RGB, 16.16 fixed point
    c, d = Cb - 128, Cr - 128
    r = np.clip(Y + ((91881 * d) >> 16), 0, 255)
    gg = np.clip(Y - ((22554 * c + 46802 * d) >> 16), 0, 255)
    b = np.clip(Y + ((116130 * c) >> 16), 0, 255)
    out = []
    for ch, v in enumerate((r, gg, b)):
        L = lin_t[v]
        L = L + (((65535 - L) * G[..., ch]) >> 12)                       # screen
        out.append(inv_t[np.clip(L >> 4, 0, 4095)])
    r2, g2, b2 = out
    Y2 = (19595 * r2 + 38470 * g2 + 7471 * b2 + 32768) >> 16
    Cb2 = ((-11059 * r2 - 21709 * g2 + 32768 * b2 + 32768) >> 16) + 128
    Cr2 = ((32768 * r2 - 27439 * g2 - 5329 * b2 + 32768) >> 16) + 128
    Yo = np.where(touched, Y2, Y); Cbo = np.where(touched, Cb2, Cb); Cro = np.where(touched, Cr2, Cr)
    o = np.clip(np.stack([Yo, Cbo, Cro], -1), 0, 255).astype(np.uint8)
    print('camera maths: %.1f%% of the pixels touched' % (100 * touched.mean()))
    return np.asarray(Image.fromarray(o, 'YCbCr').convert('RGB')), gi, go


def synthetic(path, w=6000, h=4000):
    """Night scene: dark wall, a lit window, small lamps, a bright line and
    a white card, at XPan Cineon's white (Y 222)."""
    img = np.full((h, w, 3), (28, 26, 24), np.float64)
    yy, xx = np.mgrid[0:h, 0:w]
    img[600:1700, 600:1800] = (222, 222, 218)                       # window
    img[1100:1180, 600:1800] = (30, 28, 26); img[600:1700, 1170:1230] = (30, 28, 26)   # window bars
    for cx, cy, rad in ((3000, 1000, 40), (3600, 1200, 18), (4200, 900, 8), (5000, 1100, 60)):
        img[(xx - cx) ** 2 + (yy - cy) ** 2 < rad * rad] = (224, 222, 215)   # lamps
    img[2600:2630, 500:5500] = (222, 220, 214)                       # light strip
    img[3000:3700, 3800:4600] = (200, 198, 192)                     # white card (under the threshold)
    Image.fromarray(img.astype(np.uint8)).save(path, quality=95)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('inp'); p.add_argument('out')
    p.add_argument('--strength', type=float, default=0.6)
    p.add_argument('--thr', type=float, default=30, help='levels under white where the glow starts')
    p.add_argument('--white', default='auto', help="base white in Y levels; auto = 99.99th percentile")
    p.add_argument('--inner', type=float, default=0.0025, help='inner sigma, fraction of width')
    p.add_argument('--outer', type=float, default=0.012, help='tail sigma, fraction of width')
    p.add_argument('--tail', type=float, default=0.35, help='share of the wide tail')
    p.add_argument('--camera', action='store_true', help='integer maths as in the camera')
    p.add_argument('--synthetic', action='store_true', help='write a synthetic night scene to IN first')
    a = p.parse_args()
    if a.synthetic: synthetic(a.inp)
    rgb = np.asarray(Image.open(a.inp).convert('RGB'))
    y = np.asarray(Image.fromarray(rgb).convert('YCbCr'))[..., 0]
    white = float(np.percentile(y, 99.99)) if a.white == 'auto' else float(a.white)
    out, gi, go = (halation_camera if a.camera else halation_float)(rgb, a, white)
    Image.fromarray(out).save(a.out, quality=92)
    print('white %.0f, threshold from %.0f, glow max inner %.2f tail %.2f' % (white, white - a.thr, gi.max(), go.max()))


if __name__ == '__main__':
    main()
