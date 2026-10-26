/* GPL-2.0-or-later. LCM15 full-sensor Live View readout gate for EOS M 2.0.2.
 * AI-assisted implementation (Claude), see FULLRES.md.
 *
 * Sequence (worker task, explicitly started with SET):
 *  1. Check ROM/RAM fingerprints, stock-core state and allocate two buffers
 *     (capture + accumulator: the capacity the camera reported in LCM9-13).
 *  2. Install CMOS/ADTG/ENGIO/EngDrvOut hooks. They are inert unless this
 *     gate is active AND Canon's PathDriveMode reports x5 zoom.
 *  3. Request x5 zoom. Canon reconfigures the sensor through the hooks.
 *  4. Verify the RAW window registers, measure frame cadence, then capture
 *     single frames with our own RAW slurp into the owned capture buffer
 *     (the stock core cannot: its fallback buffer is too small at full size).
 *     Every completed input is merged (copy, then exact per-pixel maximum).
 *     Frames that arrive while merging are not captured; this is a gate,
 *     not the continuous composite.
 *  5. Deactivate, return to x1, remove hooks, save DNG/LOG.
 * Uncertain DMA ownership retains the buffers and requires a reboot. */
#include "dryos.h"
#include "module.h"
#include "lens.h"
#include "raw.h"
#include "mem.h"
#include "patch.h"
#include "edmac.h"
#include "fps.h"
#include "shoot.h"
#include "propvalues.h"
#include "gui-common.h"
#include "powersave.h"
#include "camera-profile.h"
#include "fullres-lv.h"
#include "fullres-regs.h"
#include "raw14-max.h"
#include "lv-dng.h"

extern int raw_lv_is_enabled(void);
extern uint32_t raw_write_chan;
extern void edmac_raw_slurp(void *dst,int w,int h);

/* EOS M 2.0.2 addresses from the pinned crop_rec reference. RAM copies of
 * cmos_write/adtg_write and the ROM engio_write/EngDrvOut entry points. */
#define FRLV_CMOS_WRITE 0x2998Cu
#define FRLV_CMOS_INSTR 0xE92D41F0u
#define FRLV_ADTG_WRITE 0x2986Cu
#define FRLV_ADTG_INSTR 0xE92D43F8u
#define FRLV_ENGIO_WRITE 0xFF2C19ACu
#define FRLV_ENGIO_INSTR 0xE51FC15Cu
#define FRLV_ENGDRVOUT 0xFF2C1694u
#define FRLV_PATHDRIVE 0x892E8u          /* struct PathDriveMode; zoom at +0x12 */
#define FRLV_RAW_TYPE_REG 0xC0F37014u
#define FRLV_RAW_TYPE 0x10u              /* src/raw.c PREFERRED_RAW_TYPE, DIGIC V */
#define FRLV_ALLOCATION 32488032u        /* size proven twice on camera (LCM9-13) */
#define FRLV_GUARD 64u
#define FRLV_MAX_INPUTS 8u
#define FRLV_MAX_ATTEMPTS 14u        /* inputs + up to 6 partial frames */
#define FRLV_TRACE 24u
#define FRLV_VSYNC_RING 32u

enum { FE_ROM=1,FE_STATE=2,FE_MEMORY=4,FE_HOOK=8,FE_ZOOM=16,FE_REGS=32,FE_STALL=64,
       FE_DMA=128,FE_GUARD=256,FE_SIZE=512,FE_IO=1024,FE_RESTORE=2048,FE_CANCEL=4096,
       FE_RAWUSER=8192 };

static struct lc_frr_config cfg;
static struct lc_frr_stats stats;
static volatile unsigned active;           /* overrides may apply */
static volatile unsigned observing;        /* vsync bookkeeping */
static volatile unsigned vsyncs,last_vsync_ms,max_gap_ms;
static volatile unsigned want_capture,in_flight,capture_done;
static volatile unsigned capture_vsync,done_state,done_pointer,done_vsync,done_ms,start_ms,start_us;
static volatile unsigned vsync_us[FRLV_VSYNC_RING];
static volatile unsigned timer_fixes,timer_seen_b;
static uint8_t *capture_target;            /* uncached alias */
static uint32_t capture_pitch,capture_height;
static uint16_t cmos_copy[512];
static uint32_t adtg_copy[512];

/* PathDriveMode->zoom is the u16 at +0x12, i.e. the upper half of word +0x10. */
static unsigned pdm_zoom(void) { return (MEM(FRLV_PATHDRIVE+0x10)>>16)&0xFFFFu; }
static unsigned zoom5(void) { return lv && pdm_zoom()==5; }
static unsigned physical(const void *p)
{ return (unsigned)p&~livecomp_camera.cache_alias_mask; }

static void FAST cmos_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;(void)pc;
    if (!active || !zoom5()) return;
    if (lc_frr_cmos((const uint16_t *)regs[0],cmos_copy,COUNT(cmos_copy),&stats))
        regs[0]=(uint32_t)cmos_copy;
}
static void FAST adtg_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;(void)pc;
    if (!active || !zoom5()) return;
    if (lc_frr_adtg(regs[0],(const uint32_t *)regs[1],adtg_copy,COUNT(adtg_copy),&cfg,&stats))
        regs[1]=(uint32_t)adtg_copy;
}
static void FAST engio_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;(void)pc;
    if (!active || !zoom5()) return;
    lc_frr_engio((uint32_t *)regs[0],4096,&cfg,&stats);
}
static void FAST drv_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;(void)pc;
    if (!active || !zoom5()) return;
    if (lc_frr_drv(regs[0],&regs[1],&cfg)) stats.drv_changes++;
}

/* Called from the module CBR_VSYNC (before the core's raw_lv_vsync, which
 * stays idle because nobody requested core LV RAW). */
void livecomp_frlv_vsync(void)
{
    if (!observing) return;
    unsigned now=get_ms_clock();
    if (vsyncs && now-last_vsync_ms>max_gap_ms) max_gap_ms=now-last_vsync_ms;
    last_vsync_ms=now;vsync_us[vsyncs%FRLV_VSYNC_RING]=(unsigned)get_us_clock();vsyncs++;
    if (active && zoom5()) {
        /* If Canon restored its timer B outside engio_write, reapply ours the
         * same way the core's FPS override does on this camera. Logged. */
        uint32_t b=shamem_read(0xC0F06014)&0xFFFF;timer_seen_b=b;
        if (b!=cfg.timer_b) {
            timer_fixes++;
            EngDrvOutLV(0xC0F06008,cfg.timer_a+(cfg.timer_a<<16));
            EngDrvOutLV(0xC0F06014,cfg.timer_b);
            EngDrvOutLV(0xC0F06000,1);
        }
    }
    if (in_flight) {
        /* The slurp armed at the previous vsync covered one whole frame. */
        done_state=edmac_get_state(raw_write_chan);
        done_pointer=edmac_get_pointer(raw_write_chan);
        done_vsync=vsyncs;done_ms=now;in_flight=0;capture_done=1;
        return;
    }
    if (want_capture && active && zoom5() && !raw_lv_is_enabled()) {
        want_capture=0;
        EngDrvOut(FRLV_RAW_TYPE_REG,FRLV_RAW_TYPE);
        edmac_raw_slurp((void *)physical(capture_target),capture_pitch,capture_height);
        capture_vsync=vsyncs;start_ms=now;start_us=(unsigned)get_us_clock();in_flight=1;
    }
}

struct frlv_input { unsigned vsync,start_ms,done_ms,state,pointer,merge_us,tail_ok,changed_pct,ntrace;
    unsigned trace_us[FRLV_TRACE],trace_off[FRLV_TRACE]; };
struct frlv_result {
    unsigned error,stage,unsafe,rom_engdrvout,hooks;
    unsigned top_left,bottom_right,width,height,pitch,bytes;
    unsigned reg_6804,reg_6008,reg_6010,reg_6014,reg_38024,reg_6804_after;
    unsigned zoom_ms,cadence_frames,cadence_ms,max_gap;
    unsigned inputs,merged;
    struct frlv_input in[FRLV_MAX_ATTEMPTS];
    unsigned partials,vsync_count,vsync_first;
    unsigned vsync_us[FRLV_VSYNC_RING];
    unsigned black,ob_stdev_x10,center_mean,max_value;
    unsigned col_band[12],row_band[10];
    unsigned memcpy_us,saved,save_stage,save_size;
    unsigned fps_x1000,expo_pct,timer_fixes,timer_seen_b,pdm[6];
    char dng[FIO_MAX_PATH_LENGTH],log[FIO_MAX_PATH_LENGTH];
};

static int choose_paths(struct frlv_result *r,struct card_info *card)
{
    uint32_t size;
    for (unsigned i=0;i<10000;i++) {
        snprintf(r->dng,sizeof(r->dng),"%s:/LVFR%04d.DNG",card->drive_letter,i);
        snprintf(r->log,sizeof(r->log),"%s:/LVFR%04d.LOG",card->drive_letter,i);
        if (FIO_GetFileSize(r->dng,&size) && FIO_GetFileSize(r->log,&size)) return 1;
    }
    return 0;
}
static int write_text(FILE *f,const char *text,int n)
{ return n>0 && FIO_WriteFile(f,text,n)==n; }
/* Rewritten at each stage so an interrupted run still leaves evidence. */
static void write_log(const struct frlv_result *r,const char *status)
{
    if (ml_shutdown_requested) return;
    FILE *f=FIO_CreateFile(r->log);
    if (!f) return;
    static char t[2048];int ok=1;
    int n=snprintf(t,sizeof(t),LIVECOMP_BUILD_ID "\nFull-sensor LV readout gate\nStatus: %s\n"
        "Stage_error_unsafe: %d,%d,%d\n"
        "Stages: 1=checked 2=allocated 3=hooked 4=zoomed 5=verified 6=captured 7=restored 8=saved\n"
        "Errors: 1=ROM 2=state 4=memory 8=hook 16=zoom 32=registers 64=stall 128=DMA 256=guard 512=size 1024=IO 2048=restore 4096=cancel 8192=raw_user\n"
        "Requested_fps_x1000_exposure_pct_timerA_timerB_blanking: %d,%d,%x,%x,%d\n"
        "EngDrvOut_instr_hooks: %x,%d\n"
        "Regs_6800_6804_6008_6010_6014_38024: %x,%x,%x,%x,%x,%x\n"
        "Reg_6804_after_restore: %x\n"
        "Width_height_pitch_bytes: %d,%d,%d,%d\n"
        "Zoom_ms_cadence_frames_ms_max_gap_ms: %d,%d,%d,%d\n"
        "Timer_fixes_last_seen_B: %d,%x\n"
        "PathDriveMode_words: %x,%x,%x,%x,%x,%x\n",
        status,r->stage,r->error,r->unsafe,r->fps_x1000,r->expo_pct,cfg.timer_a,cfg.timer_b,cfg.blanking,
        r->rom_engdrvout,r->hooks,r->top_left,r->bottom_right,r->reg_6008,r->reg_6010,r->reg_6014,r->reg_38024,
        r->reg_6804_after,r->width,r->height,r->pitch,r->bytes,
        r->zoom_ms,r->cadence_frames,r->cadence_ms,r->max_gap,r->timer_fixes,r->timer_seen_b,
        r->pdm[0],r->pdm[1],r->pdm[2],r->pdm[3],r->pdm[4],r->pdm[5]);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),
        "Hook_cmos_calls_changes: %d,%d\nHook_adtg_calls_changes_blanking_seen: %d,%d,%d\n"
        "Hook_engio_calls_changes_drv_changes_overflow: %d,%d,%d,%d\n"
        "Canon_orig_6804_timerA_timerB_blanking: %x,%x,%x,%x\n"
        "Inputs_merged_memcpy_us: %d,%d,%d\n"
        "Black_OBstdev_x10_center_mean_max: %d,%d,%d,%d\n"
        "Col_band_means_16px: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Row_band_means_8px: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Saved_stage_size: %d,%d,%d\nDNG: %s\n",
        stats.cmos_calls,stats.cmos_changes,stats.adtg_calls,stats.adtg_changes,stats.blanking_seen,
        stats.engio_calls,stats.engio_changes,stats.drv_changes,stats.overflow,
        stats.orig_6804,stats.orig_timer_a,stats.orig_timer_b,stats.orig_blanking,
        r->inputs,r->merged,r->memcpy_us,r->black,r->ob_stdev_x10,r->center_mean,r->max_value,
        r->col_band[0],r->col_band[1],r->col_band[2],r->col_band[3],r->col_band[4],r->col_band[5],
        r->col_band[6],r->col_band[7],r->col_band[8],r->col_band[9],r->col_band[10],r->col_band[11],
        r->row_band[0],r->row_band[1],r->row_band[2],r->row_band[3],r->row_band[4],
        r->row_band[5],r->row_band[6],r->row_band[7],r->row_band[8],r->row_band[9],
        r->saved,r->save_stage,r->save_size,r->dng);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Partials: %d\nAttempt_vsync_start_done_ms_state_pointer_tail_merge_us_changed_pct:\n",r->partials);
    ok&=write_text(f,t,n);
    for (unsigned i=0;i<r->inputs && i<FRLV_MAX_ATTEMPTS;i++) {
        const struct frlv_input *p=&r->in[i];
        n=snprintf(t,sizeof(t),"%d,%d,%d,%d,%d,%x,%d,%d,%d\n",i,p->vsync,p->start_ms,p->done_ms,
            p->state,p->pointer,p->tail_ok,p->merge_us,p->changed_pct);
        ok&=write_text(f,t,n);
    }
    ok&=write_text(f,"DMA_trace_attempt: us_after_arm:bytes ...\n",42);
    for (unsigned i=0;i<r->inputs && i<FRLV_MAX_ATTEMPTS;i++) {
        const struct frlv_input *p=&r->in[i];
        n=snprintf(t,sizeof(t),"T%d:",i);
        for (unsigned k=0;k<p->ntrace && n<(int)sizeof(t)-32;k++)
            n+=snprintf(t+n,sizeof(t)-n," %d:%d",p->trace_us[k],p->trace_off[k]);
        t[n++]='\n';
        ok&=write_text(f,t,n);
    }
    n=snprintf(t,sizeof(t),"Vsync_intervals_us (last %d):",FRLV_VSYNC_RING);
    for (unsigned k=1;k<FRLV_VSYNC_RING && k<r->vsync_count;k++) {
        unsigned a=(r->vsync_first+k-1)%FRLV_VSYNC_RING,b=(r->vsync_first+k)%FRLV_VSYNC_RING;
        n+=snprintf(t+n,sizeof(t)-n," %d",r->vsync_us[b]-r->vsync_us[a]);
    }
    t[n++]='\n';ok&=write_text(f,t,n);
    FIO_CloseFile(f);
    (void)ok;
}

static int base_state_ok(const volatile unsigned *cancel,struct card_info *card)
{
    return !*cancel && !ml_shutdown_requested && get_ml_card()==card && lv && !LV_PAUSED &&
        is_movie_mode() && !RECORDING && !lens_info.job_state;
}
static unsigned wait_vsyncs(unsigned count,unsigned timeout_ms,const volatile unsigned *cancel)
{
    unsigned start=vsyncs,t0=get_ms_clock();
    while (vsyncs-start<count) {
        if (*cancel || ml_shutdown_requested || get_ms_clock()-t0>timeout_ms) return 0;
        msleep(20);
    }
    return 1;
}
/* 14-bit sample (x,y) from a packed, cached Canon RAW14 buffer. */
static unsigned sample(const uint8_t *buf,unsigned pitch,unsigned x,unsigned y)
{
    uint16_t p[8];
    lc_raw14_unpack((const uint16_t *)(buf+y*pitch+(x/8)*14),p);
    return p[x%8];
}
static void statistics(struct frlv_result *r,const uint8_t *buf)
{
    /* Optical black estimate: columns 16..63, every 8th row. Bands describe
     * where the black border ends without assuming the DNG crop. */
    uint64_t sum=0,sq=0;unsigned n=0;
    for (unsigned y=40;y+40<r->height;y+=8) for (unsigned x=16;x<64;x+=3) {
        unsigned v=sample(buf,r->pitch,x,y);sum+=v;sq+=(uint64_t)v*v;n++;
    }
    if (n) {
        r->black=(unsigned)(sum/n);
        uint64_t var=sq/n-(uint64_t)r->black*r->black;
        unsigned s=0;while ((uint64_t)(s+1)*(s+1)<=var*100) s++;
        r->ob_stdev_x10=s;
    }
    for (unsigned b=0;b<12;b++) {
        uint64_t a=0;unsigned c=0;
        for (unsigned y=r->height/4;y<3*r->height/4;y+=64) for (unsigned x=b*16;x<b*16+16;x++) {
            a+=sample(buf,r->pitch,x,y);c++;
        }
        r->col_band[b]=c ? (unsigned)(a/c):0;
    }
    for (unsigned b=0;b<10;b++) {
        uint64_t a=0;unsigned c=0;
        for (unsigned y=b*8;y<b*8+8;y++) for (unsigned x=r->width/4;x<3*r->width/4;x+=64) {
            a+=sample(buf,r->pitch,x,y);c++;
        }
        r->row_band[b]=c ? (unsigned)(a/c):0;
    }
    uint64_t a=0;unsigned c=0,mx=0;
    for (unsigned y=r->height/2-64;y<r->height/2+64;y+=4) for (unsigned x=r->width/2-64;x<r->width/2+64;x+=4) {
        unsigned v=sample(buf,r->pitch,x,y);a+=v;c++;if (v>mx) mx=v;
    }
    r->center_mean=c ? (unsigned)(a/c):0;r->max_value=mx;
}
static int frlv_save_dng(struct frlv_result *r,const uint8_t *acc)
{
    struct lc_dng_metadata m={0};
    m.width=r->width;m.height=r->height;m.pitch=r->pitch;m.bytes=r->bytes;
    m.black=r->black ? r->black:2048;
    m.white=raw_info.white_level>(int)m.black && raw_info.white_level<=16383 ? (uint32_t)raw_info.white_level:15000u;
    m.cfa=raw_info.cfa_pattern;
    /* Crop guess only (left OB ~72 px, top ~28 rows); full raster is stored. */
    m.active[0]=MIN(28u,r->height-2);m.active[1]=MIN(72u,r->width-8);
    m.active[2]=r->height;m.active[3]=r->width;
    memcpy(m.matrix,raw_info.color_matrix1,sizeof(m.matrix));
    m.illuminant=raw_info.calibration_illuminant1;
    m.iso=raw2iso(lens_info.raw_iso);m.tvr=MAX(1u,r->fps_x1000*100u/MAX(1u,r->expo_pct));
    m.fps=r->fps_x1000;m.aperture=lens_info.aperture;m.focal=lens_info.focal_len;
    r->save_stage=1;
    uint8_t *scratch=fio_malloc(16384);
    if (!scratch) return 0;
    /* DNG needs width%8==0. Otherwise store the raw raster (.RAW, no header,
     * DNG byte order) so the geometry can still be analysed offline. */
    unsigned header=LC_DNG_HEADER_BYTES;
    if (r->width%8) { header=0;memcpy(r->dng+strlen(r->dng)-3,"RAW",3); }
    else if (!lc_dng_header(scratch,&m)) { fio_free(scratch);r->save_stage=2;return 0; }
    r->save_stage=3;
    FILE *f=FIO_CreateFile(r->dng);
    if (!f || f==(void *)-1) { fio_free(scratch);return 0; }
    int ok=!header || FIO_WriteFile(f,scratch,header)==(int)header;
    r->save_stage=4;
    for (unsigned off=0;ok && off<r->bytes;off+=16384) {
        unsigned n=MIN(16384u,r->bytes-off);
        lc_dng_swap_copy(scratch,acc+off,n);
        ok=FIO_WriteFile(f,scratch,n)==(int)n;
        if (ml_shutdown_requested) ok=0;
    }
    FIO_CloseFile(f);fio_free(scratch);
    if (!ok) return 0;
    r->save_stage=5;
    uint32_t size=0;
    if (FIO_GetFileSize(r->dng,&size) || size!=header+r->bytes) return 0;
    r->save_size=size;r->save_stage=6;return 1;
}
static int guards_ok(const uint8_t *p,unsigned bytes)
{
    for (unsigned i=0;i<FRLV_GUARD;i++) if (p[(int)i-(int)FRLV_GUARD]!=0x5a || p[bytes+i]!=0x5a) return 0;
    return 1;
}

unsigned livecomp_frlv_run(struct card_info *card,const struct lc_frlv_options *o,
    const volatile unsigned *cancel,const char **status)
{
    static struct frlv_result r;
    memset(&r,0,sizeof(r));memset(&stats,0,sizeof(stats));
    lc_frr_configure(&cfg,o->fps_index,o->expo_index);
    r.fps_x1000=lc_frr_timing[o->fps_index<LC_FRR_FPS_CHOICES ? o->fps_index:0].fps_x1000;
    const unsigned period_ms=1000000u/r.fps_x1000;
    r.expo_pct=lc_frr_expo_pct[o->expo_index<LC_FRR_EXPO_CHOICES ? o->expo_index:0];
    unsigned inputs=MIN(MAX(o->inputs,1u),FRLV_MAX_INPUTS);
    void *alloc[2]={0};uint8_t *cap=NULL,*acc=NULL;
    unsigned hooked[4]={0},power=0,locked=0,old_lock=0,zoomed=0,dma_uncertain=0;
    const char *msg="Full-res readout: checking";
    *status=msg;
    want_capture=in_flight=capture_done=0;timer_fixes=0;

    /* 1: state and fingerprints. Only EOS M 2.0.2 reaches this (camera guard). */
    if (!card || !choose_paths(&r,card)) { r.error|=FE_IO;msg="No card/filename";goto out; }
    if (!base_state_ok(cancel,card) || lv_dispsize!=1 || raw_lv_is_enabled() ||
        raw_write_chan!=livecomp_camera.raw_channel) {
        r.error|=FE_STATE|(raw_lv_is_enabled() ? FE_RAWUSER:0);
        msg="Use Movie LV x1, Global Draw OFF, no other RAW tools";goto out;
    }
    if (fps_get_current_x1000()<20000) {
        r.error|=FE_STATE;msg="Turn ML FPS override OFF for this test";goto out;
    }
    r.rom_engdrvout=MEM(FRLV_ENGDRVOUT);
    for (unsigned i=0;i<6;i++) r.pdm[i]=MEM(FRLV_PATHDRIVE+4*i);
    if (MEM(FRLV_CMOS_WRITE)!=FRLV_CMOS_INSTR || MEM(FRLV_ADTG_WRITE)!=FRLV_ADTG_INSTR ||
        MEM(FRLV_ENGIO_WRITE)!=FRLV_ENGIO_INSTR ||
        pdm_zoom()!=1) {
        r.error|=FE_ROM;msg="Firmware fingerprint mismatch; nothing changed";goto out;
    }
    r.stage=1;

    /* 2: capture buffer + accumulator (exclusively owned, guarded). */
    for (unsigned i=0;i<2;i++) {
        alloc[i]=fio_malloc(FRLV_ALLOCATION);
        if (!alloc[i]) break;
    }
    if (!alloc[0]) { r.error|=FE_MEMORY;msg="No full-size buffer available";goto out; }
    cap=(uint8_t *)(((unsigned)alloc[0]+2*FRLV_GUARD-1)&~(FRLV_GUARD-1));
    if (alloc[1]) acc=(uint8_t *)physical((void *)(((unsigned)alloc[1]+FRLV_GUARD-1)&~(FRLV_GUARD-1)));
    unsigned capacity=FRLV_ALLOCATION-3*FRLV_GUARD;
    memset(cap-FRLV_GUARD,0x5a,FRLV_GUARD);
    r.stage=2;write_log(&r,"allocated");

    /* 3: hooks (inert until active && x5). */
    powersave_prohibit();power=1;
    old_lock=icu_uilock&0xffffu;
    if (old_lock&~UILOCK_SHUTTER) { r.error|=FE_STATE;msg="Camera controls busy; retry";goto out; }
    gui_uilock(old_lock|UILOCK_SHUTTER);locked=1;
    if (patch_hook_function(FRLV_CMOS_WRITE,FRLV_CMOS_INSTR,cmos_hook,"livecomp: full-res CMOS")) goto hook_fail;
    hooked[0]=1;
    if (patch_hook_function(FRLV_ADTG_WRITE,FRLV_ADTG_INSTR,adtg_hook,"livecomp: full-res ADTG")) goto hook_fail;
    hooked[1]=1;
    if (patch_hook_function(FRLV_ENGIO_WRITE,FRLV_ENGIO_INSTR,engio_hook,"livecomp: full-res timers")) goto hook_fail;
    hooked[2]=1;
    /* Preview window only: optional. Hooked only if the entry looks like a
     * normal prologue (STMFD or LDR R12,[PC]); RAW capture does not need it. */
    if (((r.rom_engdrvout&0xFFFF0000u)==0xE92D0000u || (r.rom_engdrvout&0xFFFFF000u)==0xE51FC000u) &&
        !patch_hook_function(FRLV_ENGDRVOUT,r.rom_engdrvout,drv_hook,"livecomp: full-res preview"))
        hooked[3]=1;
    r.hooks=3+hooked[3];r.stage=3;
    vsyncs=0;max_gap_ms=0;observing=1;active=1;
    write_log(&r,"hooked; entering x5");

    /* 4: x5 zoom makes Canon rewrite the sensor configuration via the hooks. */
    msg="Full-res readout: switching sensor";*status=msg;
    unsigned t0=get_ms_clock();
    set_lv_zoom(5);zoomed=1;
    while (!(lv_dispsize==5 && zoom5())) {
        if (*cancel || ml_shutdown_requested || get_ms_clock()-t0>6000) { r.error|=FE_ZOOM;break; }
        msleep(50);
    }
    r.zoom_ms=get_ms_clock()-t0;
    if (r.error) { msg="x5 zoom did not engage";goto restore; }
    if (!wait_vsyncs(3,3*period_ms+4000,cancel)) { r.error|=*cancel ? FE_CANCEL:FE_STALL;msg="No LV frames after switch";goto restore; }
    r.top_left=shamem_read(0xC0F06800);r.bottom_right=shamem_read(0xC0F06804);
    r.reg_6008=shamem_read(0xC0F06008);r.reg_6010=shamem_read(0xC0F06010);
    r.reg_6014=shamem_read(0xC0F06014);r.reg_38024=shamem_read(0xC0F38024);
    r.stage=4;write_log(&r,"zoomed");
    if (r.bottom_right!=((cfg.raw_v<<16)|cfg.raw_h)) { r.error|=FE_REGS;msg="Sensor window not applied";goto restore; }
    lc_frr_geometry(r.top_left,r.bottom_right,&r.width,&r.height);
    r.pitch=r.width*14/8;r.bytes=r.pitch*r.height;
    if (r.width<4000 || r.width>5280 || r.width%4 || r.height<3000 || r.height>3528 || r.bytes>capacity) {
        r.error|=FE_SIZE;msg="Unexpected full-res geometry; see LOG";goto restore;
    }
    memset(cap+r.bytes,0x5a,FRLV_GUARD);
    capture_pitch=r.pitch;capture_height=r.height;capture_target=cap;
    /* Cadence over a few frames (timer B may need one frame to settle). */
    {
        unsigned frames=MAX(3u,cfg.timer_b<10000 ? 6u:3u);
        wait_vsyncs(1,period_ms+4000,cancel);
        unsigned v0=vsyncs,m0=get_ms_clock();max_gap_ms=0;
        if (!wait_vsyncs(frames,frames*period_ms+4000,cancel)) { r.error|=*cancel ? FE_CANCEL:FE_STALL;msg="LV stalled at full res";goto restore; }
        r.cadence_frames=vsyncs-v0;r.cadence_ms=get_ms_clock()-m0;r.max_gap=max_gap_ms;
    }
    r.stage=5;write_log(&r,"verified");

    /* 6: capture inputs. One frame per request; merges happen between. */
    msg="Full-res readout: capturing";*status=msg;
    for (unsigned attempt=0;r.merged<inputs && attempt<FRLV_MAX_ATTEMPTS;attempt++) {
        if (*cancel || ml_shutdown_requested || !zoom5() || raw_lv_is_enabled()) {
            r.error|=raw_lv_is_enabled() ? FE_RAWUSER:*cancel ? FE_CANCEL:FE_STATE;
            msg=*cancel ? "Stopped by user":"Mode changed during capture";break;
        }
        memset(cap+r.bytes-8,0x5a,8);   /* uncached alias: no dirty lines */
        sync_caches();
        struct frlv_input *p=&r.in[r.inputs++];
        capture_done=0;want_capture=1;
        unsigned w0=get_ms_clock(),limit=3*(1000000u/r.fps_x1000)+3000,last=~0u;
        /* Sample the DMA write pointer while the frame arrives: shows when
         * readout starts relative to the vsync and how long it lasts. */
        while (!capture_done && get_ms_clock()-w0<limit) {
            if (in_flight && p->ntrace<FRLV_TRACE) {
                unsigned off=edmac_get_pointer(raw_write_chan)-physical(cap);
                if (off!=last) {
                    p->trace_us[p->ntrace]=(unsigned)get_us_clock()-start_us;
                    p->trace_off[p->ntrace++]=off;last=off;
                }
            }
            msleep(10);
        }
        if (!capture_done) {
            want_capture=0;
            if (in_flight) dma_uncertain=1;
            r.error|=FE_STALL;msg="Frame not completed; see LOG";break;
        }
        p->vsync=capture_vsync;p->start_ms=start_ms;p->done_ms=done_ms;
        p->state=done_state;p->pointer=done_pointer;
        unsigned tail=0;for (unsigned k=0;k<8;k++) if (cap[r.bytes-8+k]!=0x5a) tail=1;
        p->tail_ok=tail;
        if (done_state) {
            /* Still active one frame later: give it one more frame, else stop. */
            wait_vsyncs(1,period_ms+3000,cancel);
            if (edmac_get_state(raw_write_chan)) { dma_uncertain=1;r.error|=FE_DMA;msg="RAW DMA did not finish";break; }
        }
        if (!guards_ok(cap,r.bytes)) { dma_uncertain=1;r.error|=FE_GUARD;msg="Buffer guard overwritten";break; }
        if (!tail) {
            /* Partial frame with an idle channel: record it and try the next
             * frame (diagnostic gate only; continuous mode must not do this). */
            r.partials++;
            if (r.partials>6) { r.error|=FE_DMA;msg="Repeated partial frames; see LOG";break; }
            write_log(&r,"partial frame");
            continue;
        }
        sync_caches();
        const uint8_t *src=(const uint8_t *)physical(cap);
        if (!r.merged) statistics(&r,src);
        if (acc) {
            unsigned s0=(unsigned)get_us_clock();
            if (!r.merged) { memcpy(acc,src,r.bytes);r.memcpy_us=(unsigned)get_us_clock()-s0;p->changed_pct=100; }
            else {
                unsigned blocks=r.bytes/14,changed=lc_raw14_max_sparse((uint16_t *)acc,(const uint16_t *)src,blocks);
                p->changed_pct=(unsigned)((uint64_t)changed*100/blocks);
            }
            p->merge_us=(unsigned)get_us_clock()-s0;
        }
        r.merged++;
        write_log(&r,"capturing");
    }
    if (r.merged<inputs && !r.error) { r.error|=FE_DMA;msg="Too many partial frames; see LOG"; }
    if (r.merged && !(r.error&~FE_CANCEL)) r.stage=6;

restore:
    {
        unsigned c=vsyncs;r.vsync_count=MIN(c,FRLV_VSYNC_RING);
        r.vsync_first=c>FRLV_VSYNC_RING ? c%FRLV_VSYNC_RING:0;
        for (unsigned k=0;k<FRLV_VSYNC_RING;k++) r.vsync_us[k]=vsync_us[k];
    }
    active=0;
    msg=r.error ? msg:"Full-res readout: restoring";*status=msg;
    /* Let any armed slurp complete before leaving the mode. */
    {
        unsigned w0=get_ms_clock();
        while ((want_capture || in_flight) && get_ms_clock()-w0<period_ms+4000) { want_capture=0;msleep(20); }
        if (in_flight || edmac_get_state(raw_write_chan)) dma_uncertain=1;
    }
    if (zoomed) {
        set_lv_zoom(1);
        unsigned w0=get_ms_clock();
        while (lv && lv_dispsize!=1 && get_ms_clock()-w0<6000) msleep(50);
        if (lv && lv_dispsize!=1) r.error|=FE_RESTORE;
        /* Canon may need a full slow frame to finish the switch back. */
        w0=get_ms_clock();
        while (shamem_read(0xC0F06804)==((cfg.raw_v<<16)|cfg.raw_h) && get_ms_clock()-w0<12000) msleep(50);
        wait_vsyncs(3,4000,cancel);
        r.reg_6804_after=shamem_read(0xC0F06804);
        if (r.reg_6804_after==((cfg.raw_v<<16)|cfg.raw_h)) r.error|=FE_RESTORE;
    }
    if (zoomed && !(r.error&FE_RESTORE)) r.stage=MAX(r.stage,7u);
    goto unhook;
hook_fail:
    r.error|=FE_HOOK;msg="Hook install failed; nothing switched";
unhook:
    active=0;
    for (int i=3;i>=0;i--) if (hooked[i]) {
        static const uint32_t sites[4]={FRLV_CMOS_WRITE,FRLV_ADTG_WRITE,FRLV_ENGIO_WRITE,FRLV_ENGDRVOUT};
        if (unpatch_memory(sites[i])) r.unsafe=1;
    }
    observing=0;
out:
    r.timer_fixes=timer_fixes;r.timer_seen_b=timer_seen_b;
    if (dma_uncertain) r.unsafe=1;
    /* With a single allocation, the last captured frame is the output. */
    if (!acc && cap && !r.unsafe) acc=(uint8_t *)physical(cap);
    else if (!r.unsafe && alloc[0]) { fio_free(alloc[0]);alloc[0]=NULL;cap=NULL; }
    if (acc && r.merged && !r.unsafe && !ml_shutdown_requested && get_ml_card()==card &&
        !(r.error&(FE_DMA|FE_GUARD|FE_SIZE|FE_REGS))) {
        *status="Full-res readout: saving DNG";
        r.saved=frlv_save_dng(&r,acc);
        if (!r.saved) r.error|=FE_IO;
        else r.stage=8;
    }
    if (!r.unsafe) for (unsigned i=0;i<2;i++) if (alloc[i]) fio_free(alloc[i]);
    if (locked) gui_uilock(old_lock);
    if (power) powersave_permit();
    if (r.unsafe) msg="Full-res: ownership uncertain; keep LOG, reboot";
    else if (!r.error) msg=r.saved ? "Full-res DNG saved; see LVFR LOG":"Full-res: no frame saved";
    else if (r.saved) msg="Full-res DNG saved with warnings; see LOG";
    *status=msg;
    if (r.log[0]) write_log(&r,msg);
    return r.unsafe;
}
