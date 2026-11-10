"""Find Thumb-2 BL/BLX callers of given ROM0 addresses (scan E0040000..E1000000)."""
import sys, struct
rom = open(sys.argv[1], 'rb').read()
B = 0xE0000000
targets = set(int(t, 16) & ~1 for t in sys.argv[2:])
lo, hi = 0x40000, min(len(rom), 0x1000000)
for off in range(lo, hi - 4, 2):
    h1, h2 = struct.unpack_from('<HH', rom, off)
    if (h1 & 0xF800) != 0xF000 or (h2 & 0xC000) != 0xC000: continue
    s = (h1 >> 10) & 1
    j1 = (h2 >> 13) & 1; j2 = (h2 >> 11) & 1
    i1 = 1 - (j1 ^ s); i2 = 1 - (j2 ^ s)
    imm = (s << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3FF) << 12) | ((h2 & 0x7FF) << 1)
    if s: imm -= 1 << 25
    a = B + off
    t = a + 4 + imm
    if not (h2 & 0x1000): t &= ~3   # BLX to ARM
    if t in targets: print('%08X -> %08X' % (a, t))
