/* GPL-2.0-or-later. Bounded uncompressed DNG for the LV RAW14 composite.
 * TIFF/DNG field choices follow src/chdk-dng.c. No thumbnail allocation and no
 * in-place byte swapping of the accumulator. AI-assisted module implementation.
 */
#ifndef LIVECOMP_LV_DNG_H
#define LIVECOMP_LV_DNG_H
#include <stdint.h>
#include <string.h>
#define LC_DNG_HEADER_BYTES 2048u
struct lc_dng_metadata {
    uint32_t width,height,pitch,bytes,black,white,cfa,active[4];
    int32_t matrix[18];
    uint32_t iso,tvr,aperture,focal,fps,illuminant;
    int32_t neutral[3];   /* AsShotNeutral x1e6 (R,G,B); all zero = fixed daylight */
};
static void lc_dng_u16(uint8_t *p,uint32_t v) { p[0]=v;p[1]=v>>8; }
static void lc_dng_u32(uint8_t *p,uint32_t v)
{ p[0]=v;p[1]=v>>8;p[2]=v>>16;p[3]=v>>24; }
struct lc_dng_builder { uint8_t *h;unsigned entry,extra;int ok; };
static void lc_dng_entry(struct lc_dng_builder *b,unsigned tag,unsigned type,
                         unsigned count,const void *data,unsigned bytes)
{
    if (!b->ok || b->entry+12>1024 || b->extra+bytes>LC_DNG_HEADER_BYTES) { b->ok=0;return; }
    uint8_t *p=b->h+b->entry;b->entry+=12;
    lc_dng_u16(p,tag);lc_dng_u16(p+2,type);lc_dng_u32(p+4,count);
    if (bytes<=4) memcpy(p+8,data,bytes);
    else {
        lc_dng_u32(p+8,b->extra);memcpy(b->h+b->extra,data,bytes);
        b->extra+=(bytes+1)&~1u;
    }
}
static void lc_dng_scalar(struct lc_dng_builder *b,unsigned tag,unsigned type,uint32_t value)
{
    uint8_t v[4];lc_dng_u32(v,value);
    lc_dng_entry(b,tag,type,1,v,type==3 ? 2:4);
}
static int lc_dng_header(uint8_t *h,const struct lc_dng_metadata *m)
{
    if (!m->width || m->width>5280 || m->width%8 || !m->height || m->height>3528 ||
        m->pitch!=m->width*14/8 || m->bytes!=m->pitch*m->height ||
        m->black>=m->white || m->white>16383 || !m->tvr || !m->fps ||
        m->active[0]>=m->active[2] || m->active[2]>m->height ||
        m->active[1]>=m->active[3] || m->active[3]>m->width) return 0;
    for (unsigned i=1;i<18;i+=2) if (!m->matrix[i]) return 0;
    memset(h,0,LC_DNG_HEADER_BYTES);h[0]=h[1]='I';lc_dng_u16(h+2,42);lc_dng_u32(h+4,8);
    lc_dng_u16(h+8,30);
    struct lc_dng_builder b={h,10,1024,1};
    lc_dng_scalar(&b,256,4,m->width);lc_dng_scalar(&b,257,4,m->height);
    lc_dng_scalar(&b,258,3,14);lc_dng_scalar(&b,259,3,1);lc_dng_scalar(&b,262,3,32803);
    static const char desc[]="LCM10 LiveView per-pixel maximum";
    static const char make[]="Canon",model[]="Canon EOS M",software[]="Magic Lantern LCM10";
    lc_dng_entry(&b,270,2,sizeof(desc),desc,sizeof(desc));
    lc_dng_entry(&b,271,2,sizeof(make),make,sizeof(make));
    lc_dng_entry(&b,272,2,sizeof(model),model,sizeof(model));
    lc_dng_scalar(&b,273,4,LC_DNG_HEADER_BYTES);lc_dng_scalar(&b,274,3,1);
    lc_dng_scalar(&b,277,3,1);lc_dng_scalar(&b,278,4,m->height);
    lc_dng_scalar(&b,279,4,m->bytes);lc_dng_scalar(&b,284,3,1);
    lc_dng_entry(&b,305,2,sizeof(software),software,sizeof(software));
    uint8_t v[72];lc_dng_u16(v,2);lc_dng_u16(v+2,2);
    lc_dng_entry(&b,33421,3,2,v,4);
    lc_dng_u32(v,m->cfa);lc_dng_entry(&b,33422,1,4,v,4);
    lc_dng_scalar(&b,34665,4,512); /* EXIF IFD */
    lc_dng_u32(v,0x00000301);lc_dng_entry(&b,50706,1,4,v,4);
    lc_dng_u32(v,0x00000101);lc_dng_entry(&b,50707,1,4,v,4);
    lc_dng_entry(&b,50708,2,sizeof(model),model,sizeof(model));
    lc_dng_scalar(&b,50714,4,m->black);lc_dng_scalar(&b,50717,4,m->white);
    memset(v,0,8);lc_dng_entry(&b,50719,4,2,v,8);
    lc_dng_u32(v,m->active[3]-m->active[1]);lc_dng_u32(v+4,m->active[2]-m->active[0]);
    lc_dng_entry(&b,50720,4,2,v,8);
    for (unsigned i=0;i<18;i++) lc_dng_u32(v+4*i,(uint32_t)m->matrix[i]);
    lc_dng_entry(&b,50721,10,9,v,72);
    /* Caller's AsShotNeutral (full-res: from Canon's WB setting), otherwise the
     * same daylight neutral as the existing core's default DNG writer. */
    uint32_t neutral[]={473635,1000000,1,1,624000,1000000};
    if (m->neutral[0]>0 && m->neutral[1]>0 && m->neutral[2]>0) {
        neutral[0]=m->neutral[0];neutral[2]=m->neutral[1];neutral[3]=1000000;neutral[4]=m->neutral[2];
    }
    for (unsigned i=0;i<6;i++) lc_dng_u32(v+4*i,neutral[i]);
    lc_dng_entry(&b,50728,5,3,v,24);
    lc_dng_scalar(&b,50778,3,m->illuminant ? m->illuminant:21);
    for (unsigned i=0;i<4;i++) lc_dng_u32(v+4*i,m->active[i]);
    lc_dng_entry(&b,50829,4,4,v,16);
    lc_dng_u32(v,m->fps);lc_dng_u32(v+4,1000);lc_dng_entry(&b,51044,10,1,v,8);
    if (b.entry!=10+30*12) return 0;
    lc_dng_u16(h+512,4);b.entry=514;
    lc_dng_u32(v,1000);lc_dng_u32(v+4,m->tvr);lc_dng_entry(&b,33434,5,1,v,8);
    lc_dng_u32(v,m->aperture);lc_dng_u32(v+4,10);lc_dng_entry(&b,33437,5,1,v,8);
    lc_dng_scalar(&b,34855,3,m->iso);
    lc_dng_u32(v,m->focal);lc_dng_u32(v+4,1);lc_dng_entry(&b,37386,5,1,v,8);
    return b.ok && b.entry==514+4*12;
}
static void lc_dng_swap_copy(uint8_t *dst,const uint8_t *src,unsigned bytes)
{
    for (unsigned i=0;i<bytes;i+=2) { dst[i]=src[i+1];dst[i+1]=src[i]; }
}
#endif
