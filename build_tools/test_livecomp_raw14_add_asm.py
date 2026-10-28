#!/usr/bin/env python3
"""LCM65: the hand-written ARM Live Bulb kernels (raw14-add-asm.h) checked
bit-exact against the C word kernels and a reference, as the ARMv5TE
machine code the camera runs (compiled with the module toolchain, executed
by build_tools/livecomp_armemu.py). Also reports instructions and a rough
ARM946 cycle estimate per 2-block pair next to the C kernels.
Skipped (with a message) if arm-none-eabi-gcc is not found."""
from pathlib import Path
import random, subprocess, sys, tempfile
sys.path.insert(0, str(Path(__file__).resolve().parent))
from livecomp_armemu import CPU
from test_livecomp_raw14_asm import tool, pack, unpack, MOD, BASE, SIZE

def ref(a, b, bl, white, sub):
    w = min(white, 16383); out = []
    for x, y in zip(a, b):
        if x >= w: out.append(x); continue
        v = x - y + bl if sub else x + y - bl
        out.append(0 if v < 0 else w if v > w else v)
    return out

def data(kind, n, rnd, bl, w):
    A = bytearray(); B = bytearray()
    for _ in range(2*n):
        if kind == 'random': a = [rnd.randrange(16384) for _ in range(8)]; b = [rnd.randrange(16384) for _ in range(8)]
        elif kind == 'noise': a = [bl+rnd.randrange(-80, 300) for _ in range(8)]; b = [bl+rnd.randrange(-80, 80) for _ in range(8)]
        elif kind == 'edges':
            vals = [0, 1, bl-1, bl, bl+1, w-1, w, w+1, 16382, 16383]
            vals = [min(max(v, 0), 16383) for v in vals]
            a = [rnd.choice(vals) for _ in range(8)]; b = [rnd.choice(vals) for _ in range(8)]
        else: a = [16383 if rnd.random() < .5 else w for _ in range(8)]; b = [bl+rnd.randrange(-80, 80) for _ in range(8)]
        A += pack(a); B += pack(b)
    return A, B

def main():
    gcc, ld, objcopy, nm = (tool('arm-none-eabi-'+t) for t in ('gcc', 'ld', 'objcopy', 'nm'))
    if not all((gcc, ld, objcopy, nm)):
        print('ARM Bulb kernel check skipped (arm-none-eabi-gcc not found)'); return
    with tempfile.TemporaryDirectory() as t:
        t = Path(t)
        (t/'k.c').write_text('#include <stdint.h>\n#include "raw14-add.h"\n#include "raw14-add-asm.h"\n'
            'typedef void (*k_t)(uint32_t*,const uint32_t*,unsigned,unsigned,unsigned);\n'
            'k_t volatile k1=lc_raw14_add_sel,k2=lc_raw14_add_ldm,k3=lc_raw14_sub_sel,k4=lc_raw14_sub_ldm;\n')
        # Same code generation as the module (-Os, ARMv5TE, ARM state).
        subprocess.run([gcc, '-Os', '-march=armv5te', '-marm', '-fomit-frame-pointer', '-D__ARM__', '-I', str(MOD),
                        '-c', str(t/'k.c'), '-o', str(t/'k.o')], check=True)
        subprocess.run([ld, f'-Ttext=0x{BASE:x}', '-e', f'0x{BASE:x}', str(t/'k.o'), '-o', str(t/'k.elf')], check=True)
        subprocess.run([objcopy, '-O', 'binary', '-j', '.text', str(t/'k.elf'), str(t/'k.bin')], check=True)
        code = (t/'k.bin').read_bytes()
        syms = {l.split()[2]: int(l.split()[0], 16) for l in subprocess.check_output([nm, str(t/'k.elf')]).decode().splitlines() if len(l.split()) == 3}
    rnd = random.Random(65)
    for op, names in (('add', ['lc_raw14_add_sel', 'lc_raw14_add_ldm']), ('sub', ['lc_raw14_sub_sel', 'lc_raw14_sub_ldm'])):
        for bl, white in ((2048, 15000), (2047, 16383), (1792, 20000), (0, 0), (4000, 9000)):
            for kind in ['random', 'noise', 'edges', 'saturated']:
                n = 129
                A, B = data(kind, n, rnd, bl, min(white, 16383))
                R = bytearray(A)
                for o in range(0, len(A), 14):
                    R[o:o+14] = pack(ref(unpack(A[o:o+14]), unpack(B[o:o+14]), bl, white, op == 'sub'))
                line = []
                for fn in names:
                    mem = bytearray(SIZE); mem[:len(code)] = code
                    da, db, sp = 0x100000, 0x180000, BASE+SIZE-16
                    mem[da-BASE:da-BASE+len(A)] = A; mem[db-BASE:db-BASE+len(B)] = B
                    mem[sp-BASE:sp-BASE+4] = white.to_bytes(4, 'little')         # 5th argument
                    guard = bytes(mem[da-BASE+len(A):da-BASE+len(A)+64])
                    c = CPU(mem, BASE); c.r[0], c.r[1], c.r[2], c.r[3], c.r[13] = da, db, n, bl, sp
                    for k in range(4, 12): c.r[k] = 0x1000+k                    # callee-saved
                    c.run(syms[fn])
                    assert bytes(mem[da-BASE:da-BASE+len(A)]) == bytes(R), (fn, kind, bl, white, 'not exact')
                    assert bytes(mem[da-BASE+len(A):da-BASE+len(A)+64]) == guard, (fn, 'wrote past the end')
                    assert c.r[13] == sp and all(c.r[k] == 0x1000+k for k in range(4, 12)), (fn, 'registers/stack')
                    line.append(f'{fn[13:]} {c.count/n:.0f} instr/{c.cycles/n:.0f} cyc')
                if kind == 'noise' and bl == 2048: print(f'{op} {kind:9s}: ' + ', '.join(line))
        # zero pairs: nothing touched
        mem = bytearray(SIZE); mem[:len(code)] = code
        c = CPU(mem, BASE); c.r[0], c.r[1], c.r[2], c.r[3], c.r[13] = 0x100000, 0x180000, 0, 2048, BASE+SIZE-16
        c.run(syms[names[1]]); assert not any(mem[0x100000-BASE:0x100100-BASE]) and c.r[13] == BASE+SIZE-16
    print('PASS: ARM Live Bulb kernels (add, subtract) bit-exact on ARMv5TE code')

if __name__ == '__main__':
    main()
