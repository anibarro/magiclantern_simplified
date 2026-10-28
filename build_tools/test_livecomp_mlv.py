#!/usr/bin/env python3
"""LCM67: the one-frame MLV writer (lv-mlv.h) checked against ML's own
modules/raw_video/mlv_rec/mlv.h: block order and sizes, every field read
back through the mlv.h structs, the 4 KiB frame offset, and all RAW samples
decoded from the VIDF payload (Canon byte order, as mlv_dump reads it)."""
from pathlib import Path
import os
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def pack(values):
    """Independent MSB-first 14-bit packing, stored as little-endian 16-bit words."""
    bits = 0
    for v in values:
        bits = (bits << 14) | v
    data = bits.to_bytes(14, 'big')
    return b''.join(data[i:i+2][::-1] for i in range(0, 14, 2))


def raw_info_host():
    """raw_info_t exactly as src/raw.h gives it to host tools (64-bit)."""
    s = (ROOT/'src/raw.h').read_text()
    m = re.search(r'#if INTPTR_MAX != INT32_MAX\n(typedef struct \{.*?\} raw_info_t;)\n#else', s, re.S)
    assert m
    return m.group(1)


SOURCE = r'''
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
RAW_INFO_T
#include "mlv.h"
#include "lv-mlv.h"
#define CHECK(T,n) assert(sizeof(T)==(n))
int main(int argc,char **argv) {
    assert(argc==5);
    CHECK(mlv_file_hdr_t,52);CHECK(mlv_rawi_hdr_t,180);CHECK(mlv_idnt_hdr_t,84);CHECK(mlv_expo_hdr_t,40);
    CHECK(mlv_lens_hdr_t,96);CHECK(mlv_rtci_hdr_t,44);CHECK(mlv_vidf_hdr_t,32);CHECK(raw_info_t,0xa0);
    struct lc_dng_metadata m={0};
    m.width=atoi(argv[3]);m.height=atoi(argv[4]);m.pitch=m.width*14/8;m.bytes=m.pitch*m.height;
    m.black=2048;m.white=15000;m.cfa=0x02010100;
    m.active[0]=28;m.active[1]=72;m.active[2]=m.height;m.active[3]=m.width;
    for (unsigned i=0;i<9;i++) { m.matrix[2*i]=(int32_t)(i*1000)-3000;m.matrix[2*i+1]=10000; }
    m.illuminant=21;m.iso=1600;m.tvr=400;m.fps=400;m.aperture=20;m.focal=22;
    m.exposure_ms=25000;m.raw_api=1;m.dynamic_range=1100;
    struct lc_mlv_extra x={0x0123456789ABCDEFull,0x80000331u,"EF-M22mm f/2 STM",{5,4,23,28,9,126,3,300,0}};
    static uint8_t h[LC_MLV_HEADER_BYTES];
    struct lc_dng_metadata bad=m;bad.pitch++;assert(!lc_mlv_header(h,&bad,&x));
    bad=m;bad.tvr=0;assert(!lc_mlv_header(h,&bad,&x));
    assert(lc_mlv_header(h,&m,&x)==LC_MLV_HEADER_BYTES);
    uint8_t *raw=malloc(m.bytes);FILE *in=fopen(argv[1],"rb");assert(raw && in && fread(raw,1,m.bytes,in)==m.bytes);fclose(in);
    char path[1024];snprintf(path,sizeof(path),"%s/out.MLV",argv[2]);
    FILE *f=fopen(path,"wb");assert(f && fwrite(h,1,sizeof(h),f)==sizeof(h) && fwrite(raw,1,m.bytes,f)==m.bytes);fclose(f);
    /* read back through the mlv.h structs */
    const uint8_t *p=h;
    mlv_file_hdr_t fh;memcpy(&fh,p,sizeof(fh));p+=fh.blockSize;
    printf("MLVI %.4s %u %s %llx %u %u %u %u %u %u %u %u %u\n",(char *)fh.fileMagic,fh.blockSize,(char *)fh.versionString,
        (unsigned long long)fh.fileGuid,fh.fileNum,fh.fileCount,fh.fileFlags,fh.videoClass,fh.audioClass,
        fh.videoFrameCount,fh.audioFrameCount,fh.sourceFpsNom,fh.sourceFpsDenom);
    mlv_rawi_hdr_t ri;memcpy(&ri,p,sizeof(ri));p+=ri.blockSize;const raw_info_t *r=&ri.raw_info;
    printf("RAWI %.4s %u %u %u | %u %d %d %d %d %d %d %d | %d %d %d %d | %d %d %d %d | %d %d %x %d %d | %d %d %d\n",(char *)ri.blockType,ri.blockSize,ri.xRes,ri.yRes,
        r->api_version,r->height,r->width,r->pitch,r->frame_size,r->bits_per_pixel,r->black_level,r->white_level,
        r->jpeg.x,r->jpeg.y,r->jpeg.width,r->jpeg.height,r->active_area.y1,r->active_area.x1,r->active_area.y2,r->active_area.x2,
        r->exposure_bias[0],r->exposure_bias[1],r->cfa_pattern,r->calibration_illuminant1,r->dynamic_range,
        r->color_matrix1[0],r->color_matrix1[1],r->color_matrix1[16]);
    mlv_idnt_hdr_t id;memcpy(&id,p,sizeof(id));p+=id.blockSize;
    printf("IDNT %.4s %u %s %x\n",(char *)id.blockType,id.blockSize,(char *)id.cameraName,id.cameraModel);
    mlv_expo_hdr_t ex;memcpy(&ex,p,sizeof(ex));p+=ex.blockSize;
    printf("EXPO %.4s %u %u %u %u %u %llu\n",(char *)ex.blockType,ex.blockSize,ex.isoMode,ex.isoValue,ex.isoAnalog,ex.digitalGain,(unsigned long long)ex.shutterValue);
    mlv_lens_hdr_t ln;memcpy(&ln,p,sizeof(ln));p+=ln.blockSize;
    printf("LENS %.4s %u %u %u %u %s\n",(char *)ln.blockType,ln.blockSize,ln.focalLength,ln.focalDist,ln.aperture,(char *)ln.lensName);
    mlv_rtci_hdr_t rt;memcpy(&rt,p,sizeof(rt));p+=rt.blockSize;
    printf("RTCI %.4s %u %u %u %u %u %u %u %u %u %u\n",(char *)rt.blockType,rt.blockSize,rt.tm_sec,rt.tm_min,rt.tm_hour,rt.tm_mday,rt.tm_mon,rt.tm_year,rt.tm_wday,rt.tm_yday,rt.tm_isdst);
    mlv_vidf_hdr_t vf;memcpy(&vf,p,sizeof(vf));
    printf("VIDF %.4s %u %u %u %u\n",(char *)vf.blockType,vf.blockSize,vf.frameNumber,vf.frameSpace,(unsigned)(p-h));
    free(raw);return 0;
}
'''


def main(width=5280, height=3506):
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        pixels = width*height
        packed = b''.join(pack([((i+j)*53+7) % 16384 for j in range(8)]) for i in range(0, pixels, 8))
        (base/'input.raw').write_bytes(packed)
        (base/'check.c').write_text(SOURCE.replace('RAW_INFO_T', raw_info_host()))
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-I'+str(ROOT/'modules/livecomp'), '-I'+str(ROOT/'modules/raw_video/mlv_rec'),
                        str(base/'check.c'), '-o', str(base/'check')], check=True)
        out = subprocess.run([str(base/'check'), str(base/'input.raw'), str(base), str(width), str(height)],
                             check=True, capture_output=True, text=True).stdout.split('\n')
        f = {l.split()[0]: l.split()[1:] for l in out if l}
        bytes_ = width*14//8*height
        assert f['MLVI'] == ['MLVI', '52', 'v2.0', '123456789abcdef', '0', '1', '4', '1', '0', '1', '0', '400', '1000'], f['MLVI']
        assert ' '.join(f['RAWI']) == (f'RAWI 180 {width} {height} | 1 {height} {width} {width*14//8} {bytes_} 14 2048 15000 | '
                                       f'72 28 {width-72} {height-28} | 28 72 {height} {width} | 0 0 2010100 21 1100 | -3000 10000 5000'), f['RAWI']
        assert f['IDNT'] == ['IDNT', '84', 'Canon', 'EOS', 'M', '80000331'], f['IDNT']
        assert f['EXPO'] == ['EXPO', '40', '0', '1600', '1600', '0', '25000000'], f['EXPO']   # 25 s in us
        assert f['LENS'] == ['LENS', '96', '22', '65535', '200', 'EF-M22mm', 'f/2', 'STM'], f['LENS']
        assert f['RTCI'] == ['RTCI', '44', '5', '4', '23', '28', '9', '126', '3', '300', '0'], f['RTCI']
        space = int(f['VIDF'][3])
        assert f['VIDF'][0] == 'VIDF' and int(f['VIDF'][1]) == 32+space+bytes_ and f['VIDF'][2] == '0'
        assert int(f['VIDF'][4])+32+space == 4096, 'frame data must start at 4 KiB'
        data = (base/'out.MLV').read_bytes()
        assert len(data) == 4096+bytes_ and data[4096:] == packed
        # Walk the file like mlv_dump: block by block, sizes add up to the end.
        pos, kinds = 0, []
        while pos < len(data):
            kind, size = data[pos:pos+4].decode(), struct.unpack_from('<I', data, pos+4)[0]
            kinds.append(kind); pos += size
        assert pos == len(data) and kinds == ['MLVI', 'RAWI', 'IDNT', 'EXPO', 'LENS', 'RTCI', 'VIDF'], kinds
        # Decode every sample of the payload (Canon order: 16-bit LE words of the MSB-first stream).
        raw = data[4096:]
        stream = bytes(b for i in range(0, len(raw), 2) for b in (raw[i+1], raw[i]))
        for block in range(0, pixels//8, 997):
            value = int.from_bytes(stream[14*block:14*(block+1)], 'big')
            assert [(value >> (14*(7-j))) & 16383 for j in range(8)] == [((block*8+j)*53+7) % 16384 for j in range(8)]
    print(f'PASS: one-frame MLV matches mlv.h (7 blocks, fields read back), frame at 4 KiB, RAW14 samples decoded')


if __name__ == '__main__':
    main()
