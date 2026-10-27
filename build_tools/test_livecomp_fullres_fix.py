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
    { /* LCM56 column correction: per-column offsets in every row (also the
       * OB rows 16..26), noise and a hot pixel in the OB; measured offsets
       * are exact to 1 DN, applied to every row, saturated samples kept. */
        static int32_t pat[5280],off[5280];
        for (unsigned x=0;x<W;x++) pat[x]=(int)(rnd()%61)-30+((x%8)==3 ? 12:0);
        long s0=0,s1=0;for (unsigned x=0;x<W;x+=2) { s0+=pat[x];s1+=pat[x+1]; }
        for (unsigned y=0;y<H;y++) for (unsigned b=0;b<W/8;b++) {
            uint16_t v[8];
            for (int i=0;i<8;i++) { unsigned x=b*8+i;v[i]=(uint16_t)(y<28 ? 4000+pat[x]:6000+pat[x]); }
            lc_raw14_pack((uint16_t *)(a+y*P+b*14),v);
        }
        lc_r14_set(a,P,500,20,16000);              /* hot OB pixel: trimmed */
        lc_r14_set(a,P,600,1000,15000);            /* saturated: stays */
        unsigned rms=lc_colfix_measure(a,P,W,LC_CF_Y0,LC_CF_Y1,off);
        for (unsigned x=0;x<W;x++) {
            long m=(x&1 ? s1:s0)/(long)(W/2);
            int want=pat[x]-(int)m;assert(off[x]-want<=1 && want-off[x]<=1);
        }
        assert(rms>100 && rms<300);
        lc_colfix_apply(a,P,W,0,H,off,15000);
        for (unsigned y=0;y<H;y+=37) for (unsigned x=0;x<W;x+=3) {
            if (x==600 && y==1000) continue;
            int v=(int)lc_r14_get(a,P,x,y),base=y<28 ? 4000:6000;
            long m=(x&1 ? s1:s0)/(long)(W/2);
            assert(v-(base+(int)m)<=1 && (base+(int)m)-v<=1);
        }
        assert(lc_r14_get(a,P,600,1000)==15000);
        printf("colfix ok (rms %u.%u DN)\n",rms/10,rms%10);
    }
    { /* LCM57: the 8-column lanes differ between the OB rows and the image
       * (as on the camera); the image ones must win when the image has flat
       * dark areas, the OB ones when it has none. Column pattern + noise +
       * a gradient + a bright vertical line (must not leak into its column).
       * Errors left: the slow part of the OB noise (+-8 columns cannot see it).
       * LCM60: plus a dark vertical step of 80 DN (an object edge in the
       * shadows): with the left/right check no bands; without it (LCM59)
       * the refinement puts bands along the edge. */
        static int32_t pat[5280],off[5280],s8[5280],lob[8],li[8];static uint16_t n[5280];static int16_t row[5280];
        static const int lane_img[8]={5,-6,-10,13,3,5,2,-12},lane_obr[8]={6,-4,-7,3,7,1,-6,0};   /* zero mean per parity */
        for (int t=0;t<4;t++) {
            const int bright=t==1,step=t>=2,lr=t!=3;
            for (unsigned x=0;x<W;x++) pat[x]=(int)(rnd()%41)-20;
            for (unsigned y=0;y<H;y++) for (unsigned b=0;b<W/8;b++) {
                uint16_t v[8];
                for (int i=0;i<8;i++) {
                    unsigned x=b*8+i;int nz=(int)(rnd()%61)-30;
                    int base=y<28 ? 2048+lane_obr[x&7]:(bright ? 9000:2100+(int)(x/40))+lane_img[x&7];
                    if (y>=28 && x>=3000 && x<3002) base+=3000;        /* vertical line */
                    if (step && y>=28 && x>=4000) base+=80;                /* dark object edge */
                    v[i]=(uint16_t)(base+pat[x]+nz);
                }
                lc_raw14_pack((uint16_t *)(a+y*P+b*14),v);
            }
            lc_colfix_measure(a,P,W,LC_CF_Y0,LC_CF_Y1,off);
            lc_colfix_split_lanes(off,W,lob);
            unsigned vob=lc_colfix_ob_var(a,P,W,LC_CF_Y0,LC_CF_Y1),vpix;
            for (unsigned x=0;x<W;x++) { s8[x]=0;n[x]=0; }
            struct lc_colfix_scan sc={s8,n,row,0,0,lr ? lc_colfix_lr_max(vob):0};
            for (unsigned y=28;y<H;y++) if ((y&7)<2) lc_colfix_scan_row(&sc,a,P,W,y,off,2048);
            int img=lc_colfix_combine(off,W,&sc,lob,vob,li,&vpix);
            assert(img==!bright);
            long e2=0;int emax=0,eedge=0;
            const int *lane=bright ? lane_obr:lane_img;
            for (unsigned x=LC_CF_XA;x+8<W;x++) {
                long pm=0;for (unsigned k=x&1;k<W;k+=2) pm+=pat[k];
                int want=pat[x]+lane[x&7]-(int)(pm/(long)(W/2));
                int e=off[x]-want;e2+=e*e;
                if (step && x+16>=4000 && x<4016) { if (abs(e)>eedge) eedge=abs(e);continue; }   /* next to the edge */
                if (abs(e)>emax && !(x>=2990 && x<3012)) emax=abs(e);
            }
            const long ms100=100*e2/(long)(W-LC_CF_XA-8);  /* mean square x100 */
            static const char *name[4]={"dark (image lanes)","bright (OB lanes)","dark + edge, L/R check","dark + edge, no check"};
            printf("colfix %s: mean sq err %ld.%02ld DN^2, max %d (next to the edge %d), vob %u vpix %u\n",name[t],ms100/100,ms100%100,emax,eedge,vob,vpix);
            fflush(stdout);
            if (t==0 || t==2) assert(ms100<=484 && emax<=9);   /* image estimate: rms ~1.9 DN (LCM56: ~6) */
            if (t==2) assert(eedge<=25);                      /* edge columns: one far neighbour across */
            else if (t==1) assert(ms100<=4900);               /* OB estimate only: rms ~6 DN (OB noise) */
            else if (t==3) assert(eedge>=30);                 /* LCM59 behaviour: 36 DN next to the edge (23 with the check) */
        }
    }
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
    print('PASS: focus-pixel fix (200,942 exact positions), WB neutral mapping, Bulb column fix (LCM56/57/60)')


if __name__ == '__main__':
    main()
