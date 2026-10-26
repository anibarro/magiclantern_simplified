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
/* Same result as lc_raw14_max, but a block is repacked and stored only when
 * a source sample is brighter. Once a composite has a few inputs most blocks
 * are unchanged, which saves the store (memory) traffic. Returns the number
 * of changed blocks for diagnostics. */
static __attribute__((optimize("O3"),unused)) unsigned lc_raw14_max_sparse(uint16_t *dst,const uint16_t *src,unsigned blocks)
{
    unsigned changed=0;
    for (unsigned n=0;n<blocks;n++,dst+=7,src+=7) {
        uint16_t a[8],b[8];lc_raw14_unpack(dst,a);lc_raw14_unpack(src,b);
        unsigned any=0;
        for (unsigned p=0;p<8;p++) if (b[p]>a[p]) { a[p]=b[p];any=1; }
        if (any) { lc_raw14_pack(dst,a);changed++; }
    }
    return changed;
}
/* Compare-first variant (LCM19): extract the 16 samples of two blocks from
 * 7 aligned words and only fall back to unpack/max/pack when a source sample
 * is brighter. Measured on an emulated ARM946 (qemu, -O2): 81 instructions
 * per 8 samples when nothing changes vs 94 for lc_raw14_max_sparse, but 203
 * vs 120 when everything changes, so the caller picks it adaptively.
 * Requires 4-byte aligned dst/src (true for both full-res buffers). */
static inline __attribute__((always_inline)) uint32_t lc_r14_any_brighter(const uint32_t *d,const uint32_t *s)
{
    uint32_t any=0;
    const uint32_t dw0=d[0],dw1=d[1],dw2=d[2],dw3=d[3],dw4=d[4],dw5=d[5],dw6=d[6];
    const uint32_t sw0=s[0],sw1=s[1],sw2=s[2],sw3=s[3],sw4=s[4],sw5=s[5],sw6=s[6];
    { uint32_t a=(dw0&0xFFFFu)>>2,b=(sw0&0xFFFFu)>>2; any|=(a<b); }
    { uint32_t a=(((dw0&0xFFFFu)&3u)<<12)|((dw0>>16)>>4),b=(((sw0&0xFFFFu)&3u)<<12)|((sw0>>16)>>4); any|=(a<b); }
    { uint32_t a=(((dw0>>16)&15u)<<10)|((dw1&0xFFFFu)>>6),b=(((sw0>>16)&15u)<<10)|((sw1&0xFFFFu)>>6); any|=(a<b); }
    { uint32_t a=(((dw1&0xFFFFu)&63u)<<8)|((dw1>>16)>>8),b=(((sw1&0xFFFFu)&63u)<<8)|((sw1>>16)>>8); any|=(a<b); }
    { uint32_t a=(((dw1>>16)&255u)<<6)|((dw2&0xFFFFu)>>10),b=(((sw1>>16)&255u)<<6)|((sw2&0xFFFFu)>>10); any|=(a<b); }
    { uint32_t a=(((dw2&0xFFFFu)&1023u)<<4)|((dw2>>16)>>12),b=(((sw2&0xFFFFu)&1023u)<<4)|((sw2>>16)>>12); any|=(a<b); }
    { uint32_t a=(((dw2>>16)&4095u)<<2)|((dw3&0xFFFFu)>>14),b=(((sw2>>16)&4095u)<<2)|((sw3&0xFFFFu)>>14); any|=(a<b); }
    { uint32_t a=(dw3&0xFFFFu)&16383u,b=(sw3&0xFFFFu)&16383u; any|=(a<b); }
    { uint32_t a=(dw3>>16)>>2,b=(sw3>>16)>>2; any|=(a<b); }
    { uint32_t a=(((dw3>>16)&3u)<<12)|((dw4&0xFFFFu)>>4),b=(((sw3>>16)&3u)<<12)|((sw4&0xFFFFu)>>4); any|=(a<b); }
    { uint32_t a=(((dw4&0xFFFFu)&15u)<<10)|((dw4>>16)>>6),b=(((sw4&0xFFFFu)&15u)<<10)|((sw4>>16)>>6); any|=(a<b); }
    { uint32_t a=(((dw4>>16)&63u)<<8)|((dw5&0xFFFFu)>>8),b=(((sw4>>16)&63u)<<8)|((sw5&0xFFFFu)>>8); any|=(a<b); }
    { uint32_t a=(((dw5&0xFFFFu)&255u)<<6)|((dw5>>16)>>10),b=(((sw5&0xFFFFu)&255u)<<6)|((sw5>>16)>>10); any|=(a<b); }
    { uint32_t a=(((dw5>>16)&1023u)<<4)|((dw6&0xFFFFu)>>12),b=(((sw5>>16)&1023u)<<4)|((sw6&0xFFFFu)>>12); any|=(a<b); }
    { uint32_t a=(((dw6&0xFFFFu)&4095u)<<2)|((dw6>>16)>>14),b=(((sw6&0xFFFFu)&4095u)<<2)|((sw6>>16)>>14); any|=(a<b); }
    { uint32_t a=(dw6>>16)&16383u,b=(sw6>>16)&16383u; any|=(a<b); }
    return any;
}
static __attribute__((optimize("O2"),unused)) unsigned lc_raw14_max_fast(uint16_t *dst,const uint16_t *src,unsigned blocks)
{
    unsigned changed=0,n=0;
    for (;n+2<=blocks;n+=2,dst+=14,src+=14) {
        if (!lc_r14_any_brighter((const uint32_t *)(void *)dst,(const uint32_t *)(const void *)src)) continue;
        changed+=lc_raw14_max_sparse(dst,src,2);
    }
    if (n<blocks) changed+=lc_raw14_max_sparse(dst,src,blocks-n);
    return changed;
}
/* LCM24 select-max. Both buffers share the same packed layout, so max(a,b)
 * per sample is a per-field choice of bits from a or b: no unpack/repack.
 * Viewed as a bit stream, 16-bit halfword h0 comes first, MSB first, so the
 * stream word W'(i) = ror16(native word i) holds 32 contiguous stream bits.
 * Two blocks = 7 words = 16 fields of 14 bits; field k starts at stream bit
 * 14k, i.e. at p = 14k-32i bits below the MSB of W'(i). Six fields cross a
 * word boundary (p > 18): their high part is the low 32-p bits of W'(i),
 * the rest the top p-18 bits of W'(i+1).
 * Compare: a field shifted to the top of a 32-bit value (lower bits = the
 * following stream bits) compares like the field itself, except on ties,
 * where either choice gives the same field value. Select: mask m has the
 * field's bits where b is brighter; out = a ^ ((a ^ b) & m). A word is only
 * written when its mask is nonzero, so unchanged cache lines stay clean.
 * Returns changed 2-block pairs (diagnostics). Requires 4-byte alignment. */
#define LC_R14_F 0x3FFFu
static inline __attribute__((always_inline)) uint32_t lc_r14_ror16(uint32_t x) { return (x>>16)|(x<<16); }
#define LC_R14_IN(p,k)    if ((a<<(p))<(b<<(p))) m|=LC_R14_F<<(18-(p))
#define LC_R14_CROSS(p)   if (((a<<(p))|(an>>(32-(p))))<((b<<(p))|(bn>>(32-(p))))) \
                              { m|=LC_R14_F>>((p)-18);mn=LC_R14_F<<(50-(p)); }
#define LC_R14_NEXT(i)    an=lc_r14_ror16(d[(i)+1]);bn=lc_r14_ror16(s[(i)+1]);mn=0
#define LC_R14_PUT(i)     if (m) { d[i]=lc_r14_ror16(a^((a^b)&m));any=1; } a=an;b=bn;m=mn
static __attribute__((optimize("O2"),unused)) unsigned lc_raw14_max_sel(uint32_t *d,const uint32_t *s,unsigned pairs)
{
    unsigned changed=0;
    for (;pairs;pairs--,d+=7,s+=7) {
        uint32_t a=lc_r14_ror16(d[0]),b=lc_r14_ror16(s[0]),an,bn,m=0,mn,any=0;
        if (a<b) m|=LC_R14_F<<18;                                    /* field 0, p0 */
        LC_R14_IN(14,1);LC_R14_NEXT(0);LC_R14_CROSS(28);LC_R14_PUT(0); /* 1, 2 */
        LC_R14_IN(10,3);LC_R14_NEXT(1);LC_R14_CROSS(24);LC_R14_PUT(1); /* 3, 4 */
        LC_R14_IN(6,5); LC_R14_NEXT(2);LC_R14_CROSS(20);LC_R14_PUT(2); /* 5, 6 */
        LC_R14_IN(2,7); LC_R14_IN(16,8);
        LC_R14_NEXT(3);LC_R14_CROSS(30);LC_R14_PUT(3);                 /* 7, 8, 9 */
        LC_R14_IN(12,10);LC_R14_NEXT(4);LC_R14_CROSS(26);LC_R14_PUT(4);/* 10, 11 */
        LC_R14_IN(8,12);LC_R14_NEXT(5);LC_R14_CROSS(22);LC_R14_PUT(5); /* 12, 13 */
        LC_R14_IN(4,14);LC_R14_IN(18,15);                              /* 14, 15 */
        if (m) { d[6]=lc_r14_ror16(a^((a^b)&m));any=1; }
        changed+=any;
    }
    return changed;
}
#undef LC_R14_IN
#undef LC_R14_CROSS
#undef LC_R14_NEXT
#undef LC_R14_PUT
#endif
