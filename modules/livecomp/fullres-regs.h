/* GPL-2.0-or-later. Pure register-list rewriting for the EOS M full-sensor
 * composite. No hardware access: host tests include this header directly.
 *
 * Values follow Bilal Fakhouri's crop_rec_4k_mlv_snd (5f0cda8), EOS M
 * CROP_PRESET_1X1 / CROP_Full_Res ("5208x3478 @ 3 FPS"), applied only in x5:
 *   CMOS[5]=0, CMOS[7]=0x3A0
 *   C0F06804=(RAW_V<<16)|RAW_H, C0F06824..30=RAW_H+0x32,
 *   C0F0713C=RAW_V+1, C0F07150=RAW_V-0x3A, timers A/B (register values)
 *   ADTG PowerSave/ReadOut timing and shutter blanking scaled to timer B
 *   C0F38024/C0F383D4/C0F383DC basic preview window (register values only).
 * Deliberately NOT included: EDMAC#24 redirect to 0x1595b00, EDMAC#9 RAM
 * patches, C0F08184 and HIV address hook. Those only enlarge Canon's preview
 * and write Canon-owned memory; RAW capture does not depend on them.
 * AI-assisted implementation (Claude); see DEVELOPMENT.md. */
#ifndef LIVECOMP_FULLRES_REGS_H
#define LIVECOMP_FULLRES_REGS_H
#include <stdint.h>

#define LC_FRR_RAW_H 0x538u
#define LC_FRR_RAW_V 0xDB2u
#define LC_FRR_TIMER_A 0x56Bu
#define LC_FRR_CLOCK 32000000u
#define LC_FRR_FPS_CHOICES 16u
#define LC_FRR_FPS_DEFAULT 1u        /* 2.5 s */
#define LC_FRR_TESTED 5u             /* entries 0..4 ran on the camera */
/* Shutter (= frame period) choices, in menu order; register values = timer-1.
 * period = (A+1)(B+1) / 32 MHz, every row exposed for the whole period (no
 * gap between frames). B is 16 bits, so long periods stretch timer A in
 * multiples of the reference 0x56B+1 = 1388 (k); the sensor readout lasts
 * ~150 ms x k at the end of each period. The composite needs the CPU merge
 * (1.5-1.9 s) plus the LCD preview to finish before the next readout:
 *  - 2 s: merge fits, a full preview does not -> rolling quarter strips.
 *  - 2.5 s and longer: full preview after every frame.
 * Shorter shutters (Olympus allows 1/2 s) cannot be merged without gaps on
 * the CPU and are not offered. 2-5 s are camera-tested (LCM16-LCM41,
 * k = 1 and 3); 6-60 s use the same scheme with k up to 22 (readout up to
 * ~3.3 s) and still need a camera check. */
struct lc_frr_timing { uint32_t timer_a,timer_b,period_ms; };
static const struct lc_frr_timing lc_frr_timing[LC_FRR_FPS_CHOICES]={
    {1387u,46107u,2000u},       /* 2 s,  k=1, rolling preview (LCM27) */
    {1387u,57635u,2500u},       /* 2.5 s, k=1, default (LCM25-41) */
    {4163u,23053u,3000u},       /* 3 s,  k=3 */
    {4163u,30739u,4000u},       /* 4 s,  k=3 (LCM39/40 sessions) */
    {4163u,38424u,5000u},       /* 5 s,  k=3 */
    {4163u,46109u,6000u},       /* 6 s,  k=3 */
    {4163u,61478u,8000u},       /* 8 s,  k=3 */
    {5551u,57636u,10000u},      /* 10 s, k=4 */
    {6939u,59941u,13000u},      /* 13 s, k=5 */
    {8327u,57636u,15000u},      /* 15 s, k=6 */
    {11103u,57636u,20000u},     /* 20 s, k=8 */
    {12491u,64040u,25000u},     /* 25 s, k=9 */
    {15267u,62876u,30000u},     /* 30 s, k=11 */
    {20819u,61478u,40000u},     /* 40 s, k=15 */
    {24983u,64040u,50000u},     /* 50 s, k=18 */
    {30535u,62876u,60000u},     /* 60 s, k=22 */
};

struct lc_frr_config {
    uint32_t raw_h,raw_v,timer_a,timer_b,blanking;
};
struct lc_frr_stats {
    uint32_t cmos_calls,cmos_changes,adtg_calls,adtg_changes,blanking_seen;
    uint32_t engio_calls,engio_changes,drv_changes,overflow;
    uint32_t orig_6804,orig_timer_a,orig_timer_b,orig_blanking;
};

static inline uint32_t lc_frr_nrzi_encode(uint32_t in_val)
{
    uint32_t out_val=0,old_bit=0;
    for (int num=0;num<31;num++) {
        uint32_t bit=in_val&(1u<<(30-num)) ? 1:0;
        if (bit!=old_bit) out_val|=1u<<(30-num);
        old_bit=bit;
    }
    return out_val;
}
static inline uint32_t lc_frr_nrzi_decode(uint32_t in_val)
{
    uint32_t val=0;
    if (in_val&0x8000) val|=0x8000;
    for (int num=0;num<31;num++) {
        uint32_t old_bit=(val&(1u<<(30-num+1)))>>1;
        val|=old_bit^(in_val&(1u<<(30-num)));
    }
    return val;
}
/* Exposure as a fraction of the frame, in timer-B line units. The reference
 * clamps blanking to [10, timer_b-2]; 100% therefore means blanking 10. */
static inline uint32_t lc_frr_blanking(uint32_t timer_b_reg,uint32_t pct)
{
    uint32_t total=timer_b_reg+1,exposure=total*pct/100u,blanking=total-exposure;
    if (blanking<10u) blanking=10u;
    if (blanking>total-2u) blanking=total-2u;
    return blanking;
}
/* The composite always exposes 100% of the frame: consecutive
 * sub-exposures leave no gap in a light trail. */
static inline void lc_frr_configure(struct lc_frr_config *c,unsigned fps_index)
{
    if (fps_index>=LC_FRR_FPS_CHOICES) fps_index=LC_FRR_FPS_DEFAULT;
    c->raw_h=LC_FRR_RAW_H;c->raw_v=LC_FRR_RAW_V;
    c->timer_a=lc_frr_timing[fps_index].timer_a;
    c->timer_b=lc_frr_timing[fps_index].timer_b;
    c->blanking=lc_frr_blanking(c->timer_b,100u);
}

/* CMOS list: 16-bit (reg<<12)|value entries, 0xFFFF terminated. Copied so
 * Canon's own list stays unchanged. Returns 0 on overflow (caller keeps the
 * original list). */
static inline int lc_frr_cmos(const uint16_t *in,uint16_t *out,unsigned cap,struct lc_frr_stats *st)
{
    unsigned n=0;
    st->cmos_calls++;
    for (;;) {
        if (n>=cap) { st->overflow++;return 0; }
        uint16_t v=in[n];
        if (v==0xFFFFu) { out[n]=v;return 1; }
        unsigned reg=v>>12;
        if (reg==5) { v=(uint16_t)((5u<<12)|0x000u);st->cmos_changes++; }
        else if (reg==7) { v=(uint16_t)((7u<<12)|0x3A0u);st->cmos_changes++; }
        out[n++]=v;
    }
}

/* ADTG list: 32-bit (reg<<16)|value entries, 0xFFFFFFFF terminated, for chip
 * select mask dst. Copied; only registers already present are replaced. */
struct lc_frr_adtg { uint32_t dst,reg,val; };
static inline unsigned lc_frr_adtg_table(const struct lc_frr_config *c,struct lc_frr_adtg *t)
{
    const uint32_t b=c->timer_b+1,end=c->raw_v;
    unsigned n=0;
    /* Blanking is NRZI-encoded on the wire, like the timing registers. */
    t[n++]=(struct lc_frr_adtg){6,0x8061,lc_frr_nrzi_encode(c->blanking)}; /* non-zoom */
    t[n++]=(struct lc_frr_adtg){6,0x805F,lc_frr_nrzi_encode(c->blanking)}; /* zoom */
    t[n++]=(struct lc_frr_adtg){6,0x8172,lc_frr_nrzi_encode(end+1)};
    t[n++]=(struct lc_frr_adtg){6,0x8178,lc_frr_nrzi_encode(end+1)};
    t[n++]=(struct lc_frr_adtg){6,0x8196,lc_frr_nrzi_encode(end+1)};
    t[n++]=(struct lc_frr_adtg){6,0x8173,lc_frr_nrzi_encode(b-1)};
    t[n++]=(struct lc_frr_adtg){6,0x8179,lc_frr_nrzi_encode(b-1)};
    t[n++]=(struct lc_frr_adtg){6,0x8197,lc_frr_nrzi_encode(b-1)};
    t[n++]=(struct lc_frr_adtg){6,0x82B6,lc_frr_nrzi_encode(end-1)};
    t[n++]=(struct lc_frr_adtg){6,0x82F8,lc_frr_nrzi_encode(end+1)};
    t[n++]=(struct lc_frr_adtg){6,0x82F9,lc_frr_nrzi_encode(b-1)};
    return n;
}
#define LC_FRR_ADTG_ENTRIES 11u
static inline int lc_frr_adtg(uint32_t cs,const uint32_t *in,uint32_t *out,unsigned cap,
                              const struct lc_frr_config *c,struct lc_frr_stats *st)
{
    struct lc_frr_adtg t[LC_FRR_ADTG_ENTRIES];
    unsigned entries=lc_frr_adtg_table(c,t),n=0,dst=cs&0xF;
    st->adtg_calls++;
    for (;;) {
        if (n>=cap) { st->overflow++;return 0; }
        uint32_t v=in[n];
        if (v==0xFFFFFFFFu) { out[n]=v;return 1; }
        uint32_t reg=v>>16;
        for (unsigned i=0;i<entries;i++) if (reg==t[i].reg && (dst&t[i].dst)) {
            if (reg==0x805F || reg==0x8061) {
                st->blanking_seen++;
                if (!st->orig_blanking) st->orig_blanking=lc_frr_nrzi_decode(v&0xFFFFu)|0x10000u;
            }
            v=(v&0xFFFF0000u)|(t[i].val&0xFFFFu);st->adtg_changes++;
        }
        out[n++]=v;
    }
}
/* ENGIO list: (register,value) pairs, 0xFFFFFFFF terminated. Rewritten in
 * place, as in the reference implementation. Originals are recorded once. */
static inline uint32_t lc_frr_engio_value(uint32_t reg,const struct lc_frr_config *c,int *hit)
{
    *hit=1;
    switch (reg) {
        case 0xC0F06804: return (c->raw_v<<16)+c->raw_h;
        case 0xC0F06824: case 0xC0F06828: case 0xC0F0682C: case 0xC0F06830: return c->raw_h+0x32;
        case 0xC0F0713C: return c->raw_v+1;
        case 0xC0F07150: return c->raw_v-0x3A;
        case 0xC0F06014: return c->timer_b;
        case 0xC0F06010: return c->timer_a;
        case 0xC0F06008: case 0xC0F0600C: return c->timer_a+(c->timer_a<<16);
    }
    *hit=0;return 0;
}
static inline void lc_frr_engio(uint32_t *list,unsigned cap,const struct lc_frr_config *c,struct lc_frr_stats *st)
{
    st->engio_calls++;
    for (unsigned n=0;n+1<cap && list[n]!=0xFFFFFFFFu;n+=2) {
        int hit;uint32_t reg=list[n],v=lc_frr_engio_value(reg,c,&hit);
        if (!hit) continue;
        if (reg==0xC0F06804 && !st->orig_6804) st->orig_6804=list[n+1];
        if (reg==0xC0F06010 && !st->orig_timer_a) st->orig_timer_a=list[n+1]|0x10000000u;
        if (reg==0xC0F06014 && !st->orig_timer_b) st->orig_timer_b=list[n+1]|0x10000000u;
        if (list[n+1]!=v) { list[n+1]=v;st->engio_changes++; }
    }
}
/* EngDrvOut: basic Canon preview window for the enlarged RAW. */
static inline int lc_frr_drv(uint32_t reg,uint32_t *val,const struct lc_frr_config *c)
{
    uint32_t v;
    switch (reg) {
        case 0xC0F38024: v=((c->raw_v-1)<<16)+c->raw_h-0x11;break;
        case 0xC0F383D4: v=(0x593u<<16)+0x217u;break;
        case 0xC0F383DC: v=(0x84Cu<<16)+0x31Fu;break;
        default: return 0;
    }
    if (*val==v) return 0;
    *val=v;return 1;
}
/* Same arithmetic as src/raw.c raw_lv_get_resolution for DIGIC V / EOS M. */
static inline void lc_frr_geometry(uint32_t top_left,uint32_t bottom_right,unsigned *w,unsigned *h)
{
    *w=((bottom_right&0xFFFF)-(top_left&0xFFFF))*4u;
    *h=(bottom_right>>16)-(top_left>>16)+1u;
}
#endif
