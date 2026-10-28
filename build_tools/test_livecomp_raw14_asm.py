#!/usr/bin/env python3
"""LCB2: the hand-written ARM select-max kernels (raw14-max-asm.h) checked
bit-exact against the reference maximum, as the ARMv5TE machine code the
camera runs (compiled with the module toolchain, executed by a small ARM
interpreter, build_tools/livecomp_armemu.py). Also reports instructions and
a rough ARM946 cycle estimate per 2-block pair next to the C kernel.
Skipped (with a message) if arm-none-eabi-gcc is not found."""
from pathlib import Path
import os, random, shutil, struct, subprocess, sys, tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
from livecomp_armemu import CPU

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT/'modules/livecomp'
BASE, SIZE = 0x10000, 0x200000

def tool(name):
    for d in [os.environ.get('ARM_BINPATH', ''), '']:
        p = shutil.which(name, path=d or None)
        if p: return p
    return None

def pack(v):
    bits = 0
    for x in v: bits = (bits << 14) | x
    return struct.pack('<7H', *[(bits >> (16*(6-k))) & 0xffff for k in range(7)])
def unpack(b):
    bits = 0
    for h in struct.unpack('<7H', b): bits = (bits << 16) | h
    return [(bits >> (14*(7-k))) & 0x3fff for k in range(8)]

def data(kind, n, rnd):
    A = bytearray(); B = bytearray()
    for _ in range(2*n):
        if kind == 'random': a = [rnd.randrange(16384) for _ in range(8)]; b = [rnd.randrange(16384) for _ in range(8)]
        elif kind == 'noise': a = [2047+rnd.randrange(-64, 64) for _ in range(8)]; b = [2047+rnd.randrange(-64, 64) for _ in range(8)]
        elif kind == 'ties':
            vals = [0, 1, 8191, 8192, 16383, 2047]
            a = [rnd.choice(vals) for _ in range(8)]
            b = [x if rnd.random() < .5 else rnd.choice(vals+[x ^ 1, x ^ (1 << 13)]) for x in a]
        elif kind == 'steady': a = [2147+rnd.randrange(-64, 64) for _ in range(8)]; b = [2047+rnd.randrange(-64, 64) for _ in range(8)]
        else: a = [16383]*8; b = [2047+rnd.randrange(-64, 64) for _ in range(8)]
        A += pack(a); B += pack(b)
    return A, B

def main():
    gcc, ld, objcopy, nm = (tool('arm-none-eabi-'+t) for t in ('gcc', 'ld', 'objcopy', 'nm'))
    if not all((gcc, ld, objcopy, nm)):
        print('ARM kernel check skipped (arm-none-eabi-gcc not found)'); return
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        (t/'k.c').write_text('#include <stdint.h>\n#include "raw14-max.h"\n#include "raw14-max-asm.h"\n'
            'unsigned (*volatile k1)(uint32_t*,const uint32_t*,unsigned)=lc_raw14_max_sel;\n'
            'unsigned (*volatile k2)(uint32_t*,const uint32_t*,unsigned)=lc_raw14_max_swar;\n'
            'unsigned (*volatile k3)(uint32_t*,const uint32_t*,unsigned)=lc_raw14_max_field;\n')
        # Same code generation as the module (-Os, ARMv5TE, ARM state).
        subprocess.run([gcc, '-Os', '-march=armv5te', '-marm', '-fomit-frame-pointer', '-D__ARM__', '-I', str(MOD),
                        '-c', str(t/'k.c'), '-o', str(t/'k.o')], check=True)
        subprocess.run([ld, f'-Ttext=0x{BASE:x}', '-e', f'0x{BASE:x}', str(t/'k.o'), '-o', str(t/'k.elf')], check=True)
        subprocess.run([objcopy, '-O', 'binary', '-j', '.text', str(t/'k.elf'), str(t/'k.bin')], check=True)
        code = (t/'k.bin').read_bytes()
        syms = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.check_output([nm, str(t/'k.elf')]).decode().splitlines() if len(l.split()) == 3}
    rnd = random.Random(14)
    names = ['lc_raw14_max_sel', 'lc_raw14_max_swar', 'lc_raw14_max_field']
    for kind in ['random', 'noise', 'ties', 'steady', 'none']:
        n = 257
        A, B = data(kind, n, rnd)
        R = bytearray(A)
        for o in range(0, len(A), 14):
            R[o:o+14] = pack([max(x, y) for x, y in zip(unpack(A[o:o+14]), unpack(B[o:o+14]))])
        line = []; changed = set()
        for fn in names:
            mem = bytearray(SIZE); mem[:len(code)] = code
            da, db = 0x100000, 0x180000
            mem[da-BASE:da-BASE+len(A)] = A; mem[db-BASE:db-BASE+len(B)] = B
            c = CPU(mem, BASE); c.r[0], c.r[1], c.r[2], c.r[13] = da, db, n, BASE+SIZE-16
            c.run(syms[fn])
            assert bytes(mem[da-BASE:da-BASE+len(A)]) == bytes(R), (fn, kind, 'not exact')
            changed.add(c.r[0])
            line.append(f'{fn[13:]} {c.count/n:.0f} instr/{c.cycles/n:.0f} cyc')
        assert len(changed) == 1, (kind, 'changed counts differ', changed)
        print(f'{kind:7s} changed {changed.pop():3d}/{n}: ' + ', '.join(line))
    # zero pairs: returns 0 and touches nothing
    print('PASS: ARM select-max kernels (SWAR, per field) bit-exact on ARMv5TE code')

if __name__ == '__main__':
    main()
