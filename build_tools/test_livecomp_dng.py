#!/usr/bin/env python3
"""Exercise the actual DNG writer, decode all RAW samples, inject I/O failures."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
from livecomp_test_utils import function
from test_livecomp_lv import pack

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


def main():
    source = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "lv-dng.h"
#define MIN(a,b) ((a)<(b)?(a):(b))
static int card,ml_shutdown_requested,fail_alloc,fail_create,fail_size,partial;
static long fail_after=-1;
static void *fio_malloc(unsigned n) { return fail_alloc ? NULL:malloc(n); }
static void fio_free(void *p) { free(p); }
static void *get_ml_card(void) { return &card; }
static FILE *FIO_CreateFile(const char *p) { return fail_create ? NULL:fopen(p,"wb"); }
static int FIO_WriteFile(FILE *f,const void *p,unsigned n) {
    long offset=ftell(f);
    if (fail_after>=0 && offset>=fail_after) return -1;
    if (fail_after>=0 && offset+n>fail_after) n=(unsigned)(fail_after-offset);
    if (partial && n>997) n=997;
    return (int)fwrite(p,1,n,f);
}
static void FIO_CloseFile(FILE *f) { assert(!fclose(f)); }
static int FIO_GetFileSize(const char *p,uint32_t *n) {
    struct stat s;if (fail_size || stat(p,&s)) return -1;*n=(uint32_t)s.st_size;return 0;
}
static int raw2iso(int iso) { (void)iso;return 200; }
static struct { int aperture,focal_len; } lens_info={28,22};
struct lv_session {
    struct { int width,height,pitch,black_level,white_level,cfa_pattern;
             int dng_active_area[4],color_matrix1[18],calibration_illuminant1; } info;
    unsigned bytes,frame_iso,iso,tvr,fps,save_stage,save_written,save_size;
    int save_last_return;
    void *card;uint8_t *accumulator;char path[1024];
};
'''
    camera = (ROOT/'modules/livecomp/livecomp-lv.c').read_text()
    source += function(camera, 'save_write_all(')+function(camera, 'save_lv_dng(')
    source += r'''
int main(int argc,char **argv) {
    assert(argc==3);
    struct lv_session s={0};s.info.width=1808;s.info.height=727;s.info.pitch=3164;
    s.bytes=2300228;s.info.black_level=2048;s.info.white_level=15000;s.info.cfa_pattern=0x02010100;
    int area[]={28,72,725,1808};memcpy(s.info.dng_active_area,area,sizeof(area));
    for (unsigned i=0;i<9;i++) { s.info.color_matrix1[2*i]=(i%4==0)?10000:0;s.info.color_matrix1[2*i+1]=10000; }
    s.info.calibration_illuminant1=21;s.frame_iso=72;s.tvr=3030;s.fps=3000;s.card=&card;
    s.accumulator=malloc(s.bytes);uint8_t *original=malloc(s.bytes);assert(s.accumulator && original);
    FILE *input=fopen(argv[1],"rb");assert(input);
    assert(fread(s.accumulator,1,s.bytes,input)==s.bytes);fclose(input);
    memcpy(original,s.accumulator,s.bytes);
    snprintf(s.path,sizeof(s.path),"%s/normal.DNG",argv[2]);assert(save_lv_dng(&s));
    assert(s.save_stage==7 && s.save_written==2048+s.bytes && s.save_size==s.save_written);
    assert(!memcmp(original,s.accumulator,s.bytes));
    partial=1;s.save_written=0;
    snprintf(s.path,sizeof(s.path),"%s/partial.DNG",argv[2]);assert(save_lv_dng(&s));
    assert(s.save_written==2048+s.bytes && !memcmp(original,s.accumulator,s.bytes));partial=0;
    fail_after=3048;s.save_written=0;
    snprintf(s.path,sizeof(s.path),"%s/failed.DNG",argv[2]);assert(!save_lv_dng(&s));
    assert(s.save_stage==5 && s.save_written==3048 && s.save_last_return==-1);fail_after=-1;
    fail_alloc=1;assert(!save_lv_dng(&s) && s.save_stage==2);fail_alloc=0;
    fail_create=1;assert(!save_lv_dng(&s) && s.save_stage==3);fail_create=0;
    s.tvr=0;assert(!save_lv_dng(&s) && s.save_stage==1);s.tvr=3030;
    fail_size=1;s.save_written=0;assert(!save_lv_dng(&s) && s.save_stage==6);fail_size=0;
    ml_shutdown_requested=1;assert(!save_lv_dng(&s) && s.save_stage==4);ml_shutdown_requested=0;
    assert(!memcmp(original,s.accumulator,s.bytes));free(s.accumulator);free(original);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        base = Path(tmp)
        # Independent bit-stream packing, at the camera's exact odd-height geometry.
        pixels = 1808*727
        packed = b''.join(pack([((i+j)*37+13)%16384 for j in range(8)])
                          for i in range(0, pixels, 8))
        (base/'input.raw').write_bytes(packed)
        (base/'check.c').write_text(source)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(ROOT/'modules/livecomp'), str(base/'check.c'), '-o', str(base/'check')], check=True)
        subprocess.run([str(base/'check'), str(base/'input.raw'), str(base)], check=True)
        data = (base/'normal.DNG').read_bytes()
        assert data == (base/'partial.DNG').read_bytes()
        assert data[:8] == b'II*\0\x08\0\0\0'
        main_tags = tags(data, 8)
        def scalar(tag):
            kind, n, raw = main_tags[tag]
            assert n == 1
            return struct.unpack('<H' if kind == 3 else '<I', raw)[0]
        assert scalar(256) == 1808 and scalar(257) == 727 and scalar(258) == 14
        assert scalar(259) == 1 and scalar(262) == 32803
        assert main_tags[33422][2] == bytes([0,1,1,2])
        assert struct.unpack('<4I', main_tags[50829][2]) == (28,72,725,1808)
        assert struct.unpack('<2I', main_tags[50720][2]) == (1736,697)
        exif = tags(data, scalar(34665))
        assert struct.unpack('<2I', exif[33434][2]) == (1000,3030)
        offset, size = scalar(273), scalar(279)
        assert offset == 2048 and size == len(packed) and len(data) == offset+size
        raw = data[offset:]
        for block in range(pixels//8):
            value = int.from_bytes(raw[14*block:14*(block+1)], 'big')
            decoded = [(value >> (14*(7-j))) & 16383 for j in range(8)]
            assert decoded == [((block*8+j)*37+13)%16384 for j in range(8)]
    print(f'PASS: actual DNG writer, {pixels} decoded RAW14 samples, TIFF/DNG/EXIF tags, short writes, allocation/create/write/size/shutdown failures and unchanged accumulator')


if __name__ == '__main__':
    main()
