#ifndef LIVECOMP_RAW14_ADD_H
#define LIVECOMP_RAW14_ADD_H
/* GPL-2.0-or-later. Live Bulb merge kernel on packed 14-bit RAW.
 *
 * A bulb exposure is the sum of its sub-exposures. The accumulator starts as
 * the first frame (black level + signal); every further frame adds its signal
 * only:   acc = acc + (frame - black)
 * so the accumulator stays at one black level and the DNG black level remains
 * valid. The subtraction is signed per sample: noise below black is added as
 * it is (no positive bias from clamping each frame at black). The result is
 * clamped to [0, white]. A sample that has reached white stays there, like a
 * saturated photosite in one long exposure (no wobble from later noise).
 *
 * Unlike the select-max kernel, every word changes, so there is no shortcut:
 * unpack, add, clamp, pack; the hot loop is compiled at O3 like lc_raw14_max.
 * Both buffers share Canon's packed layout (raw14-max.h). */
#include <stdint.h>
#include "raw14-max.h"
/* Reference kernel (LCM54): kept for the host tests. */
static __attribute__((optimize("O3"),unused)) void lc_raw14_add(uint16_t *dst,const uint16_t *src,unsigned blocks,
    unsigned black,unsigned white)
{
    const int bl=(int)black,wh=(int)(white>16383u ? 16383u:white);
    for (unsigned n=0;n<blocks;n++,dst+=7,src+=7) {
        uint16_t a[8],b[8];lc_raw14_unpack(dst,a);lc_raw14_unpack(src,b);
        for (unsigned p=0;p<8;p++) {
            if (a[p]>=wh) continue;                       /* saturated: stays */
            const int v=(int)a[p]+(int)b[p]-bl;
            a[p]=(uint16_t)(v<0 ? 0:v>wh ? wh:v);
        }
        lc_raw14_pack(dst,a);
    }
}
/* Word kernel (LCM55), used on the camera. Same result as lc_raw14_add, bit
 * for bit (host and emulated ARM946 tests), but works like lc_raw14_max_sel:
 * on 32-bit stream words W'(i)=ror16(native word i) of two blocks (7 words,
 * 16 fields, field k at stream bit 14k), every field taken out with one shift
 * and mask (two words for the 6 fields that cross a word), summed, clamped,
 * and put back; no halfword loads, no stack arrays. ARM946 (qemu): 241
 * instructions per 16 pixels vs 376 for lc_raw14_add (LCM54: 3.85 s per
 * full frame on the camera). Requires 4-byte alignment; pairs of blocks. */
#define LC_A_M 0x3FFFu
static inline __attribute__((always_inline)) uint32_t lc_a_r16(uint32_t x) { return (x>>16)|(x<<16); }
static inline __attribute__((always_inline)) uint32_t lc_a_one(uint32_t a,uint32_t b,int bl,uint32_t w)
{
    int v=(int)(a+b)-bl;
    v=v<0 ? 0:v;
    v=v>(int)w ? (int)w:v;
    return a>=w ? a:(uint32_t)v;
}
#define LC_A_IN(W,p)      (((W)>>(18-(p)))&LC_A_M)
#define LC_A_CR(W,N,p)    ((((W)<<((p)-18))|((N)>>(50-(p))))&LC_A_M)
static __attribute__((optimize("O2"),unused)) void lc_raw14_add_sel(uint32_t *d,const uint32_t *s,unsigned pairs,
    unsigned black,unsigned white)
{
    const int bl=(int)black;const uint32_t w=white>16383u ? 16383u:white;
    for (;pairs;pairs--,d+=7,s+=7) {
        uint32_t a0=lc_a_r16(d[0]),b0=lc_a_r16(s[0]),a1=lc_a_r16(d[1]),b1=lc_a_r16(s[1]),v,o0,o1,o2;
        o0 =lc_a_one(LC_A_IN(a0,0),LC_A_IN(b0,0),bl,w)<<18;
        o0|=lc_a_one(LC_A_IN(a0,14),LC_A_IN(b0,14),bl,w)<<4;
        v=lc_a_one(LC_A_CR(a0,a1,28),LC_A_CR(b0,b1,28),bl,w); o0|=v>>10; o1=v<<22;
        d[0]=lc_a_r16(o0);
        uint32_t a2=lc_a_r16(d[2]),b2=lc_a_r16(s[2]);
        o1|=lc_a_one(LC_A_IN(a1,10),LC_A_IN(b1,10),bl,w)<<8;
        v=lc_a_one(LC_A_CR(a1,a2,24),LC_A_CR(b1,b2,24),bl,w); o1|=v>>6; o2=v<<26;
        d[1]=lc_a_r16(o1);
        uint32_t a3=lc_a_r16(d[3]),b3=lc_a_r16(s[3]);
        o2|=lc_a_one(LC_A_IN(a2,6),LC_A_IN(b2,6),bl,w)<<12;
        v=lc_a_one(LC_A_CR(a2,a3,20),LC_A_CR(b2,b3,20),bl,w); o2|=v>>2; o0=v<<30;
        d[2]=lc_a_r16(o2);
        uint32_t a4=lc_a_r16(d[4]),b4=lc_a_r16(s[4]);
        o0|=lc_a_one(LC_A_IN(a3,2),LC_A_IN(b3,2),bl,w)<<16;
        o0|=lc_a_one(LC_A_IN(a3,16),LC_A_IN(b3,16),bl,w)<<2;
        v=lc_a_one(LC_A_CR(a3,a4,30),LC_A_CR(b3,b4,30),bl,w); o0|=v>>12; o1=v<<20;
        d[3]=lc_a_r16(o0);
        uint32_t a5=lc_a_r16(d[5]),b5=lc_a_r16(s[5]);
        o1|=lc_a_one(LC_A_IN(a4,12),LC_A_IN(b4,12),bl,w)<<6;
        v=lc_a_one(LC_A_CR(a4,a5,26),LC_A_CR(b4,b5,26),bl,w); o1|=v>>8; o2=v<<24;
        d[4]=lc_a_r16(o1);
        uint32_t a6=lc_a_r16(d[6]),b6=lc_a_r16(s[6]);
        o2|=lc_a_one(LC_A_IN(a5,8),LC_A_IN(b5,8),bl,w)<<10;
        v=lc_a_one(LC_A_CR(a5,a6,22),LC_A_CR(b5,b6,22),bl,w); o2|=v>>4; o0=v<<28;
        d[5]=lc_a_r16(o2);
        o0|=lc_a_one(LC_A_IN(a6,4),LC_A_IN(b6,4),bl,w)<<14;
        o0|=lc_a_one(LC_A_IN(a6,18),LC_A_IN(b6,18),bl,w);
        d[6]=lc_a_r16(o0);
    }
}
/* LCM59 dark frames: acc = acc - (dark - black), the exact inverse of
 * lc_raw14_add (same clamp; a sample at or above white stays saturated).
 * Reference kernel for the host tests; lc_raw14_sub_sel is used on the
 * camera (same structure and speed as lc_raw14_add_sel). */
static __attribute__((optimize("O3"),unused)) void lc_raw14_sub(uint16_t *dst,const uint16_t *src,unsigned blocks,
    unsigned black,unsigned white)
{
    const int bl=(int)black,wh=(int)(white>16383u ? 16383u:white);
    for (unsigned n=0;n<blocks;n++,dst+=7,src+=7) {
        uint16_t a[8],b[8];lc_raw14_unpack(dst,a);lc_raw14_unpack(src,b);
        for (unsigned p=0;p<8;p++) {
            if (a[p]>=wh) continue;
            const int v=(int)a[p]-(int)b[p]+bl;
            a[p]=(uint16_t)(v<0 ? 0:v>wh ? wh:v);
        }
        lc_raw14_pack(dst,a);
    }
}
static inline __attribute__((always_inline)) uint32_t lc_s_one(uint32_t a,uint32_t b,int bl,uint32_t w)
{
    int v=(int)a-(int)b+bl;
    v=v<0 ? 0:v;
    v=v>(int)w ? (int)w:v;
    return a>=w ? a:(uint32_t)v;
}
static __attribute__((optimize("O2"),unused)) void lc_raw14_sub_sel(uint32_t *d,const uint32_t *s,unsigned pairs,
    unsigned black,unsigned white)
{
    const int bl=(int)black;const uint32_t w=white>16383u ? 16383u:white;
    for (;pairs;pairs--,d+=7,s+=7) {
        uint32_t a0=lc_a_r16(d[0]),b0=lc_a_r16(s[0]),a1=lc_a_r16(d[1]),b1=lc_a_r16(s[1]),v,o0,o1,o2;
        o0 =lc_s_one(LC_A_IN(a0,0),LC_A_IN(b0,0),bl,w)<<18;
        o0|=lc_s_one(LC_A_IN(a0,14),LC_A_IN(b0,14),bl,w)<<4;
        v=lc_s_one(LC_A_CR(a0,a1,28),LC_A_CR(b0,b1,28),bl,w); o0|=v>>10; o1=v<<22;
        d[0]=lc_a_r16(o0);
        uint32_t a2=lc_a_r16(d[2]),b2=lc_a_r16(s[2]);
        o1|=lc_s_one(LC_A_IN(a1,10),LC_A_IN(b1,10),bl,w)<<8;
        v=lc_s_one(LC_A_CR(a1,a2,24),LC_A_CR(b1,b2,24),bl,w); o1|=v>>6; o2=v<<26;
        d[1]=lc_a_r16(o1);
        uint32_t a3=lc_a_r16(d[3]),b3=lc_a_r16(s[3]);
        o2|=lc_s_one(LC_A_IN(a2,6),LC_A_IN(b2,6),bl,w)<<12;
        v=lc_s_one(LC_A_CR(a2,a3,20),LC_A_CR(b2,b3,20),bl,w); o2|=v>>2; o0=v<<30;
        d[2]=lc_a_r16(o2);
        uint32_t a4=lc_a_r16(d[4]),b4=lc_a_r16(s[4]);
        o0|=lc_s_one(LC_A_IN(a3,2),LC_A_IN(b3,2),bl,w)<<16;
        o0|=lc_s_one(LC_A_IN(a3,16),LC_A_IN(b3,16),bl,w)<<2;
        v=lc_s_one(LC_A_CR(a3,a4,30),LC_A_CR(b3,b4,30),bl,w); o0|=v>>12; o1=v<<20;
        d[3]=lc_a_r16(o0);
        uint32_t a5=lc_a_r16(d[5]),b5=lc_a_r16(s[5]);
        o1|=lc_s_one(LC_A_IN(a4,12),LC_A_IN(b4,12),bl,w)<<6;
        v=lc_s_one(LC_A_CR(a4,a5,26),LC_A_CR(b4,b5,26),bl,w); o1|=v>>8; o2=v<<24;
        d[4]=lc_a_r16(o1);
        uint32_t a6=lc_a_r16(d[6]),b6=lc_a_r16(s[6]);
        o2|=lc_s_one(LC_A_IN(a5,8),LC_A_IN(b5,8),bl,w)<<10;
        v=lc_s_one(LC_A_CR(a5,a6,22),LC_A_CR(b5,b6,22),bl,w); o2|=v>>4; o0=v<<28;
        d[5]=lc_a_r16(o2);
        o0|=lc_s_one(LC_A_IN(a6,4),LC_A_IN(b6,4),bl,w)<<14;
        o0|=lc_s_one(LC_A_IN(a6,18),LC_A_IN(b6,18),bl,w);
        d[6]=lc_a_r16(o0);
    }
}
#undef LC_A_IN
#undef LC_A_CR
#endif
