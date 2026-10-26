#!/usr/bin/env python3
"""Exercise actual pattern filling and full-buffer verification for every layout."""
from pathlib import Path
import os
import subprocess
import tempfile
from livecomp_test_utils import function

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / 'modules/livecomp'


def main():
    source = (MOD / 'fullres-check.c').read_text()
    fields = source[source.index('struct check_result {'):source.index('static unsigned physical')]
    code = r'''
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/mman.h>
#include "native-max.h"
#include "fullres-layout.h"
#include "raw14-max.h"
#define MAX(a,b) ((a)>(b)?(a):(b))
static uintptr_t physical(void *p) { return (uintptr_t)p; }
struct card_info { int unused; };
static unsigned probe_allocs,probe_frees,probe_limit;
static int valid(const volatile unsigned *cancel,struct card_info *card) { return !*cancel && card; }
static void *fio_malloc(unsigned bytes) {
    assert(bytes==1024*1024);if(probe_allocs==probe_limit)return NULL;
    probe_allocs++;return malloc(bytes);
}
static void fio_free(void *ptr) {
    uint32_t *p=ptr;assert(p[0]==0x13579bdf && p[1024*1024/4-1]==0x2468ace0);
    probe_frees++;free(ptr);
}
static void msleep(unsigned n) { (void)n; }
''' + fields + function(source,'probe_segments(') + function(source, 'fill(') + function(source, 'verify(') + r'''
int main(void) {
    uint8_t *a=mmap((void *)0x02000000,LC_FR_STORAGE,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    uint8_t *b=mmap((void *)0x06000000,LC_FR_STORAGE,PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(a==(void *)0x02000000 && b==(void *)0x06000000);
    struct check_result result={0};
    for (unsigned test=0;test<=LC_FR_CASES;test++) {
        unsigned rows=test==LC_FR_CASES ? LC_FR_HEIGHT:lc_fr_case_rows[test];
        unsigned output=test==LC_FR_CASES ? LC_FR_HALO_ROWS:lc_fr_output_row(test);
        assert((output+rows)*LC_FR_PITCH<=LC_FR_STORAGE);
        if(test<LC_FR_CASES && !lc_fr_case_inplace(test)) assert(output>=rows+2*LC_FR_HALO_ROWS);
        fill(a,0);fill(b,1);
        /* Independent scalar oracle writes the expected output, without the
         * production maximum kernel or verifier's destination mapping. */
        uint16_t *dst=(void *)(a+output*LC_FR_PITCH);
        for (unsigned i=0;i<rows*LC_FR_WIDTH;i+=8,dst+=7) {
            uint16_t samples[8];
            for(unsigned k=0;k<8;k++) {
                unsigned x=LC_FR_HALO_ROWS*LC_FR_WIDTH+i+k;
                unsigned x0=(x*37u+13u)&16383u,x1=(x*109u+173u)&16383u;
                samples[k]=x0>x1 ? x0:x1;
            }
            lc_raw14_pack(dst,samples);
        }
        verify(a,b,&result,output,rows);
        assert(result.checked==LC_FR_STORAGE*8/14 && !result.mismatches && result.source_ok);
        a[0]^=1; /* a write outside the destination must be detected */
        verify(a,b,&result,output,rows);assert(result.mismatches);
        a[0]^=1;b[LC_FR_STORAGE-1]^=1;
        verify(a,b,&result,output,rows);assert(!result.source_ok);
    }
    struct card_info card;unsigned cancel=0;
    memset(&result,0,sizeof(result));probe_limit=3;
    probe_segments(&result,&cancel,&card);
    assert(result.segment_blocks==3 && result.segment_bytes==3*1024*1024 && probe_frees==3);
    memset(&result,0,sizeof(result));probe_allocs=probe_frees=0;probe_limit=40;
    probe_segments(&result,&cancel,&card);
    assert(result.segment_blocks==32 && probe_allocs==32 && probe_frees==32);
    memset(&result,0,sizeof(result));cancel=1;probe_allocs=probe_frees=0;
    probe_segments(&result,&cancel,&card);assert(!probe_allocs && !probe_frees);
    return 0;
}
'''
    with tempfile.TemporaryDirectory() as tmp:
        p = Path(tmp)
        (p / 'check.c').write_text(code)
        subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu99', '-O2', '-no-pie',
                        '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                        '-I' + str(MOD), str(p / 'check.c'), '-o', str(p / 'check')], check=True)
        subprocess.run([str(p / 'check')], check=True)
    print('PASS: all six native layouts and full-frame CPU verifier; every sample, source and outside-output corruption')


if __name__ == '__main__':
    main()
