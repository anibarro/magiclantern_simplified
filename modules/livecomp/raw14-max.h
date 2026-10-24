#ifndef LIVECOMP_RAW14_MAX_H
#define LIVECOMP_RAW14_MAX_H
#include <stdint.h>
/* Canon's seven little-endian words contain eight MSB-first 14-bit samples.
 * Source is a completed, exclusively leased frame. Destination is CPU-owned.
 * Explicit word operations avoid compiler-dependent C bitfield layout. */
static inline __attribute__((always_inline)) void lc_raw14_unpack(const uint16_t *s,uint16_t *p)
{
    p[0]=s[0]>>2;
    p[1]=((s[0]&3)<<12)|(s[1]>>4);
    p[2]=((s[1]&15)<<10)|(s[2]>>6);
    p[3]=((s[2]&63)<<8)|(s[3]>>8);
    p[4]=((s[3]&255)<<6)|(s[4]>>10);
    p[5]=((s[4]&1023)<<4)|(s[5]>>12);
    p[6]=((s[5]&4095)<<2)|(s[6]>>14);
    p[7]=s[6]&16383;
}
static inline __attribute__((always_inline)) void lc_raw14_pack(uint16_t *d,const uint16_t *p)
{
    d[0]=(p[0]<<2)|(p[1]>>12);
    d[1]=(p[1]<<4)|(p[2]>>10);
    d[2]=(p[2]<<6)|(p[3]>>8);
    d[3]=(p[3]<<8)|(p[4]>>6);
    d[4]=(p[4]<<10)|(p[5]>>4);
    d[5]=(p[5]<<12)|(p[6]>>2);
    d[6]=(p[6]<<14)|p[7];
}
/* This loop processes every sensor pixel. At -Os the old build called unpack
 * twice per eight pixels and used stack arrays; optimize only this hot path. */
static __attribute__((optimize("O3"))) void lc_raw14_max(uint16_t *dst,const uint16_t *src,unsigned blocks)
{
    for (unsigned n=0;n<blocks;n++,dst+=7,src+=7) {
        uint16_t a[8],b[8];lc_raw14_unpack(dst,a);lc_raw14_unpack(src,b);
        for (unsigned p=0;p<8;p++) if (b[p]>a[p]) a[p]=b[p];
        lc_raw14_pack(dst,a);
    }
}
#endif
