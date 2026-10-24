#!/usr/bin/env python3
"""Exercise the actual packed Bayer max, geometry gates and capture-slot policy.

Does not emulate sensor readout or prove camera DMA/preview behavior.
"""
import ctypes
import os
from pathlib import Path
import random
import struct
import subprocess
import tempfile
from livecomp_test_utils import function

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / 'modules/livecomp'


def pack(pixels):
    # Independent canonical bit-stream encoder, not the C word expressions.
    value = 0
    for pixel in pixels:
        value = (value << 14) | pixel
    return struct.pack('<7H', *((value >> shift) & 65535 for shift in range(96, -1, -16)))


def main():
    source = r'''
#include <assert.h>
#include "raw14-max.h"
#include "lv-ring.h"
void merge(uint16_t *a,const uint16_t *b,unsigned n) { lc_raw14_max(a,b,n); }
struct raw_info {
    int bits_per_pixel,width,height,pitch,frame_size;
    struct { int x1,y1,x2,y2; } active_area;
};
'''
    source += function((MOD/'livecomp-lv.c').read_text(), 'geometry_valid(')
    source += r'''
void check(void) {
    struct raw_info info={14,1808,727,3164,2300228,{72,28,1808,724}};
    assert(geometry_valid(&info));
    info.width=5280;assert(!geometry_valid(&info));info.width=1808;
    info.bits_per_pixel=12;assert(!geometry_valid(&info));info.bits_per_pixel=14;
    info.frame_size--;assert(!geometry_valid(&info));info.frame_size++;
    info.active_area.x2=1809;assert(!geometry_valid(&info));info.active_area.x2=1808;
    info.pitch++;assert(!geometry_valid(&info));info.pitch--;
    struct lc_ring r;lc_ring_init(&r);
    assert(lc_ring_take(&r)==-1);
    int a=lc_ring_arm(&r);assert(a>=0 && r.sequence[a]==1);
    assert(lc_ring_take(&r)==-1 && lc_ring_arm(&r)==-1);
    assert(!lc_ring_release(&r,a)); /* DMA slot cannot be freed by consumer */
    assert(lc_ring_complete(&r)==a);
    int b=lc_ring_arm(&r);assert(b!=a);
    assert(lc_ring_take(&r)==a);
    assert(lc_ring_complete(&r)==b);
    int c=lc_ring_arm(&r);assert(c!=a && c!=b);
    assert(lc_ring_complete(&r)==c);
    assert(lc_ring_arm(&r)==-1 && r.issued==3); /* no overwrite under load */
    assert(lc_ring_release(&r,a));
    int d=lc_ring_arm(&r);assert(d==a && r.sequence[d]==4);
    assert(lc_ring_take(&r)==b);assert(lc_ring_release(&r,b));
    assert(lc_ring_take(&r)==c);assert(lc_ring_release(&r,c));
    assert(lc_ring_take(&r)==-1); /* Stop with one input still in flight */
    assert(lc_ring_complete(&r)==d);
    assert(lc_ring_take(&r)==d);assert(lc_ring_release(&r,d));
    assert(lc_ring_take(&r)==-1 && r.pending==-1 && r.issued==4);
    for (unsigned i=5;i<10000;i++) {
        int slot=lc_ring_arm(&r);assert(slot>=0);
        assert(r.sequence[slot]==i);assert(lc_ring_take(&r)==-1);
        assert(lc_ring_complete(&r)==slot);assert(lc_ring_take(&r)==slot);
        assert(lc_ring_release(&r,slot));
    }
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        (base/'check.c').write_text(source)
        lib = base/'check.so'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-O2', '-Wall', '-Wextra',
                        '-Werror', '-shared', '-fPIC', '-I'+str(MOD),
                        str(base/'check.c'), '-o', str(lib)], check=True)
        native = ctypes.CDLL(str(lib))
        native.check()
        native.merge.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint]
        rng = random.Random(20261023)
        inputs = [([0]*8, [16383]*8), ([16383]*8, [0]*8),
                  ([0, 1, 3, 15, 63, 255, 1023, 4095], [4096, 1024, 256, 64, 16, 4, 2, 1])]
        inputs += [([rng.randrange(16384) for _ in range(8)],
                    [rng.randrange(16384) for _ in range(8)]) for _ in range(8192)]
        a = b''.join(pack(x) for x, _ in inputs)
        b = b''.join(pack(y) for _, y in inputs)
        expected = b''.join(pack([max(x, y) for x, y in zip(p, q)]) for p, q in inputs)
        guard = bytes([0xA5])*16
        dest = ctypes.create_string_buffer(guard+a+guard)
        src = ctypes.create_string_buffer(guard+b+guard)
        native.merge(ctypes.addressof(dest)+16, ctypes.addressof(src)+16, len(inputs))
        assert dest.raw[:16] == dest.raw[-17:-1] == guard
        assert dest.raw[16:-17] == expected
        assert src.raw == guard+b+guard+b'\0'
        native.merge(ctypes.addressof(dest)+16, ctypes.addressof(dest)+16, len(inputs))
        assert dest.raw[16:-17] == expected
    print(f'PASS: {len(inputs)*8} independent RAW14 max samples, guards, geometry, queue ownership and Stop drain')


if __name__ == '__main__':
    main()
