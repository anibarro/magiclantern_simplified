/* GPL-2.0-or-later. Speed benchmark (branch livecomp-speed, LCB1).
 * LCB2/3: also the hand-written ARM select-max kernels (raw14-max-asm.h:
 * per field, SWAR, per field with LDM) next to the C kernel, on four data
 * sets. Power saving is held off while it runs (LCB2: the first run was
 * interrupted).
 * Measures on the camera, with the same two 32 MB buffers a session uses
 * and Live View running (x1), what limits the shortest shutter:
 *  - memory: cached/uncached reads (word loop, LDM 4/8), two interleaved
 *    read streams (the floor of a merge), writes, memcpy, EDMAC copy;
 *  - the merge kernels on full frames: select-max with no / all / mixed
 *    changes, with the frame read cached or uncached; Live Bulb add;
 *  - the preview copy into the three LV buffers: memcpy from the uncached
 *    preview buffer (today), from a cached copy, and by EDMAC.
 * Every test runs in 8 bands with a 10 ms sleep between them (as the
 * session's merge) so the GUI stays alive; only the active time counts.
 * Result: DCIM/LIVE/LVBENCH.LOG and the Status line. Nothing else changes. */
#ifndef LIVECOMP_SPEED_BENCH_H
#define LIVECOMP_SPEED_BENCH_H
#include "edmac-memcpy.h"
#include "raw14-max-asm.h"

#define LCB_BANDS 8u
#define LCB_PV_BYTES (720u*480u*2u)
struct lcb_line { const char *name; unsigned us,bytes,extra; };
static struct lcb_line lcb_lines[56];
static unsigned lcb_n;
static uint8_t *lcb_a,*lcb_b;                    /* cached aliases */
static unsigned lcb_alias;

static inline uint8_t *lcb_unc(uint8_t *p) { return (uint8_t *)((unsigned)p|lcb_alias); }
static void lcb_add(const char *name,unsigned us,unsigned bytes,unsigned extra)
{
    if (lcb_n<COUNT(lcb_lines)) { lcb_lines[lcb_n].name=name;lcb_lines[lcb_n].us=us;lcb_lines[lcb_n].bytes=bytes;lcb_lines[lcb_n].extra=extra;lcb_n++; }
}
/* --- memory loops (ARMv5TE: no PLD on the ARM946, so plain bursts) --- */
static uint32_t __attribute__((noinline)) lcb_rd_word(const uint32_t *p,unsigned bytes)
{
    uint32_t s=0;
    for (unsigned n=bytes/16;n;n--,p+=4) s+=p[0]+p[1]+p[2]+p[3];
    return s;
}
static void __attribute__((noinline)) lcb_rd_ldm4(const uint32_t *p,unsigned bytes)
{
    unsigned n=bytes/16;
    asm volatile("1: ldmia %0!,{r3-r6}\n subs %1,%1,#1\n bne 1b\n"
        :"+r"(p),"+r"(n)::"r3","r4","r5","r6","cc","memory");
}
static void __attribute__((noinline)) lcb_rd_ldm8(const uint32_t *p,unsigned bytes)
{
    unsigned n=bytes/32;
    asm volatile("1: ldmia %0!,{r3-r8,r10,r12}\n subs %1,%1,#1\n bne 1b\n"
        :"+r"(p),"+r"(n)::"r3","r4","r5","r6","r7","r8","r10","r12","cc","memory");
}
/* Two streams, 16 bytes from each per step: what a merge must read. */
static void __attribute__((noinline)) lcb_rd_two(const uint32_t *a,const uint32_t *b,unsigned bytes)
{
    unsigned n=bytes/16;
    asm volatile("1: ldmia %0!,{r3-r6}\n ldmia %1!,{r7,r8,r10,r12}\n subs %2,%2,#1\n bne 1b\n"
        :"+r"(a),"+r"(b),"+r"(n)::"r3","r4","r5","r6","r7","r8","r10","r12","cc","memory");
}
static void __attribute__((noinline)) lcb_wr_stm8(uint32_t *p,unsigned bytes)
{
    unsigned n=bytes/32;
    asm volatile("mov r3,#0\n mov r4,#0\n mov r5,#0\n mov r6,#0\n mov r7,#0\n mov r8,#0\n mov r10,#0\n mov r12,#0\n"
        "1: stmia %0!,{r3-r8,r10,r12}\n subs %1,%1,#1\n bne 1b\n"
        :"+r"(p),"+r"(n)::"r3","r4","r5","r6","r7","r8","r10","r12","cc","memory");
}
/* --- band runner: fn(kind, band start byte, band bytes) --- */
static volatile uint32_t lcb_sink;
static unsigned lcb_changed;
static void lcb_do(unsigned kind,unsigned off,unsigned len)
{
    uint8_t *a=lcb_a+off,*b=lcb_b+off;
    switch (kind) {
    case 0: lcb_sink+=lcb_rd_word((const uint32_t *)(void *)b,len);break;
    case 1: lcb_rd_ldm4((const uint32_t *)(void *)b,len);break;
    case 2: lcb_rd_ldm8((const uint32_t *)(void *)b,len);break;
    case 3: lcb_rd_ldm8((const uint32_t *)(void *)lcb_unc(b),len);break;
    case 4: lcb_rd_two((const uint32_t *)(void *)a,(const uint32_t *)(void *)b,len);break;
    case 5: lcb_rd_two((const uint32_t *)(void *)a,(const uint32_t *)(void *)lcb_unc(b),len);break;
    case 6: lcb_wr_stm8((uint32_t *)(void *)a,len);break;
    case 7: lcb_wr_stm8((uint32_t *)(void *)lcb_unc(a),len);break;
    case 8: memcpy(a,b,len);break;
    case 9: lcb_changed+=lc_raw14_max_sel((uint32_t *)(void *)a,(const uint32_t *)(void *)b,len/28);break;
    case 10: lcb_changed+=lc_raw14_max_sel((uint32_t *)(void *)a,(const uint32_t *)(void *)lcb_unc(b),len/28);break;
    case 11: lc_raw14_add_sel((uint32_t *)(void *)a,(const uint32_t *)(void *)b,len/28,2047,15000);break;
    case 12: lcb_changed+=lc_raw14_max_field((uint32_t *)(void *)a,(const uint32_t *)(void *)b,len/28);break;
    case 13: lcb_changed+=lc_raw14_max_swar((uint32_t *)(void *)a,(const uint32_t *)(void *)b,len/28);break;
    case 14: lcb_changed+=lc_raw14_max_ldm((uint32_t *)(void *)a,(const uint32_t *)(void *)b,len/28);break;
    }
}
static unsigned lcb_run(unsigned kind,unsigned bytes)
{
    unsigned us=0;
    lcb_changed=0;
    for (unsigned k=0;k<LCB_BANDS;k++) {
        unsigned b0=(bytes/28)*k/LCB_BANDS*28,b1=(bytes/28)*(k+1)/LCB_BANDS*28;
        if (k==LCB_BANDS-1) b1=bytes;
        sync_caches();
        const unsigned t0=(unsigned)get_us_clock();
        lcb_do(kind,b0,b1-b0);
        us+=(unsigned)get_us_clock()-t0;
        msleep(10);
        if (ml_shutdown_requested) break;
    }
    return us;
}
/* Pseudo-random 14-bit noise (mean 2047, ~+-64) packed into the frame
 * layout, so the select kernel sees realistic data. */
static void lcb_fill_noise(uint8_t *p,unsigned bytes,uint32_t seed,unsigned base)
{
    for (unsigned off=0;off+14<=bytes;off+=14) {
        uint16_t v[8];
        for (unsigned i=0;i<8;i++) { seed=seed*1664525u+1013904223u;v[i]=(uint16_t)(base+((seed>>16)&127)); }
        lc_raw14_pack((uint16_t *)(void *)(p+off),v);
        if ((off&0x3FFFF)==0) msleep(1);
    }
}
static void lcb_text(const char *t) { bmp_printf(FONT(FONT_MED,COLOR_WHITE,COLOR_BLACK),20,420,"%-50s",t); }

static void lcb_write_log(struct card_info *card,const char *status,unsigned total_ms)
{
    char dir[24],path[FIO_MAX_PATH_LENGTH];
    if (!card || ml_shutdown_requested || !output_dir(card,dir,sizeof(dir))) return;
    snprintf(path,sizeof(path),"%s/LVBENCH.LOG",dir);
    FILE *f=FIO_CreateFile(path);
    if (!f) return;
    static char t[4096];
    int n=snprintf(t,sizeof(t),LIVECOMP_BUILD_ID "\nSpeed benchmark (LV running, x1)\nStatus: %s\nTotal_ms: %d\n"
        "Test: us, bytes, MB/s, extra (changed_pct for merges)\n",status,total_ms);
    for (unsigned i=0;i<lcb_n && n<(int)sizeof(t)-120;i++) {
        const struct lcb_line *l=&lcb_lines[i];
        const unsigned mbs=l->us ? (unsigned)((uint64_t)l->bytes*1000000u/l->us/1048576u):0;
        n+=snprintf(t+n,sizeof(t)-n,"%s: %d,%d,%d,%d\n",l->name,l->us,l->bytes,mbs,l->extra);
    }
    FIO_WriteFile(f,t,n);
    FIO_CloseFile(f);
}
unsigned livecomp_bench_run(struct card_info *card,const char **status)
{
    static char msg[80];
    void *alloc[2]={NULL,NULL};
    lcb_n=0;lcb_alias=livecomp_camera.cache_alias_mask;
    const unsigned t_all=get_ms_clock();
    if (!lv || LV_PAUSED || lens_info.job_state || raw_lv_is_enabled()) { *status="Benchmark: Live View idle (x1), no RAW tools";return 0; }
    powersave_prohibit();                        /* LCB2: auto power-off interrupted a run */
    for (unsigned i=0;i<2;i++) alloc[i]=fio_malloc(FRLV_ALLOCATION);
    if (!alloc[0] || !alloc[1]) { *status="Benchmark: not enough memory";goto out; }
    lcb_a=(uint8_t *)physical((void *)FRLV_ALIGN((uintptr_t)alloc[0]));
    lcb_b=(uint8_t *)physical((void *)FRLV_ALIGN((uintptr_t)alloc[1]));
    const unsigned F=32395440u;                  /* one full-res frame */
    lcb_text("Benchmark: preparing test data (~10 s)");
    lcb_fill_noise(lcb_b,F,12345u,1983);
    /* Memory */
    lcb_text("Benchmark: memory");
    lcb_add("read_cached_words",lcb_run(0,F),F,0);
    lcb_add("read_cached_ldm4",lcb_run(1,F),F,0);
    lcb_add("read_cached_ldm8",lcb_run(2,F),F,0);
    lcb_add("read_uncached_ldm8",lcb_run(3,F),F,0);
    lcb_add("read_two_streams_cached",lcb_run(4,F),2*F,0);
    lcb_add("read_two_streams_frame_uncached",lcb_run(5,F),2*F,0);
    lcb_add("write_cached_stm8",lcb_run(6,F),F,0);
    lcb_add("write_uncached_stm8",lcb_run(7,F),F,0);
    lcb_add("memcpy_cached",lcb_run(8,F),F,0);
    /* Merge kernels: acc = A, frame = B (noise). For each kernel (C select,
     * ARM per field, ARM SWAR) four accumulators: white (nothing changes),
     * black (everything), independent noise (mixed), noise + 100 DN
     * ("steady": a long session, ~30 % of the pairs change). */
    lcb_text("Benchmark: merge kernels (~1 min)");
    static const char *kn[4][4]={
        {"max_sel_none_changed","max_sel_all_changed","max_sel_mixed","max_sel_steady"},
        {"asm_field_none_changed","asm_field_all_changed","asm_field_mixed","asm_field_steady"},
        {"asm_swar_none_changed","asm_swar_all_changed","asm_swar_mixed","asm_swar_steady"},
        {"asm_ldm_none_changed","asm_ldm_all_changed","asm_ldm_mixed","asm_ldm_steady"}};
    static const unsigned kk[4]={9,12,13,14};
    unsigned us;
    for (unsigned k=0;k<4 && !ml_shutdown_requested;k++) {
        snprintf(msg,sizeof(msg),"Benchmark: merge kernel %d/4",k+1);lcb_text(msg);
        sync_caches();memset(lcb_unc(lcb_a),0xff,F);
        us=lcb_run(kk[k],F);lcb_add(kn[k][0],us,2*F,lcb_changed*100/(F/28));
        sync_caches();memset(lcb_unc(lcb_a),0,F);
        us=lcb_run(kk[k],F);lcb_add(kn[k][1],us,2*F,lcb_changed*100/(F/28));
        lcb_fill_noise(lcb_a,F,777u,1983);
        us=lcb_run(kk[k],F);lcb_add(kn[k][2],us,2*F,lcb_changed*100/(F/28));
        lcb_fill_noise(lcb_a,F,4242u,2083);
        us=lcb_run(kk[k],F);lcb_add(kn[k][3],us,2*F,lcb_changed*100/(F/28));
    }
    lcb_fill_noise(lcb_a,F,4242u,1983);
    us=lcb_run(10,F);lcb_add("max_sel_mixed_frame_uncached",us,2*F,lcb_changed*100/(F/28));
    us=lcb_run(11,F);lcb_add("bulb_add_sel",us,2*F,100);
    /* Preview copy: 3 x 720x480 UYVY into the LV buffers' stand-ins. */
    lcb_text("Benchmark: preview copy");
    uint8_t *pv_unc=lcb_unc(lcb_a),*pv_cac=lcb_a,*dst=lcb_unc(lcb_b);
    unsigned t0=(unsigned)get_us_clock();
    for (unsigned i=0;i<3;i++) memcpy(dst+i*LCB_PV_BYTES,pv_unc,LCB_PV_BYTES);
    lcb_add("preview_copy3_memcpy_from_uncached",(unsigned)get_us_clock()-t0,3*LCB_PV_BYTES,0);
    sync_caches();
    t0=(unsigned)get_us_clock();
    for (unsigned i=0;i<3;i++) memcpy(dst+i*LCB_PV_BYTES,pv_cac,LCB_PV_BYTES);
    lcb_add("preview_copy3_memcpy_from_cached",(unsigned)get_us_clock()-t0,3*LCB_PV_BYTES,0);
    msleep(10);
    edmac_memcpy_res_lock();
    t0=(unsigned)get_us_clock();
    for (unsigned i=0;i<3;i++) edmac_memcpy(dst+i*LCB_PV_BYTES,pv_unc,LCB_PV_BYTES);
    lcb_add("preview_copy3_edmac",(unsigned)get_us_clock()-t0,3*LCB_PV_BYTES,0);
    /* EDMAC on a large block (4 MB) for reference */
    t0=(unsigned)get_us_clock();
    edmac_memcpy(dst,pv_unc,4u*1048576u);
    lcb_add("edmac_copy_4MB",(unsigned)get_us_clock()-t0,4u*1048576u,0);
    edmac_memcpy_res_unlock();
    {
        unsigned mix=0,two=0,sel=0,fld=0,swr=0,ldm=0;
        for (unsigned i=0;i<lcb_n;i++) {
            if (!strcmp(lcb_lines[i].name,"max_sel_mixed")) mix=lcb_lines[i].us;
            if (!strcmp(lcb_lines[i].name,"read_two_streams_cached")) two=lcb_lines[i].us;
            if (!strcmp(lcb_lines[i].name,"max_sel_steady")) sel=lcb_lines[i].us;
            if (!strcmp(lcb_lines[i].name,"asm_field_steady")) fld=lcb_lines[i].us;
            if (!strcmp(lcb_lines[i].name,"asm_swar_steady")) swr=lcb_lines[i].us;
            if (!strcmp(lcb_lines[i].name,"asm_ldm_steady")) ldm=lcb_lines[i].us;
        }
        snprintf(msg,sizeof(msg),"Benchmark: C %d ms, field %d, SWAR %d, LDM %d",sel/1000,fld/1000,swr/1000,ldm/1000);(void)mix;(void)two;
        *status=msg;
    }
out:
    for (unsigned i=0;i<2;i++) if (alloc[i]) fio_free(alloc[i]);
    powersave_permit();
    lcb_write_log(card,*status,get_ms_clock()-t_all);
    return 0;
}
#endif
