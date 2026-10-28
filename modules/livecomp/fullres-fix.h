/* GPL-2.0-or-later. Full-res output fixes for the EOS M (LCM19).
 * Pure functions on packed Canon RAW14 rasters: host-tested.
 *
 * Focus pixels: EOS M (same sensor as 650D/700D/100D) has PDAF pixels that
 * appear as a dot grid in LV RAW. For the full-res raster (5280x3506) the
 * positions are an exact 24x30 tile, verified against MLV App's map
 * pixel_maps/80000331_5280x3506.fpm (200,942 points, identical set) and
 * against our own frames: (x%24,y%30) in the table below, for x in
 * [72,5279] and y in [31,3499]. All are red or blue sites and none of
 * their same-colour neighbours (x+-2, y+-2) is a focus pixel.
 *
 * White balance: DNG AsShotNeutral for colour temperatures 2500-10000 K
 * (step 100), computed offline as ColorMatrix1(EOS M, D65) * XYZ(white),
 * white on the Planckian locus below 4000 K, CIE daylight above 5000 K and
 * blended between (monotonic), normalised to green. 5200 K gives
 * 0.464/0.635, close to the core's fixed daylight 0.474/0.624. */
#ifndef LIVECOMP_FULLRES_FIX_H
#define LIVECOMP_FULLRES_FIX_H
#include <stdint.h>
#include "raw14-max.h"

#define LC_FP_X0 72u
#define LC_FP_X1 5279u
#define LC_FP_Y0 31u
#define LC_FP_Y1 3499u
static const uint8_t lc_fp_tile[8][2]={{0,4},{5,23},{6,18},{11,19},{12,16},{17,23},{18,18},{23,1}};

static inline unsigned lc_r14_get(const uint8_t *buf,unsigned pitch,unsigned x,unsigned y)
{
    uint16_t p[8];lc_raw14_unpack((const uint16_t *)(buf+y*pitch+(x/8)*14),p);return p[x%8];
}
static inline void lc_r14_set(uint8_t *buf,unsigned pitch,unsigned x,unsigned y,unsigned v)
{
    uint16_t p[8],*b=(uint16_t *)(buf+y*pitch+(x/8)*14);
    lc_raw14_unpack(b,p);p[x%8]=(uint16_t)(v&16383u);lc_raw14_pack(b,p);
}
/* Replace every focus pixel by the mean of its 4 same-colour neighbours.
 * Only for the 5280-wide full-res raster; returns the number fixed. */
static unsigned lc_fix_focus_pixels(uint8_t *buf,unsigned width,unsigned height,unsigned pitch)
{
    if (width!=5280 || height<LC_FP_Y1+3) return 0;
    unsigned n=0;
    for (unsigned y=LC_FP_Y0;y<=LC_FP_Y1;y++) {
        for (unsigned t=0;t<8;t++) {
            if (lc_fp_tile[t][1]!=y%30) continue;
            unsigned x=LC_FP_X0+((lc_fp_tile[t][0]+24-LC_FP_X0%24)%24);
            for (;x<=LC_FP_X1;x+=24) {
                unsigned l=lc_r14_get(buf,pitch,x-2,y),u=lc_r14_get(buf,pitch,x,y-2),d=lc_r14_get(buf,pitch,x,y+2);
                unsigned r=x+2<width ? lc_r14_get(buf,pitch,x+2,y):l;
                lc_r14_set(buf,pitch,x,y,(l+r+u+d+2)/4);n++;
            }
        }
    }
    return n;
}

/* Live Bulb column fixed-pattern correction (LCM56).
 * Every column of the sensor has a small fixed offset (~2 DN per frame on
 * the EOS M at ISO 100, partly with an 8-column period). A maximum keeps it
 * at one frame's size, but a sum of N frames multiplies it by N while
 * random noise only grows as sqrt(N): vertical bars in the shadows (LCM55
 * LVBL0009: ~23 DN at 12 frames). The same offsets are in the top optical
 * black rows of the sum (correlation 0.8 with the dark image, 0.88 between
 * two sessions), so they are measured there and subtracted from every row.
 * Rows 16..26 only: the first OB rows are much noisier (std 160-340).
 * Measure: per column the mean of rows y0..y1-1 without its minimum and
 * maximum (hot pixels), x16; minus the mean of all columns of the same
 * parity (CFA), rounded to DN. Returns the rms offset x10. */
#define LC_CF_Y0 16u
#define LC_CF_Y1 27u
static unsigned lc_colfix_measure(const uint8_t *buf,unsigned pitch,unsigned width,unsigned y0,unsigned y1,int32_t *off)
{
    const unsigned n=y1-y0;
    if (n<4) return 0;
    int32_t ref[2]={0,0};                /* <= 2640 x 16 x 16383: fits */
    for (unsigned x=0;x<width;x++) {
        unsigned s=0,mn=16383,mx=0;
        for (unsigned y=y0;y<y1;y++) {
            const unsigned v=lc_r14_get(buf,pitch,x,y);
            s+=v;mn=v<mn ? v:mn;mx=v>mx ? v:mx;
        }
        off[x]=(int32_t)(((s-mn-mx)*16u+(n-2)/2)/(n-2));
        ref[x&1]+=off[x];
    }
    const int32_t r0=ref[0]/(int32_t)((width+1)/2),r1=ref[1]/(int32_t)(width/2);
    uint32_t sq=0;                       /* no 64-bit division in the module */
    for (unsigned x=0;x<width;x++) {
        const int32_t d=off[x]-(x&1 ? r1:r0);
        off[x]=d>=0 ? (d+8)/16:-((-d+8)/16);
        const uint32_t a=(uint32_t)(off[x]<0 ? -off[x]:off[x]);
        sq+=a>300u ? 90000u:a*a;         /* rms is diagnostic only */
    }
    uint32_t m=sq/width*100u+(sq%width)*100u/width,r=0,bit=1u<<30;   /* isqrt(100 x mean square) */
    while (bit>m) bit>>=2;
    while (bit) { if (m>=r+bit) { m-=r+bit;r=(r>>1)+bit; } else r>>=1; bit>>=2; }
    return r;
}
/* Subtract the column offsets from rows ya..yb-1 (packed in place); samples
 * at or above white stay (saturated), the rest are clamped to [0,white]. */
static __attribute__((optimize("O3"))) void lc_colfix_apply(uint8_t *buf,unsigned pitch,unsigned width,unsigned ya,unsigned yb,
    const int32_t *off,unsigned white)
{
    const int w=(int)(white>16383u ? 16383u:white);
    for (unsigned y=ya;y<yb;y++) {
        uint16_t *b=(uint16_t *)(buf+y*pitch);
        const int32_t *o=off;
        for (unsigned k=0;k<width/8;k++,b+=7,o+=8) {
            uint16_t p[8];lc_raw14_unpack(b,p);
            for (unsigned i=0;i<8;i++) {
                if ((int)p[i]>=w) continue;
                const int v=(int)p[i]-o[i];
                p[i]=(uint16_t)(v<0 ? 0:v>w ? w:v);
            }
            lc_raw14_pack(b,p);
        }
    }
}

/* LCM57: what LCM56 got wrong, and the refinement.
 * Camera test LVBL0012 (LCM56): the bars were weaker but still there (dark
 * column std 12 DN, 14 frames). Two reasons, measured on LVBL0005/0009/0012:
 * 1. The 8-column (one packed block) part of the pattern is not the same in
 *    the OB rows as in the image: per lane x%8 the image has about
 *    +5 -6 -10 +13 +3 +5 +3 -11 DN (12 frames, the same in every session),
 *    the OB rows 16..26 +6 -4 -7 +3 +6 +1 -7 -2. LCM56 subtracted the OB one,
 *    so lanes 3, 6 and 7 stayed (or got worse).
 * 2. 9 OB samples per column (trimmed) leave ~9 DN of noise in each column's
 *    offset, and that noise becomes new full-height bars.
 * So the OB rows only give the non-periodic part; the lanes and a per-column
 * refinement come from flat dark areas of the image itself: in rows with
 * y%8<2 (both CFA rows), for every sample whose 9 same-colour neighbours
 * x-8..x+8 are all below black+LC_CF_T, d = v - mean(v[x+-2,4,6,8]) (after
 * the OB offsets). A column offset passes this high-pass unchanged, flat
 * scene and any linear gradient cancel. Per column: mean d (S8/n, x8);
 * per lane: the mean of all of them (parity-mean zero). Final offset per
 * column = OB non-periodic + image lane + w*(column mean - lane), where
 * w = n*vob/(n*vob+vpix) weighs the image estimate against the OB one
 * (vob: noise variance of the OB estimate, vpix: of one d sample), so a
 * bright scene falls back to the OB value. Fewer than LC_CF_LANE_MIN dark
 * samples in a lane: the OB lanes are kept (LCM56 behaviour).
 * Offline on LVBL0005/0009 (estimate from one half, scored on the other):
 * column std 9-11 DN (LCM56) -> 4.3-7.6 DN; LVBL0012 on top of LCM56:
 * 12.1 -> 5.5 DN. */
#define LC_CF_T        1024    /* dark: all 9 samples below black+T */
#define LC_CF_CLIP     200     /* |d| at or above: edge or hot pixel, skipped */
#define LC_CF_LANE_MIN 4000u
#define LC_CF_XA       80u     /* first column with x-8 in the active area */
static inline int32_t lc_rdiv(int32_t a,int32_t b) { return a>=0 ? (a+b/2)/b:-((-a+b/2)/b); }
/* Lane (x%8) means of the OB offsets; they are removed from off (now the
 * non-periodic part). lane[] in DN. */
static void lc_colfix_split_lanes(int32_t *off,unsigned width,int32_t *lane)
{
    for (unsigned k=0;k<8;k++) {
        int32_t s=0;for (unsigned x=k;x<width;x+=8) s+=off[x];
        lane[k]=lc_rdiv(s,(int32_t)(width/8));
    }
    for (unsigned x=0;x<width;x++) off[x]-=lane[x&7];
}
/* Noise variance (DN^2) of the trimmed OB mean of rows y0..y1-1. */
static unsigned lc_colfix_ob_var(const uint8_t *buf,unsigned pitch,unsigned width,unsigned y0,unsigned y1)
{
    const unsigned n=y1-y0;
    if (n<4) return 1000;
    uint32_t acc=0;
    for (unsigned x=0;x<width;x++) {
        const int v0=(int)lc_r14_get(buf,pitch,x,y0);
        int32_t s=0;uint32_t q=0;
        for (unsigned y=y0;y<y1;y++) {
            int u=(int)lc_r14_get(buf,pitch,x,y)-v0;
            u=u<-1000 ? -1000:u>1000 ? 1000:u;
            s+=u;q+=(uint32_t)(u*u);
        }
        const uint32_t v=((uint32_t)n*q-(uint32_t)(s*s))/(n*(n-1));
        acc+=v>100000u ? 100000u:v;          /* <= 5280 x 1e5: fits */
    }
    const unsigned v=acc/width/(n-2);
    return v<1 ? 1:v>4000 ? 4000:v;
}
struct lc_colfix_scan {
    int32_t *s8;         /* [width] sum of 8*d per column */
    uint16_t *n;         /* [width] samples per column */
    int16_t *row;        /* [width] scratch */
    uint32_t sq,cnt;     /* sum of (d^2>>8), samples */
    int lr_max;          /* LCM60: |right sum - left sum| limit (0: off), lc_colfix_lr_max() */
};
/* LCM60: a column offset looks the same from the left and the right
 * neighbours, a vertical scene edge does not. LCM59 LVBL0021 (2 frames,
 * weak OB estimate): the edge of a dark object in the shadows biased d in
 * every row and the refinement put -31/+32 DN bands along it. Samples whose
 * left and right neighbour means differ by 2 sigma or more (sigma from the
 * OB rows: pixel variance = 9 x vob) are skipped. Offline: the band
 * -31/+32 -> -6/+8 DN; LVBL0005/0009 cross-validation slightly better. */
static int lc_colfix_lr_max(unsigned vob)
{
    unsigned v=9u*vob,s=0;
    while ((s+1)*(s+1)<=v && s<4000) s++;
    const int m=4*2*(int)(s<4 ? 4:s);                 /* sums of 4: 4 x (2 sigma) */
    return m;
}
/* One row: unpack, subtract the OB offsets, accumulate d where flat and dark. */
static __attribute__((optimize("O3"))) void lc_colfix_scan_row(struct lc_colfix_scan *sc,const uint8_t *buf,unsigned pitch,
    unsigned width,unsigned y,const int32_t *off,unsigned black)
{
    const uint16_t *b=(const uint16_t *)(buf+y*pitch);
    int16_t *row=sc->row;
    for (unsigned k=0;k<width/8;k++,b+=7) {
        uint16_t p[8];lc_raw14_unpack(b,p);
        for (unsigned i=0;i<8;i++) row[k*8+i]=(int16_t)((int)p[i]-off[k*8+i]);
    }
    const int lim=(int)black+LC_CF_T;
    uint32_t sq=0;
    for (unsigned x=LC_CF_XA;x+8<width;x++) {
        const int c=row[x];
        if (c>=lim) continue;
        const int a2=row[x-2],b2=row[x+2],a4=row[x-4],b4=row[x+4],a6=row[x-6],b6=row[x+6],a8=row[x-8],b8=row[x+8];
        if (a2>=lim||b2>=lim||a4>=lim||b4>=lim||a6>=lim||b6>=lim||a8>=lim||b8>=lim) continue;
        if (sc->lr_max) {
            const int lr=(b2+b4+b6+b8)-(a2+a4+a6+a8);
            if (lr>=sc->lr_max || lr<=-sc->lr_max) continue;
        }
        const int d8=8*c-(a2+b2+a4+b4+a6+b6+a8+b8);
        if (d8>=8*LC_CF_CLIP || d8<=-8*LC_CF_CLIP) continue;
        sc->s8[x]+=d8;sc->n[x]++;
        const int d=d8>>3;sq+=(uint32_t)(d*d);sc->cnt++;
    }
    sc->sq+=sq>>8;
}
/* Final offsets (in place, off = OB non-periodic on entry). Returns 1 when
 * the image lanes were used. lane_img[] (x10 DN) for the LOG. The column
 * means are a high-pass: d at x also contains -mean(e at x+-2..8) of the
 * neighbours' remaining OB error e, so one Jacobi step is added:
 * e = w*(r + mean of the neighbours' w*r) (host test: rms error 2.3 -> 1.9 DN;
 * more steps give little: what is left is the slow part of the OB noise,
 * wider than the +-8 neighbourhood, much less visible than single columns).
 * s8 is overwritten with r (x8). */
static int lc_colfix_combine(int32_t *off,unsigned width,struct lc_colfix_scan *sc,const int32_t *lane_ob,
    unsigned vob,int32_t *lane_img_x10,unsigned *vpix_out)
{
    const unsigned vpix=sc->cnt>=256 ? sc->sq/(sc->cnt>>8):100000u;
    *vpix_out=vpix;
    int32_t l8[8];int img=1;
    for (unsigned k=0;k<8;k++) {
        int32_t s=0;uint32_t m=0;
        for (unsigned x=k;x<width;x+=8) { s+=sc->s8[x];m+=sc->n[x]; }
        if (m<LC_CF_LANE_MIN) img=0;
        l8[k]=m ? lc_rdiv(s,(int32_t)m):0;
    }
    if (img) for (unsigned par=0;par<2;par++) {
        const int32_t m=lc_rdiv(l8[par]+l8[par+2]+l8[par+4]+l8[par+6],4);
        for (unsigned k=par;k<8;k+=2) l8[k]-=m;
    }
    for (unsigned k=0;k<8;k++) { if (!img) l8[k]=8*lane_ob[k]; lane_img_x10[k]=lc_rdiv(10*l8[k],8); }
    /* r8 = column mean - lane (x8), weighted: wr = w*r8 */
    int32_t *wr=sc->s8;
    for (unsigned x=0;x<width;x++) {
        const uint32_t n=sc->n[x];
        if (!n) { wr[x]=0;continue; }
        const int32_t r8=lc_rdiv(sc->s8[x],(int32_t)n)-l8[x&7];
        const int32_t w256=(int32_t)((256u*vob*n)/(vob*n+vpix));
        wr[x]=lc_rdiv(w256*r8,256);
    }
    for (unsigned x=0;x<width;x++) {
        int32_t t8=8*off[x]+l8[x&7];
        const uint32_t n=sc->n[x];
        if (n && x>=8 && x+8<width) {                          /* n>0 only in [XA, width-8) */
            const int32_t w256=(int32_t)((256u*vob*n)/(vob*n+vpix));
            const int32_t nb=wr[x-2]+wr[x+2]+wr[x-4]+wr[x+4]+wr[x-6]+wr[x+6]+wr[x-8]+wr[x+8];
            t8+=wr[x]+lc_rdiv(w256*nb,2048);                    /* w*r + w*mean(w*r) */
        }
        off[x]=lc_rdiv(t8,8);
    }
    return img;
}
/* AsShotNeutral R and B (x1e6, G=1e6), 2500..10000 K in 100 K steps. */
static const int32_t lc_wb_neutral[76][2]={
    {823981,322381},{792760,338564},{764710,354461},{739384,370062},{716406,385362},{695464,400359},
    {676298,415051},{658687,429442},{642444,443533},{627412,457328},{613456,470833},{600458,484053},
    {588320,496994},{576955,509661},{566286,522062},{556249,534201},{546072,543948},{536555,553119},
    {527515,561955},{518916,570488},{510726,578748},{502915,586758},{495458,594539},{488328,602107},
    {481505,609476},{474968,616659},{469426,625759},{464159,634644},{459149,643316},{454379,651779},
    {449833,660035},{445497,668089},{441358,675944},{437403,683605},{433622,691076},{430003,698362},
    {426537,705465},{423216,712392},{420031,719145},{416975,725731},{414040,732152},{411219,738414},
    {408507,744520},{405898,750475},{403387,756282},{400968,761946},{398626,767498},{396369,772911},
    {394191,778191},{392090,783343},{390061,788371},{388100,793279},{386204,798070},{384371,802748},
    {382596,807317},{380879,811779},{379215,816139},{377602,820398},{376039,824561},{374523,828630},
    {373051,832608},{371623,836498},{370236,840301},{368889,844022},{367580,847662},{366307,851223},
    {365070,854708},{363866,858119},{362694,861458},{361553,864727},{360443,867928},{359361,871063},
    {358306,874134},{357279,877143},{356277,880091},{355300,882980},
};
/* Canon WB mode -> colour temperature (same mapping as mlv_dump). */
static inline unsigned lc_wb_kelvin(int wb_mode,int kelvin)
{
    switch (wb_mode) {
        case 9: case 0: return kelvin>=2500 && kelvin<=10000 ? (unsigned)kelvin:5500u;  /* KELVIN, AUTO */
        case 8: return 7000;   /* SHADE */
        case 2: return 6000;   /* CLOUDY */
        case 3: return 3200;   /* TUNGSTEN */
        case 4: return 4000;   /* FLUORESCENT */
        default: return 5500;  /* SUNNY, FLASH, unknown */
    }
}
/* neutral[0..2] in 1e6 units (G = 1e6). CUSTOM (6) uses Canon's gains. */
static inline void lc_wb_neutral_for(int wb_mode,int kelvin,unsigned gr,unsigned gg,unsigned gb,int32_t *neutral)
{
    if (wb_mode==6 && gr && gg && gb) {
        neutral[0]=(int32_t)((uint64_t)gg*1000000u/gr);neutral[1]=1000000;
        neutral[2]=(int32_t)((uint64_t)gg*1000000u/gb);return;
    }
    unsigned k=lc_wb_kelvin(wb_mode,kelvin),i=(k-2500+50)/100;
    if (i>75) i=75;
    neutral[0]=lc_wb_neutral[i][0];neutral[1]=1000000;neutral[2]=lc_wb_neutral[i][1];
}
#endif
