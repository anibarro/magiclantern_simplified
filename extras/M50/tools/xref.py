"""Find Thumb code references (ldr literal, adr, adr.w, movw/movt) to given ROM0 addresses."""
import sys, struct
rom = open(sys.argv[1], 'rb').read()
B = 0xE0000000
targets = set(int(t, 16) for t in sys.argv[2:])
lits = {}
for t in targets:
    p = struct.pack('<I', t); i = rom.find(p)
    while i >= 0:
        if i % 4 == 0: lits[B + i] = t
        i = rom.find(p, i + 4)
movw = {}
for off in range(0, len(rom) - 4, 2):
    a = B + off
    h = struct.unpack_from('<H', rom, off)[0]
    if (h & 0xF800) == 0x4800:                      # ldr rX,[pc,#imm8*4]
        l = ((a + 4) & ~3) + (h & 0xFF) * 4
        if l in lits: print('%08X ldr  -> %08X' % (a, lits[l]))
    elif (h & 0xF800) == 0xA000:                    # adr rX,#imm8*4
        t = ((a + 4) & ~3) + (h & 0xFF) * 4
        if t in targets: print('%08X adr  -> %08X' % (a, t))
    else:
        h2 = struct.unpack_from('<H', rom, off + 2)[0]
        if h == 0xF8DF or (h & 0xFF7F) == 0xF85F:     # ldr.w rX,[pc,#imm12]
            imm = h2 & 0xFFF
            l = ((a + 4) & ~3) + (imm if (h & 0x80) else -imm)
            if l in lits: print('%08X ldr.w-> %08X' % (a, lits[l]))
        elif (h & 0xFBFF) in (0xF20F, 0xF2AF) and (h2 & 0x8000) == 0:  # adr.w (addw/subw rd,pc,#imm)
            imm = ((h >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 0xFF)
            base = (a + 4) & ~3
            t = base + imm if (h & 0xFBFF) == 0xF20F else base - imm
            if t in targets: print('%08X adr.w-> %08X' % (a, t))
        elif (h & 0xFBF0) in (0xF240, 0xF2C0) and (h2 & 0x8000) == 0:  # movw / movt
            imm = (h & 0xF) << 12 | ((h >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 0xFF)
            rd = (h2 >> 8) & 0xF
            if (h & 0xFBF0) == 0xF240: movw[rd] = (a, imm)
            elif rd in movw and a - movw[rd][0] <= 16:
                t = imm << 16 | movw[rd][1]
                if t in targets: print('%08X movw/t-> %08X' % (movw[rd][0], t))
