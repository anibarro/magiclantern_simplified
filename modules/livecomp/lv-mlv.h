/* GPL-2.0-or-later. LCM67: one-frame MLV (v2.0) for the LV RAW14 result.
 * Same blocks and field values as modules/silent (single image mode), laid
 * out byte by byte here so the module needs neither mlv_rec nor mlv.h:
 * MLVI, RAWI, IDNT, EXPO, LENS, RTCI, VIDF. The VIDF frameSpace pads the
 * header to LC_MLV_HEADER_BYTES, so the frame data starts at a 4 KiB file
 * offset and is written straight from the accumulator in Canon's byte order
 * (no swap, unlike the DNG). Checked against mlv.h by test_livecomp_mlv.py.
 * AI-assisted module implementation. */
#ifndef LIVECOMP_LV_MLV_H
#define LIVECOMP_LV_MLV_H
#include <stdint.h>
#include <string.h>
#include "lv-dng.h"
#define LC_MLV_HEADER_BYTES 4096u
struct lc_mlv_extra {
    uint64_t guid;
    uint32_t camera_model;          /* PROP 0x00000002 offset 32 (camera_model_id) */
    char lens[32];
    uint16_t tm[9];                 /* sec,min,hour,mday,mon,year-1900,wday,yday,isdst */
};
static void lc_mlv_u64(uint8_t *p,uint64_t v) { lc_dng_u32(p,(uint32_t)v);lc_dng_u32(p+4,(uint32_t)(v>>32)); }
static uint8_t *lc_mlv_block(uint8_t *p,const char *type,unsigned size)
{
    memcpy(p,type,4);lc_dng_u32(p+4,size);lc_mlv_u64(p+8,0);return p+16;   /* timestamp 0 */
}
/* Returns the header length (LC_MLV_HEADER_BYTES), 0 for bad metadata. */
static unsigned lc_mlv_header(uint8_t *h,const struct lc_dng_metadata *m,const struct lc_mlv_extra *x)
{
    if (!m->width || m->width>5280 || !m->height || m->height>3528 ||
        m->pitch!=m->width*14/8 || m->bytes!=m->pitch*m->height ||
        m->black>=m->white || m->white>16383 || !m->tvr || !m->fps ||
        m->active[0]>=m->active[2] || m->active[2]>m->height ||
        m->active[1]>=m->active[3] || m->active[3]>m->width) return 0;
    memset(h,0,LC_MLV_HEADER_BYTES);
    uint8_t *p=h;
    /* MLVI 52 */
    memcpy(p,"MLVI",4);lc_dng_u32(p+4,52);memcpy(p+8,"v2.0",4);lc_mlv_u64(p+16,x->guid);
    lc_dng_u16(p+24,0);lc_dng_u16(p+26,1);lc_dng_u32(p+28,4);   /* file 0 of 1, single image mode */
    lc_dng_u16(p+32,1);lc_dng_u16(p+34,0);lc_dng_u32(p+36,1);lc_dng_u32(p+40,0);   /* RAW, 1 frame */
    lc_dng_u32(p+44,m->fps);lc_dng_u32(p+48,1000);p+=52;
    /* RAWI 180: xRes, yRes, raw_info (0xa0) */
    uint8_t *q=lc_mlv_block(p,"RAWI",180);
    lc_dng_u16(q,m->width);lc_dng_u16(q+2,m->height);q+=4;
    const uint32_t y1=m->active[0],x1=m->active[1],y2=m->active[2],x2=m->active[3];
    const uint32_t ri[21]={m->raw_api,0,m->height,m->width,m->pitch,m->bytes,14,m->black,m->white,
        x1,y1,x2-x1,y2-y1,y1,x1,y2,x2,0,0,m->cfa,m->illuminant ? m->illuminant:21};
    for (unsigned i=0;i<21;i++) lc_dng_u32(q+4*i,ri[i]);
    for (unsigned i=0;i<18;i++) lc_dng_u32(q+84+4*i,(uint32_t)m->matrix[i]);
    lc_dng_u32(q+156,(uint32_t)m->dynamic_range);p+=180;
    /* IDNT 84 */
    q=lc_mlv_block(p,"IDNT",84);memcpy(q,"Canon EOS M",11);lc_dng_u32(q+32,x->camera_model);p+=84;
    /* EXPO 40: manual ISO, shutter in microseconds */
    q=lc_mlv_block(p,"EXPO",40);
    lc_dng_u32(q,0);lc_dng_u32(q+4,m->iso);lc_dng_u32(q+8,m->iso);lc_dng_u32(q+12,0);
    lc_mlv_u64(q+16,m->exposure_ms ? (uint64_t)m->exposure_ms*1000u:1000000000u/m->tvr);p+=40;
    /* LENS 96 */
    q=lc_mlv_block(p,"LENS",96);
    lc_dng_u16(q,m->focal);lc_dng_u16(q+2,65535);lc_dng_u16(q+4,m->aperture*10);   /* aperture: f x10 -> x100 */
    for (unsigned i=0;i<31 && x->lens[i];i++) q[16+i]=(uint8_t)x->lens[i];
    p+=96;
    /* RTCI 44 */
    q=lc_mlv_block(p,"RTCI",44);
    for (unsigned i=0;i<9;i++) lc_dng_u16(q+2*i,x->tm[i]);
    p+=44;
    /* VIDF 32 + frameSpace, then the frame */
    const unsigned used=(unsigned)(p-h)+32,space=LC_MLV_HEADER_BYTES-used;
    q=lc_mlv_block(p,"VIDF",32+space+m->bytes);
    lc_dng_u32(q,0);lc_dng_u32(q+12,space);           /* frame 0, crop/pan 0 */
    return LC_MLV_HEADER_BYTES;
}
#endif
