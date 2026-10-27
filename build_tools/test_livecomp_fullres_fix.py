#!/usr/bin/env python3
"""Host checks for the full-res output fixes and the merge kernels.

- Focus-pixel fix on a synthetic 5280x3506 packed RAW14 raster: exactly the
  24x30-tile positions are replaced by the mean of their 4 same-colour
  neighbours; every other sample is unchanged.
- White-balance neutral table/mapping.
- Optional: if LIVECOMP_QEMU_ARM points to qemu-arm, the select merge
  kernel is checked against the reference maximum as ARMv5TE code on
  an emulated ARM946 (what the camera executes). The host-side exactness
  check of the kernel is in test_livecomp_frlv.py.
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
    printf("fix ok\n");return 0;
}
'''

ARM = r'''
#include <stdint.h>
void *memcpy(void *d,const void *s,unsigned n){char*a=d;const char*b=s;while(n--)*a++=*b++;return d;}
#include "raw14-max.h"
#include "raw14-add.h"
static uint32_t st=3; static uint32_t rnd(void){ st=st*1664525u+1013904223u; return st>>8; }
enum{B=6000}; static uint16_t a[7*B] __attribute__((aligned(4))),b[7*B] __attribute__((aligned(4))),c[7*B];
static void sys_exit(int code){ register int r0 asm("r0")=code; register int r7 asm("r7")=1; asm volatile("svc 0"::"r"(r0),"r"(r7)); for(;;); }
void _start(void){
  for (int r=0;r<4;r++) {
    for (int i=0;i<B;i++){ uint16_t va[8],vb[8];
      for (int p=0;p<8;p++){ uint16_t x=rnd()&16383; va[p]=x; vb[p]=(r==0)?(rnd()&16383):((rnd()&((8<<r)-1))==0)?(rnd()&16383):(x?x-1:0);} 
      lc_raw14_pack(a+7*i,va); lc_raw14_pack(b+7*i,vb); memcpy(c+7*i,a+7*i,14);}
    /* select kernel on the camera's CPU type: same result as the reference */
    for (int i=0;i<7*B;i++) c[i]=a[i];
    lc_raw14_max(c,b,B); lc_raw14_max_sel((uint32_t *)(void *)a,(const uint32_t *)(const void *)b,B/2);
    for (int i=0;i<7*B;i++) if (a[i]!=c[i]) sys_exit(4);
    /* Live Bulb word kernel (LCM55) on the camera's CPU type: same as the reference */
    for (int i=0;i<7*B;i++) c[i]=a[i];
    lc_raw14_add(c,b,B,2048-r,15000+r*500); lc_raw14_add_sel((uint32_t *)(void *)a,(const uint32_t *)(const void *)b,B/2,2048-r,15000+r*500);
    for (int i=0;i<7*B;i++) if (a[i]!=c[i]) sys_exit(5);
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
            print('ARM946 (qemu) merge check ok: select max and Live Bulb add')
        else:
            print('ARM946 merge check skipped (set LIVECOMP_QEMU_ARM to run it)')
    print('PASS: focus-pixel fix (200,942 exact positions), WB neutral mapping')


if __name__ == '__main__':
    main()
