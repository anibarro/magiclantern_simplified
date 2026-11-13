#!/usr/bin/env python3
"""XPan M50 Cineon base for the highlight headroom (white balance gains -1 EV).

Calibration photos (README-XPAN-M50.md "Highlight headroom on the 14-bit
data", IMG_2736-2747): the same scene at four exposures, each developed by
Canon at white balance gains 0, -1 and -2 EV. ISO 100, tripod, Picture
Style Neutral, LUT IDENT, tone base Canon Neutral.

Canon's tone curve T is the same for every photo; X(J) = c * T^-1(J). Two
kinds of pixel pairs give it:
  * gain pairs (same exposure, gains one EV apart): log2 X(hi) - log2 X(lo)
    = 1 exactly (a lower gain is a linear pre-scale);
  * exposure pairs (same gain, neighbouring shutter times): log2 X(hi) -
    log2 X(lo) = d, one unknown d per pair, fitted too (so the shutter's
    real ratio does not matter; the pairs only add shape information).
log2 X(J) is piecewise linear over knots in J, fitted by least squares with
a smoothness term, on per-level medians; bins more than 0.2 EV off the fit
are dropped (the monitor in the scene changed between shots). Anchor: the
sensor clip in the -1 EV render (green) is X = 1 = RAW green white, the EOS
M convention. One curve for R, G and B; Canon's Neutral colour is kept.

Shoulder (camera test IMG_2748-2753: the log-correct base met the sensor
clip with no roll-off, "more abrupt" than Cineon log): above `knee` EV below
the clip, u = log2 X is replaced by a quadratic with slope 1 at the knee and
0 at the clip, so the clip lands knee/2 EV lower and is approached smoothly.

Cineon log encoding exactly as the EOS M base (xpan_cineon_base_v7.py: X <=
1, gain 0.18 x 2^3.5), so XPan's LUTs see the same scene scale. Plain
Python + PIL (no numpy in this WSL).

Usage: xpan_m50_cin1ev.py OUT.cube KNEE_EV T1 T2 ...
       each T: "0EV.JPG,-1EV.JPG,-2EV.JPG", one per exposure, from the
       shortest to the longest; the longest must reach the sensor clip.
"""
import sys, math
from PIL import Image

def load(p):
    return list(Image.open(p).convert('RGB').resize((1500, 554), Image.BILINEAR).getdata())

out_path = sys.argv[1]
knee = float(sys.argv[2])
T = [[load(p) for p in t.split(',')] for t in sys.argv[3:]]

def ceilings(A):
    """Per channel: where the sensor clip lands in this photo (99.99th percentile)."""
    return [sorted(p[c] for p in A)[int(len(A) * 0.9999)] for c in range(3)]
CL = [[ceilings(A) for A in t] for t in T]
ceil1 = CL[-1][1][1]                            # brightest exposure, -1 EV, green: X = 1
print('sensor clip in the -1 EV render (green): J = %d' % ceil1)

K = [8, 16, 32, 56, 88, 120, 152, 184, 210, 230, 255]
NK = len(K)
def basis(J):
    J = min(max(J, K[0]), K[-1])
    for i in range(NK - 1):
        if J <= K[i + 1]: break
    f = (J - K[i]) / (K[i + 1] - K[i]); b = [0.0] * NK
    b[i] = 1 - f; b[i + 1] = f
    return b

def pair_rows(lo, hi, cl, ch):
    """Per-level medians: lower photo's value -> brighter photo's value, nothing near a clip."""
    bins = {}
    for c in range(3):
        for pl, ph in zip(lo, hi):
            a, b = pl[c], ph[c]
            if a >= 10 and b >= 10 and a <= cl[c] - 8 and b <= min(ch[c], 255) - 8:
                bins.setdefault(a, []).append(b)
    out = []
    for v, l in bins.items():
        if len(l) >= 40: l.sort(); out.append((v, l[len(l) // 2]))
    return out

groups = []                                     # (rows, None = 1 EV exactly, or index of its unknown d)
gain_rows = []
for t, cl in zip(T, CL):
    gain_rows += pair_rows(t[1], t[0], cl[1], cl[0]) + pair_rows(t[2], t[1], cl[2], cl[1])
groups.append((gain_rows, None))
nd = 0
for e in range(len(T) - 1):
    for g in range(3):
        groups.append((pair_rows(T[e][g], T[e + 1][g], CL[e][g], CL[e + 1][g]), nd)); nd += 1
NP = NK + nd
print('bins: %d gain pairs, %d exposure pairs (%d unknown exposure steps)' % (
    len(gain_rows), sum(len(r) for r, d in groups[1:]), nd))

def solve(groups):
    AtA = [[0.0] * NP for _ in range(NP)]; Atb = [0.0] * NP
    def add(r, y):
        for i in range(NP):
            if r[i] == 0: continue
            Atb[i] += r[i] * y
            for j in range(NP): AtA[i][j] += r[i] * r[j]
    for rows, d in groups:
        for a, b in rows:
            ba, bb = basis(a), basis(b); r = [bb[k] - ba[k] for k in range(NK)] + [0.0] * nd
            if d is None: add([3 * x for x in r], 3.0)          # exact: weighted 3x
            else: r[NK + d] = -1.0; add(r, 0.0)
    add([100 * x for x in basis(ceil1)] + [0.0] * nd, 0.0)
    for d in range(nd):                         # tiny ridge: a step with no pairs stays defined
        r = [0.0] * NP; r[NK + d] = 1e-3; add(r, 0.0)
    for i in range(NK - 2):
        r = [0.0] * NP; r[i] = 0.3; r[i + 1] = -0.6; r[i + 2] = 0.3; add(r, 0.0)
    M = [AtA[i][:] + [Atb[i]] for i in range(NP)]
    for i in range(NP):
        p = max(range(i, NP), key=lambda k: abs(M[k][i])); M[i], M[p] = M[p], M[i]
        for k in range(NP):
            if k != i:
                f = M[k][i] / M[i][i]
                for j in range(i, NP + 1): M[k][j] -= f * M[i][j]
    return [M[i][NP] / M[i][i] for i in range(NP)]

def lx(P, J): return sum(w * l for w, l in zip(basis(J), P[:NK]))
def resid(P, a, b, d): return lx(P, b) - lx(P, a) - (1.0 if d is None else P[NK + d])
all_groups = groups
P = solve([groups[0]])                          # start: gain pairs only (exact)
for gi, (rows, d) in enumerate(all_groups):
    if d is not None:                           # each exposure step from the gain-only curve
        r = sorted(lx(P, b) - lx(P, a) for a, b in rows); P[NK + d] = r[len(r) // 2] if r else 0.0
for lim in (0.5, 0.35, 0.25, 0.2, 0.2):
    groups = [([(a, b) for a, b in rows if abs(resid(P, a, b, d)) < lim], d) for rows, d in all_groups]
    P = solve(groups)
    print('  trim %.2f EV: kept %s' % (lim, ' '.join('%d/%d' % (len(g[0]), len(a[0])) for g, a in zip(groups, all_groups))))
kept = sum(len(r) for r, d in groups); total = sum(len(r) for r, d in all_groups)
res = [resid(P, a, b, d) for rows, d in groups for a, b in rows]
print('fit: %d of %d bins kept, residual rms %.3f EV, max %.3f EV' % (kept, total, math.sqrt(sum(r * r for r in res) / len(res)), max(abs(r) for r in res)))
print('exposure steps found (EV):', ' '.join('%.2f' % x for x in P[NK:]))
L = P[:NK]
print('knots', ' '.join('%d:%.2f' % (k, l) for k, l in zip(K, L)))
if any(L[i + 1] <= L[i] for i in range(NK - 1)): sys.exit('fit not monotone')

def X_raw(J):
    if J < K[0]: return 2 ** L[0] * J / K[0]     # toe: linear to black
    return 2 ** lx(P, J)
def X_of(J):
    """Scene value of a -1 EV render value J, RAW green white = 1 (the sensor clip)."""
    if J >= ceil1: return 1.0
    return min(1.0, X_raw(J))
def shoulder(x):
    """log2 x above -knee: slope 1 at the knee, 0 at the clip (x = 1)."""
    if x <= 0: return 0.0
    u = math.log2(x)
    if u <= -knee: return x
    t = (u + knee) / knee                       # 0 at the knee, 1 at the clip
    return 2 ** (-knee + knee * (t - t * t / 2))

# Checks. Per bin, all pairs: the curve against the data.
be = sorted(abs(resid(P, a, b, d)) for rows, d in groups for a, b in rows)
print('check per bin: median %.3f EV, 90%% within %.3f EV' % (be[len(be) // 2], be[int(len(be) * 0.9)]))
w = min([v for v, l in ((v, [ph[1] for pl, ph in zip(T[-1][1], T[-1][0]) if pl[1] == v]) for v in range(150, 256)) if len(l) >= 40 and sorted(l)[len(l) // 2] >= 254] or [ceil1])
print('Canon JPEG white: J = %d at -1 EV, %.2f EV below the sensor clip' % (w, -math.log2(X_of(w))))

gain = 0.18 * 2 ** 3.5; bb = 10 ** ((95 - 685) * 0.002 / 0.6)
def cineon(l): return min(1.0, max(0.0, (685 + 300 * math.log10(max(l, 0) * (1 - bb) + bb)) / 1023))
for x in (0.25, 1 / 2 ** knee, 0.5, 0.71, 1.0):
    print('  scene %.3f: Cineon code %.0f (without shoulder %.0f)' % (x, cineon(shoulder(x) * gain) * 1023, cineon(x * gain) * 1023))
def C(J):
    """Target: Cineon code (0..1) of a -1 EV render value J."""
    return cineon((shoulder(X_of(J)) if J > 0 else 0.0) * gain)

# Normal render (XPAN_RENDER0=1, README "XPan Cineon from Canon's curve"):
# the same fitted curve for Canon's render at its own gains, the M50's
# replacement for the EOS M's CINLV. A gain-0 value J is the scene of the
# -1 EV render's J at half the light: X = X_raw(J) / 2. Canon's normal white
# sits at W0 (the -1 EV render's J = w above: 0.35 EV under the sensor
# clip), so the curve ends there and the shoulder ends at W0, not at 1.
import os
RENDER0 = os.environ.get('XPAN_RENDER0') == '1'
W0 = X_of(w)
if RENDER0:
    def C(J):
        if J <= 0: return cineon(0.0)
        return cineon(W0 * shoulder(min(1.0, X_raw(J) / 2 / W0)) * gain)
    print('normal render: Canon white at %.3f (%.2f EV under the sensor clip); Cineon top %.0f; J 32/64/128/192/255 -> %s' % (
        W0, -math.log2(W0), C(255) * 1023, ' '.join('%.0f' % (C(j) * 1023) for j in (32, 64, 128, 192, 255))))
    # The camera builds this base itself (src/xpan-m50.c m50_cin0_build),
    # with the user's Shoulder knee / slope: it needs only the scene value
    # of Canon's 17 grey nodes J = 0, 255/16, ... 255, relative to Canon's
    # white (xpan_m50_bcurve.py --cin0 makes the node table from them).
    with open(out_path, 'w') as nf:
        nf.write('# xpan_m50_cin1ev.py XPAN_RENDER0=1: Canon white %.6f of the sensor clip; scene / white at J = j x 255/16\n' % W0)
        nf.write('%.6f\n' % W0)
        for j in range(17):
            J = j * 255 / 16
            nf.write('%.6f\n' % (min(1.0, X_raw(J) / 2 / W0) if J > 0 else 0.0))
    print('written %s (17 grey nodes)' % out_path)
    sys.exit(0)

# Table-step compensation (camera test IMG_2754-2761: lower mid-tones about
# 12 % too steep). The camera applies the base only at Canon's colour table
# nodes; on the grey axis Canon's Neutral nodes sit at J = 0, 32, ... 255
# (RAM dump, Neutral table 0x69A820), and the table draws straight lines
# between them. So only the base's values at those J matter, and they are
# chosen by least squares so the straight lines follow the target curve
# (uniform weight over J), black fixed. The base then is the target plus a
# smooth correction through those node offsets (also used by the other
# table nodes, whose outputs lie between).
NODES = [0, 32, 64, 96, 128, 159, 191, 223, 255]
nn = len(NODES)
def seg(J):
    for k in range(nn - 1):
        if J <= NODES[k + 1]: return k, (J - NODES[k]) / (NODES[k + 1] - NODES[k])
    return nn - 2, 1.0
# weight: how often each value occurs in the -1 EV calibration renders (all
# channels, all exposures), plus a floor so no part of the range is ignored
HIST = [0] * 256
for t in T:
    for p in t[1]:
        for c in range(3): HIST[p[c]] += 1
hmax = max(HIST)
AtA = [[0.0] * nn for _ in range(nn)]; Atb = [0.0] * nn
for s in range(0, 255 * 4 + 1):
    J = s / 4; k, f = seg(J); y = C(J)
    wgt = math.sqrt(HIST[min(255, int(J))] / hmax + 0.02)
    r = [0.0] * nn; r[k] += (1 - f) * wgt; r[k + 1] += f * wgt; y *= wgt
    for i in range(nn):
        if r[i] == 0: continue
        Atb[i] += r[i] * y
        for j in range(nn): AtA[i][j] += r[i] * r[j]
AtA[0] = [1e6] + [0.0] * (nn - 1); Atb[0] = 1e6 * C(0)          # black stays black
Mx = [AtA[i][:] + [Atb[i]] for i in range(nn)]
for i in range(nn):
    p = max(range(i, nn), key=lambda k: abs(Mx[k][i])); Mx[i], Mx[p] = Mx[p], Mx[i]
    for k in range(nn):
        if k != i:
            fct = Mx[k][i] / Mx[i][i]
            for j in range(i, nn + 1): Mx[k][j] -= fct * Mx[i][j]
V = [Mx[i][nn] / Mx[i][i] for i in range(nn)]
DELTA = [V[k] - C(NODES[k]) for k in range(nn)]
def comp(J):
    k, f = seg(min(max(J, 0), 255)); return DELTA[k] * (1 - f) + DELTA[k + 1] * f
def table(vals, J):
    k, f = seg(J); return vals[k] * (1 - f) + vals[k + 1] * f
plain = [C(j) for j in NODES]
print('node offsets (Cineon codes):', ' '.join('%d:%+.1f' % (j, d * 1023) for j, d in zip(NODES, DELTA)))
for name, vals in (('without compensation', plain), ('with compensation', V)):
    errs = [abs(table(vals, s / 4) - C(s / 4)) * 1023 for s in range(0, 1021)]
    werr = sum(abs(table(vals, j) - C(j)) * 1023 * HIST[j] for j in range(256)) / sum(HIST)
    seg1 = max(abs(table(vals, s / 4) - C(s / 4)) * 1023 for s in range(4, 129))
    print('  through the table, %s: mean error %.1f codes, max %.1f; per photo pixel %.1f; worst in J 1..32: %.0f' % (name, sum(errs) / len(errs), max(errs), werr, seg1))
# The camera test's exposure check, simulated (grey, 2.04 EV, levels = code / 4)
def Jrender(x):
    lo, hi = 0.0, 255.0
    for _ in range(40):
        m = (lo + hi) / 2
        if X_raw(m) < x: lo = m
        else: hi = m
    return lo
for name, fn in (('exact curve', C), ('table, without', lambda J: table(plain, J)), ('table, with', lambda J: table(V, J))):
    row = []
    for lvl in (45, 57, 69, 81, 99, 117, 135, 153, 171):
        J = min(range(0, 256), key=lambda j: abs(fn(j) * 1023 / 4 - lvl))
        Jh = min(Jrender(X_raw(J) * 2 ** 2.04), 255)
        row.append('%d:%+.0f' % (lvl, (fn(Jh) - fn(J)) * 1023 / 4))
    print('  exposure check simulated (ideal +46), %-16s %s' % (name + ':', ' '.join(row)))

# The compensation is only reported, not applied (2026-11-10): weighted by
# the photos' own pixels it cuts the error from 23 to 21 codes per pixel and
# makes other tones worse; the error sits inside Canon's first table segment
# (J 0..32), which no node value can bend. XPAN_STEPCOMP=1 applies it anyway.
STEP = os.environ.get('XPAN_STEPCOMP') == '1'
N = 17
lines = ['TITLE "XPan M50 Cineon log base, %s (Canon Neutral tone curve inverted, fitted on IMG_2736-2747; shoulder from %.1f EV below the %s%s)"' % (
             "Canon's own gains" if RENDER0 else 'highlight headroom -1 EV', knee, "render's white" if RENDER0 else 'clip', '; table-step compensated' if STEP else ''),
         'LUT_3D_SIZE %d' % N, 'DOMAIN_MIN 0 0 0', 'DOMAIN_MAX 1 1 1']
for bi in range(N):
    for gi in range(N):
        for ri in range(N):
            J = [ri * 255 / (N - 1), gi * 255 / (N - 1), bi * 255 / (N - 1)]
            black = all(j < 0.5 for j in J)
            lines.append('%.6f %.6f %.6f' % tuple(min(1.0, max(0.0, (C(0) if black else C(j)) + (comp(j) if STEP else 0.0))) for j in J))
open(out_path, 'w').write('\n'.join(lines) + '\n')
print('written %s (%d-point)' % (out_path, N))
