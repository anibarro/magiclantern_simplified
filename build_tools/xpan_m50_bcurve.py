#!/usr/bin/env python3
"""XPan's Cineon log for Canon's David tone curve B (M50).

README-XPAN-M50.md "XPan's Cineon log in curve B":
* curve B has 25 points (P1 +4E8..+548), after the header 800 100 100;
  point u = index + 1 (the header's last 100 is u = 0);
* JPEG luma = B's output / 8 (B flat 100..800: 32, 64, ... 225);
* the sensor clip is at u = 20.5 (B straight: brightest luma 216);
* where each point sits in exposure (EV below the sensor clip) was fitted
  from two brackets one EV apart, B straight (IMG_2856-2860) and the first
  Cineon curve (IMG_2878-2882), together: 795 pixel-level pairs, residual
  0.02 EV median (scratchpad upos.py, 2026-11-12). The first curve assumed
  evenly spaced points (u - 0.9) / 19.6; the fit shows the points sit lower
  in the shadows and mid-tones, which made its lower mid-tones about 1.6x
  too steep (steps of +35..+40 levels per EV instead of 22.6).

Each point gets the Cineon code of its scene value X = 2^EV (X = 1 at the
sensor clip, RAW white, as XPan's EOS M Cineon base; cut at 1 above it),
with the EOS M base's encoding (gain 0.18 x 2^3.5, black code 95), as JPEG
luma code / 4, i.e. B output = 2 x code.

The toe (README "Extending the toe", header test IMG_2890-2893): the first
0x100 word before the curve acts as its lowest point (u = 0, about -5.82 EV
in the fit); the second has no effect. With --header the script prints
that point's Cineon value instead.

Exposure placement (README "Old and new Cineon base compared"): IDENT
photos IMG_2902 (old "Cineon log" base, CINLV.cube) and IMG_2904 (A+B
Cineon) put every mid-tone a constant +25 JPEG levels (100 Cineon codes,
1.1 EV) apart, with the same contrast. XPan's LUTs are made for the old
placement, so X is scaled by 2^EV_SHIFT: the sensor clip then lands at the
old base's top (about code 680) instead of 777.

Shoulder (README "Highlight roll-off"): IMG_2907-2910 showed the pure log
running into the clip with no roll-off (4343 pixels on one level at the
top of the new base, a smooth spread on the old one) and the mid-tones 8
levels (0.35 EV) under the old base. So, as for the M50's CIN1EV base:
above KNEE EV below the clip, log2 X is replaced by a quadratic with slope 1
at the knee and END_SLOPE at the clip; and EV_SHIFT is -0.75 so the
mid-tones match the old base. README "Levels and range check": a full
shoulder (end slope 0) left 9 codes for the last half stop; 0.5 keeps 27
(pure log: 45) and puts the clip at code 683, the old base's top.

Prints the C initialiser; build_xpan.sh checks that src/xpan-m50.c holds
exactly these values.
"""
import math

# EV below the sensor clip of points u = 1..20 (fit above); u >= 21 is above
# the clip.
EV = [-5.38, -4.08, -3.35, -2.92, -2.50, -2.28, -2.06, -1.82, -1.59, -1.44,
      -1.35, -1.24, -1.15, -1.02, -0.89, -0.78, -0.64, -0.45, -0.34, -0.11]
EV_U0 = -5.82
EV_SHIFT = -0.75
KNEE = 1.2
END_SLOPE = 0.5
GAIN = 0.18 * 2 ** 3.5
BB = 10 ** ((95 - 685) * 0.002 / 0.6)


def cineon(l):
    return (685 + 300 * math.log10(max(l, 0) * (1 - BB) + BB)) / 1023


def shoulder(ev):
    """EV below the clip (<= 0) -> rolled-off EV."""
    if ev <= -KNEE:
        return ev
    d = ev + KNEE
    return -KNEE + d - (1 - END_SLOPE) * d * d / (2 * KNEE)


def code2(ev):
    return int(round(2 * 1023 * cineon(2 ** (shoulder(min(ev, 0.0)) + EV_SHIFT) * GAIN)))


def values():
    out = []
    for i in range(25):
        out.append(code2(EV[i] if i < len(EV) else 0.0))
    return out


import sys
if __name__ == "__main__" and not {"--tables", "--canonb", "--cin0"} & set(sys.argv):
    import sys
    if "--header" in sys.argv:
        print("0x%03X" % code2(EV_U0))
        sys.exit(0)
    v = values()
    assert all(b >= a for a, b in zip(v, v[1:])) and 0x100 <= v[0] and v[-1] <= 0x800
    print("{" + ",".join("0x%03X" % x for x in v) + "}")


# ---------------------------------------------------------------------------
# Tone base "XPan Cineon" (README "XPan Cineon tone base"): curves for every
# Knee x Slope setting, with and without the highlight clip raise.
#
# Knee: the shoulder starts where the pure-log output reaches KNEES[k] % of
# its top (from Cineon black 95). Slope: how much of the slope is gone at the
# top, SLOPES[s] % (0 = pure log, 100 = flat at the top).
# Clip raise: Hilton's clip word is raised by 5/4 (README "Real extra
# range"), so the red and blue channels go up to X = 1.25. Points above the
# old clip are extrapolated from the fit's last two (X 0.924 at u = 20,
# 1.082 at u = 21: 0.158 per point).
KNEES = [60, 70, 80, 90]

# Measured point positions (README "Camera test of `30e00acc5bcd`", B top
# ramp bracket IMG_2937-2948, scratchpad top_fit.py): EV of points u = 11..21
# from point 20, mean of green and blue (they agree within ~0.05 EV; red
# does not fit on a warm wall). The sensor clip is at point 20.1 in every
# channel, CLIP_FROM_20 EV above point 20. Points 1..10 keep the bracket fit
# above (only 3 levels per point in the ramp there), moved to the same
# reference: the old fit put point 20 at -0.11 EV with the clip at 20.5.
TOP_FROM_20 = [-1.108, -0.986, -0.903, -0.804, -0.699, -0.608, -0.4955,
               -0.372, -0.2415, 0.0, 0.2065]
CLIP_FROM_20 = 0.022
EV_M = ([e - EV[19] - CLIP_FROM_20 for e in EV[:10]] +
        [e - CLIP_FROM_20 for e in TOP_FROM_20])        # u = 1..21, from the clip
EV_M_U0 = EV_U0 - EV[19] - CLIP_FROM_20
STEP_ABOVE = 2 ** EV_M[20] - 2 ** EV_M[19]               # X per point above 20
# The old fit's reference made every point 0.088 EV too low; XPan's LUTs were
# matched on it (EV_SHIFT), so the same scene levels keep their output.
EV_SHIFT_M = EV_SHIFT + (EV[19] + CLIP_FROM_20)
SLOPES = [0, 25, 50, 75, 100]
RAISE = 1.25


def x_of_point(i):
    """Linear light of point u = i + 1 (1 = sensor clip), also above the
    clip: the hardware interpolates between points 20 and 21 for the last
    tenth of a point under the clip, so point 21 needs its own value."""
    u = i + 1
    if u <= 21:
        return 2 ** EV_M[i]
    return 2 ** EV_M[20] + STEP_ABOVE * (u - 21)


def pure_code(ev):
    return 1023 * cineon(2 ** (ev + EV_SHIFT_M) * GAIN)


def knee_ev(top_ev, pct):
    """EV where the pure log reaches pct % of its top (from code 95)."""
    target = 95 + pct / 100 * (pure_code(top_ev) - 95)
    lo, hi = top_ev - 12, top_ev
    for _ in range(60):
        mid = (lo + hi) / 2
        if pure_code(mid) < target:
            lo = mid
        else:
            hi = mid
    return lo


def curve(knee_pct, slope_pct, raised):
    top = math.log2(RAISE) if raised else 0.0
    k = top - knee_ev(top, knee_pct)
    end = 1 - slope_pct / 100
    def roll(e):
        if e <= top - k:
            return e
        if e > top:        # above the clip: on at the end slope (monotone)
            return roll(top) + end * (e - top)
        d = e - (top - k)
        return top - k + d - (1 - end) * d * d / (2 * k)
    out = []
    for i in range(25):
        e = math.log2(x_of_point(i))
        out.append(min(0x800, int(round(2 * 1023 * cineon(2 ** (roll(e) + EV_SHIFT_M) * GAIN)))))
    return out


def curve_a():
    """Curve A (David's colour curve): pure Cineon log on every point, no
    shoulder, continued above the clip (README "Highlight colour bands").
    Canon's own A keeps rising past white (... 7F2 802 80F 81A 823 829)
    while its B flattens; a shoulder or a flat top on A bends the colour
    channels unevenly near white (hue bands, IMG_2911-2914)."""
    out = []
    for i in range(25):
        out.append(min(0x800, int(round(2 * 1023 * cineon(x_of_point(i) * 2 ** EV_SHIFT_M * GAIN)))))
    return out


def c_tables():
    a = curve_a()
    assert all(b > c for c, b in zip(a, a[1:])) and 0x100 <= a[0] and a[-1] <= 0x800
    lines = ["static const uint16_t m50_xcin_a[25]={" + ",".join("0x%03X" % x for x in a) + "};"]
    lines += ["static const uint16_t m50_xcin_curve[2][%d][%d][25]={" % (len(KNEES), len(SLOPES))]
    for r in (0, 1):
        lines.append(" {")
        for kp in KNEES:
            row = []
            for sp in SLOPES:
                v = curve(kp, sp, r)
                assert all(b >= a for a, b in zip(v, v[1:])) and 0x100 <= v[0] and v[-1] <= 0x800
                row.append("{" + ",".join("0x%03X" % x for x in v) + "}")
            lines.append("  {" + ",".join(row) + "},")
        lines.append(" },")
    lines.append("};")
    return "\n".join(lines)


if __name__ == "__main__" and "--tables" in sys.argv:
    print(c_tables())


# ---------------------------------------------------------------------------
# Tone base "XPan Cineon" (README "XPan Cineon from Canon's curve"): Canon's
# own Neutral curve B, with the style's contrast taken out: the David hook
# writes this copy of Canon's contrast-0 curve B, and the colour table maps
# Canon's render to Cineon with the M50's own base (data/XPAN/M50/CIN0EV.cube,
# xpan_m50_cin1ev.py with XPAN_RENDER0=1, fitted on Canon's render).
# CANON_B: as the David probe logged it in 84 photo records (IMG_2949 and
# the sessions before it; P1 +4E8...+548; header 800 100 100).
CANON_B = [0x128, 0x2DE, 0x3EA, 0x4A2, 0x529, 0x593, 0x5F2, 0x641, 0x67D,
           0x6B3, 0x6E4, 0x70D, 0x730, 0x753, 0x770, 0x788, 0x7A0, 0x7C6,
           0x7E1, 0x7F6, 0x808, 0x808, 0x808, 0x808, 0x808]

if __name__ == "__main__" and "--canonb" in sys.argv:
    print("{" + ",".join("0x%03X" % x for x in CANON_B) + "}")


# ---------------------------------------------------------------------------
# The M50 base for Canon's render (README "XPan Cineon from Canon's curve"),
# with the user's Shoulder knee / slope: data/XPAN/M50/cin0ev_nodes.txt
# (xpan_m50_cin1ev.py, XPAN_RENDER0=1: the scene value of Canon's 17 grey
# nodes, relative to Canon's white W0) -> Cineon / 1023 x 65535 per node, for
# every Knee x Slope. Knee: the shoulder starts where the log reaches
# KNEES[k] % of the top (from Cineon black 95); Slope: how much of the slope
# is gone at Canon's white. The camera fills its 17-point base from the row.
def cin0_rows(path="data/XPAN/M50/cin0ev_nodes.txt"):
    vals = [float(l) for l in open(path) if l.strip() and not l.startswith("#")]
    w0, xs = vals[0], vals[1:]
    assert len(xs) == 17

    def code(e):
        return 1023 * cineon(w0 * 2 ** e * GAIN)
    rows = []
    for kp in KNEES:
        target = 95 + kp / 100 * (code(0.0) - 95)
        lo, hi = -12.0, 0.0
        for _ in range(60):
            mid = (lo + hi) / 2
            if code(mid) < target:
                lo = mid
            else:
                hi = mid
        k = -lo
        row = []
        for sp in SLOPES:
            end = 1 - sp / 100
            out = []
            for x in xs:
                if x <= 0:
                    out.append(int(round(65535 * cineon(0.0))))
                    continue
                e = math.log2(x)
                if e > -k:
                    d = e + k
                    e = -k + d - (1 - end) * d * d / (2 * k)
                out.append(int(round(65535 * cineon(w0 * 2 ** e * GAIN))))
            assert all(b > a for a, b in zip(out, out[1:]))
            row.append(out)
        rows.append((k, row))
    return rows


if __name__ == "__main__" and "--cin0" in sys.argv:
    lines = ["static const uint16_t m50_cin0_nodes[%d][%d][17]={" % (len(KNEES), len(SLOPES))]
    for k, row in cin0_rows():
        lines.append(" {" + ",".join("{" + ",".join("%d" % v for v in r) + "}" for r in row) + "},")
    lines.append("};")
    print("\n".join(lines))
