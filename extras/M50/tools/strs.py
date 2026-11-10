"""List debug strings referenced (adr / ldr literal / adr.w) by code in a ROM0 range."""
import sys, struct
rom = open(sys.argv[1], 'rb').read(); B = 0xE0000000
lo, hi = int(sys.argv[2], 16), int(sys.argv[3], 16)
h = lambda a: struct.unpack_from('<H', rom, a - B)[0]
w = lambda a: struct.unpack_from('<I', rom, a - B)[0]
def s(a):
    if not (B <= a < B + len(rom)): return None
    o = a - B; e = rom.find(b'\0', o)
    if e < 0 or e - o < 4 or e - o > 140: return None
    t = rom[o:e]
    if not all(32 <= c < 127 for c in t): return None
    return t.decode()
for a in range(lo, hi, 2):
    x = h(a); t = None
    if (x & 0xF800) == 0xA000: t = ((a + 4) & ~3) + (x & 0xFF) * 4
    elif (x & 0xF800) == 0x4800: t = w(((a + 4) & ~3) + (x & 0xFF) * 4)
    elif (x & 0xFBFF) in (0xF20F, 0xF2AF):
        y = h(a + 2)
        if not (y & 0x8000):
            imm = ((x >> 10) & 1) << 11 | ((y >> 12) & 7) << 8 | (y & 0xFF)
            base = (a + 4) & ~3
            t = base + imm if (x & 0xFBFF) == 0xF20F else base - imm
    if t is not None:
        v = s(t)
        if v: print('%08X "%s"' % (a, v))
