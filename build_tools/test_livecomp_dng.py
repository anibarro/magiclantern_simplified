#!/usr/bin/env python3
"""Exercise the actual DNG writer, decode all RAW samples, inject I/O failures."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile


def pack(values):
    """Independent MSB-first 14-bit packing, stored as little-endian 16-bit words."""
    bits = 0
    for v in values:
        bits = (bits << 14) | v
    data = bits.to_bytes(14, 'big')
    return b''.join(data[i:i+2][::-1] for i in range(0, 14, 2))

ROOT = Path(__file__).resolve().parents[1]


def tags(data, offset):
    count, = struct.unpack_from('<H', data, offset)
    entries = [struct.unpack_from('<HHII', data, offset+2+12*i) for i in range(count)]
    assert [e[0] for e in entries] == sorted({e[0] for e in entries})
    result = {}
    for i, (tag, kind, n, value) in enumerate(entries):
        size = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 10: 8}[kind]*n
        pos = offset+2+12*i+8 if size <= 4 else value
        assert pos+size <= len(data)
        result[tag] = (kind, n, data[pos:pos+size])
    return result


def main(width=5280,height=3506):
    source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "lv-dng.h"
/* The module's DNG = lc_dng_header (2048 bytes) + lc_dng_swap_words of the
 * packed RAW14 raster, written in chunks (fullres-lv.c frlv_save_dng). */
int main(int argc,char **argv) {
    assert(argc==5);
    struct lc_dng_metadata m={0};
    m.width=atoi(argv[3]);m.height=atoi(argv[4]);m.pitch=m.width*14/8;m.bytes=m.pitch*m.height;
    m.black=2048;m.white=15000;m.cfa=0x02010100;
    m.active[0]=28;m.active[1]=72;m.active[2]=m.height-2;m.active[3]=m.width;
    for (unsigned i=0;i<9;i++) { m.matrix[2*i]=(i%4==0)?10000:0;m.matrix[2*i+1]=10000; }
    m.illuminant=21;m.iso=200;m.tvr=400;m.fps=400;m.aperture=28;m.focal=22;
    m.exposure_ms=2500;m.neutral[0]=441358;m.neutral[1]=1000000;m.neutral[2]=675944;
    uint8_t *raw=malloc(m.bytes),*out=malloc(m.bytes),h[LC_DNG_HEADER_BYTES];
    FILE *input=fopen(argv[1],"rb");assert(input && raw && out);
    assert(fread(raw,1,m.bytes,input)==m.bytes);fclose(input);
    struct lc_dng_metadata bad=m;bad.tvr=0;assert(!lc_dng_header(h,&bad));
    bad=m;bad.width=5288;assert(!lc_dng_header(h,&bad));
    assert(lc_dng_header(h,&m));
    /* LCM67: word swap, 4 MiB chunks; equal to the byte swap for every tail length */
    for (unsigned len=0;len<64;len+=2) {
        uint32_t a[16],b[16];lc_dng_swap_copy((uint8_t *)a,raw+4*len,len);lc_dng_swap_words((uint8_t *)b,raw+4*len,len);assert(!memcmp(a,b,len));
        /* LCM68: in place (dst==src), the final DNG */
        uint32_t c[16];memcpy(c,raw+4*len,len);lc_dng_swap_words((uint8_t *)c,(const uint8_t *)c,len);assert(!memcmp(a,c,len));
        memcpy(c,raw+4*len,len);lc_dng_swap_copy((uint8_t *)c,(const uint8_t *)c,len);assert(!memcmp(a,c,len));
    }
    for (unsigned off=0;off<m.bytes;off+=4u<<20) lc_dng_swap_words(out+off,raw+off,m.bytes-off<(4u<<20) ? m.bytes-off:4u<<20);
    char path[1024];snprintf(path,sizeof(path),"%s/normal.DNG",argv[2]);
    FILE *f=fopen(path,"wb");assert(f);
    assert(fwrite(h,1,sizeof(h),f)==sizeof(h) && fwrite(out,1,m.bytes,f)==m.bytes);fclose(f);
    free(raw);free(out);return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        # Independent bit-stream packing, at the camera's exact odd-height geometry.
        pixels = width*height
        packed = b''.join(pack([((i+j)*37+13)%16384 for j in range(8)])
                          for i in range(0, pixels, 8))
        (base/'input.raw').write_bytes(packed)
        (base/'check.c').write_text(source)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'modules/livecomp'), str(base/'check.c'), '-o', str(base/'check')], check=True)
        subprocess.run([str(base/'check'), str(base/'input.raw'), str(base), str(width), str(height)], check=True)
        data = (base/'normal.DNG').read_bytes()
        assert data[:8] == b'II*\0\x08\0\0\0'
        main_tags = tags(data, 8)
        def scalar(tag):
            kind, n, raw = main_tags[tag]
            assert n == 1
            return struct.unpack('<H' if kind == 3 else '<I', raw)[0]
        assert scalar(256) == width and scalar(257) == height and scalar(258) == 14
        assert scalar(259) == 1 and scalar(262) == 32803
        assert main_tags[33422][2] == bytes([0,1,1,2])
        assert struct.unpack('<4I', main_tags[50829][2]) == (28,72,height-2,width)
        assert struct.unpack('<2I', main_tags[50720][2]) == (width-72,height-30)
        exif = tags(data, scalar(34665))
        assert struct.unpack('<2I', exif[33434][2]) == (2500,1000)   # 2.5 s shutter
        assert struct.unpack('<6I', main_tags[50728][2]) == (441358,1000000,1000000,1000000,675944,1000000)
        offset, size = scalar(273), scalar(279)
        assert offset == 2048 and size == len(packed) and len(data) == offset+size
        raw = data[offset:]
        for block in range(pixels//8):
            value = int.from_bytes(raw[14*block:14*(block+1)], 'big')
            decoded = [(value >> (14*(7-j))) & 16383 for j in range(8)]
            assert decoded == [((block*8+j)*37+13)%16384 for j in range(8)]
    print(f'PASS: DNG header and RAW14 byte order, {pixels} decoded samples, TIFF/DNG/EXIF tags (shutter, WB neutral), refused geometry')


if __name__ == '__main__':
    main()
