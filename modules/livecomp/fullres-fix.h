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
