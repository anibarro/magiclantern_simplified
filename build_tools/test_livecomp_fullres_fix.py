#!/usr/bin/env python3
"""Host checks for LCM19 full-res output fixes and the compare-first merge.

- Focus-pixel fix on a synthetic 5280x3506 packed RAW14 raster: exactly the
  24x30-tile positions are replaced by the mean of their 4 same-colour
  neighbours; every other sample is unchanged.
- White-balance neutral table/mapping.
- lc_raw14_max_fast == lc_raw14_max_sparse on random/edge data, and the
  compare-first predicate is exact for every field position.
- Optional: if LIVECOMP_QEMU_ARM points to qemu-arm, the same merge check runs
  as ARMv5TE code on an emulated ARM946 (what the camera executes).
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / 'modules/livecomp'

HOST = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fullres-fix.h"
static uint32_t st=7; static uint32_t rnd(void){ st=st*1664525u+1013904223u; return st>>8; }
static int is_fp(unsigned x,unsigned y){
    if (x<LC_FP_X0||x>LC_FP_X1||y<LC_FP_Y0||y>LC_FP_Y1) return 0;
    for (int t=0;t<8;t++) if (x%24==lc_fp_tile[t][0] && y%30==lc_fp_tile[t][1]) return 1;
    return 0;
}
int main(void){
    const unsigned W=5280,H=3506,P=W*14/8;
    uint8_t *a=malloc((size_t)P*H),*ref=malloc((size_t)P*H);
    for (unsigned y=0;y<H;y++) for (unsigned b=0;b<W/8;b++) {
        uint16_t v[8];for (int i=0;i<8;i++) v[i]=2000+rnd()%3000;
        lc_raw14_pack((uint16_t *)(a+y*P+b*14),v);
    }
    unsigned count=0;
    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) if (is_fp(x,y)) { lc_r14_set(a,P,x,y,16000);count++; }
    assert(count==200942);   /* same size as MLV App's 80000331_5280x3506.fpm */
    memcpy(ref,a,(size_t)P*H);
    unsigned fixed=lc_fix_focus_pixels(a,W,H,P);
    assert(fixed==count);
    for (unsigned y=0;y<H;y++) for (unsigned x=0;x<W;x++) {
        unsigned v=lc_r14_get(a,P,x,y);
        if (!is_fp(x,y)) { assert(v==lc_r14_get(ref,P,x,y)); continue; }
        unsigned l=lc_r14_get(ref,P,x-2,y),r=x+2<W?lc_r14_get(ref,P,x+2,y):l,u=lc_r14_get(ref,P,x,y-2),d=lc_r14_get(ref,P,x,y+2);
        assert(!is_fp(x-2,y) && !is_fp(x,y-2) && !is_fp(x,y+2) && (x+2>=W || !is_fp(x+2,y)));
        assert(v==(l+r+u+d+2)/4);
    }
    assert(lc_fix_focus_pixels(a,1808,727,1808*14/8)==0);   /* only the full-res raster */
    /* white balance */
    int32_t n[3];
    lc_wb_neutral_for(9,5200,0,0,0,n);
    assert(n[1]==1000000 && n[0]>450000 && n[0]<480000 && n[2]>620000 && n[2]<650000);
    for (int k=2600;k<=10000;k+=100) { int32_t p[3];lc_wb_neutral_for(9,k-100,0,0,0,p);lc_wb_neutral_for(9,k,0,0,0,n);
        assert(n[0]<=p[0] && n[2]>=p[2]); }   /* warmer -> more red neutral, less blue */
    lc_wb_neutral_for(3,0,0,0,0,n);int32_t t[3];lc_wb_neutral_for(9,3200,0,0,0,t);assert(n[0]==t[0]);
    lc_wb_neutral_for(6,0,2048,1024,1536,n);assert(n[0]==500000 && n[2]==666666);
    lc_wb_neutral_for(9,99999,0,0,0,n);lc_wb_neutral_for(1,0,0,0,0,t);assert(n[0]==t[0]);  /* invalid kelvin -> 5500 */
    /* compare-first merge */
    enum{B=40002}; static uint16_t x[7*B],y[7*B],z[7*B];
    for (int r=0;r<10;r++) {
        for (int i=0;i<B;i++) { uint16_t va[8],vb[8];
            for (int p=0;p<8;p++) { uint16_t q=rnd()&16383,w;
                switch (r%5) { case 0: w=rnd()&16383;break; case 1: w=q;break; case 2: w=q?q-1:0;break;
                    case 3: q=(rnd()&1)?16383:0;w=(rnd()&1)?16383:0;break; default: w=(rnd()%40==0)?(rnd()&16383):(q?q-1:0); }
                va[p]=q;vb[p]=w; }
            lc_raw14_pack(x+7*i,va);lc_raw14_pack(y+7*i,vb); }
        memcpy(z,x,sizeof x);
        unsigned c1=lc_raw14_max_sparse(z,y,B),c2=lc_raw14_max_fast(x,y,B);
        assert(!memcmp(x,z,sizeof x) && c1==c2);
    }
    for (int pos=0;pos<16;pos++) for (int u=0;u<16384;u+=97) for (int v=0;v<16384;v+=89) {
        uint16_t va[16],vb[16],d[14],s[14];
        for (int p=0;p<16;p++) { va[p]=rnd()&16383;vb[p]=va[p]?va[p]-1:0; }
        va[pos]=u;vb[pos]=v;
        lc_raw14_pack(d,va);lc_raw14_pack(d+7,va+8);lc_raw14_pack(s,vb);lc_raw14_pack(s+7,vb+8);
        uint32_t dw[7],sw[7];memcpy(dw,d,28);memcpy(sw,s,28);
        assert((lc_r14_any_brighter(dw,sw)!=0)==(v>u));
    }
    printf("fix ok\n");return 0;
}
'''

ARM = r'''
#include <stdint.h>
void *memcpy(void *d,const void *s,unsigned n){char*a=d;const char*b=s;while(n--)*a++=*b++;return d;}
#include "raw14-max.h"
static uint32_t st=3; static uint32_t rnd(void){ st=st*1664525u+1013904223u; return st>>8; }
enum{B=6000}; static uint16_t a[7*B],b[7*B],c[7*B];
static void sys_exit(int code){ register int r0 asm("r0")=code; register int r7 asm("r7")=1; asm volatile("svc 0"::"r"(r0),"r"(r7)); for(;;); }
void _start(void){
  for (int r=0;r<4;r++) {
    for (int i=0;i<B;i++){ uint16_t va[8],vb[8];
      for (int p=0;p<8;p++){ uint16_t x=rnd()&16383; va[p]=x; vb[p]=(r==0)?(rnd()&16383):((rnd()&((8<<r)-1))==0)?(rnd()&16383):(x?x-1:0);} 
      lc_raw14_pack(a+7*i,va); lc_raw14_pack(b+7*i,vb); memcpy(c+7*i,a+7*i,14);}
    unsigned c1=lc_raw14_max_sparse(c,b,B),c2=lc_raw14_max_fast(a,b,B);
    if (c1!=c2) sys_exit(2);
    for (int i=0;i<7*B;i++) if (a[i]!=c[i]) sys_exit(3);
  }
  sys_exit(0);
}
'''


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        (tmp / 'fix.c').write_text(HOST)
        subprocess.run(['cc', '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-sign-compare',
                        '-I' + str(MOD), str(tmp / 'fix.c'), '-o', str(tmp / 'fix')], check=True)
        print(subprocess.run([str(tmp / 'fix')], check=True, capture_output=True, text=True).stdout.strip())
        qemu = os.environ.get('LIVECOMP_QEMU_ARM')
        if qemu:
            (tmp / 'arm.c').write_text(ARM)
            subprocess.run(['arm-none-eabi-gcc', '-O2', '-march=armv5te', '-marm', '-nostdlib', '-static', '-w',
                            '-I' + str(MOD), str(tmp / 'arm.c'), '-o', str(tmp / 'arm'), '-Wl,-Ttext=0x10000'], check=True)
            rc = subprocess.run([qemu, '-cpu', 'arm946', str(tmp / 'arm')]).returncode
            assert rc == 0, rc
            print('ARM946 (qemu) merge check ok')
        else:
            print('ARM946 merge check skipped (set LIVECOMP_QEMU_ARM to run it)')
    print('PASS: focus-pixel fix (200,942 exact positions), WB neutral mapping, compare-first merge exactness')


if __name__ == '__main__':
    main()
