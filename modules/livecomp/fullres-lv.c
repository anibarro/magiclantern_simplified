/* GPL-2.0-or-later. Full-sensor Live View composite for the EOS M 2.0.2.
 * AI-assisted implementation (Claude); design log and evidence in FULLRES.md.
 *
 * Sequence (worker task, started with SET from the livecomp menu):
 *  1. Check ROM/RAM fingerprints and the camera state; allocate two
 *     full-size buffers (capture + accumulator, 32,488,032 bytes each).
 *  2. Install CMOS/ADTG/ENGIO/EngDrvOut hooks. They are inert unless this
 *     session is active AND Canon's PathDriveMode reports x5 zoom.
 *  3. Request x5 zoom. Canon reconfigures the sensor through the hooks, which
 *     substitute the CROP_Full_Res values (5208x3478 active, pitch 9240) and
 *     the slow frame timers chosen in the menu (fullres-regs.h).
 *  4. Capture with our own RAW slurp (EDMAC 0x12, source 0) into the capture
 *     buffer, one whole frame per vsync period, re-armed after every frame.
 *     Each frame is merged into the accumulator with an exact per-pixel
 *     maximum on the CPU (raw14-max.h), in bands, while the next exposure is
 *     already running. Every frame is a 100% exposure of the frame period
 *     (no gap between sub-exposures). A 360x240 preview of the accumulator
 *     replaces Canon's LV image.
 *  5. Leave x5, remove the hooks, fix the PDAF pixels, save DNG + LOG + CSV.
 * If the RAW DMA does not finish, buffer ownership is uncertain: the buffers
 * are kept allocated and the user is told to reboot.
 *
 * Canon's own image-max engine was investigated as a faster merge (LCM20-40)
 * and found unusable while Live View runs; see FULLRES.md. */
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
#include "vram.h"
#include "fullres-gamma.h"
#include "notify_box.h"
#include "zebra.h"
#include "fullres-fix.h"

extern int raw_lv_is_enabled(void);
extern int battery_level_bars;   /* PROP_BATTERY_POWER: 0 = last (red) level */
extern uint32_t raw_write_chan;

/* Own RAW slurp (same steps as the core's edmac_raw_slurp) so the EDMAC
 * complete/abort/pop callbacks can be timestamped: partial frames (LCM15/17/18)
 * need to show whether the transfer completed early or was aborted. */
static volatile unsigned cb_kind,cb_us,cb_ptr,arm_us;
static void FAST slurp_cb(void *ctx)
{
    unsigned kind=(unsigned)ctx;
    if (!cb_kind) { cb_us=(unsigned)get_us_clock();cb_ptr=edmac_get_pointer(raw_write_chan); }
    cb_kind|=kind;
}
static void frlv_slurp(void *dst,int pitch,int height)
{
    cb_kind=0;cb_us=0;cb_ptr=0;arm_us=(unsigned)get_us_clock();
    RegisterEDmacCompleteCBR(raw_write_chan,slurp_cb,(void *)1);
    RegisterEDmacAbortCBR(raw_write_chan,slurp_cb,(void *)2);
    RegisterEDmacPopCBR(raw_write_chan,slurp_cb,(void *)4);
    ConnectWriteEDmac(raw_write_chan,0);
    struct edmac_info info={0};info.xb=pitch;info.yb=height-1;
    SetEDmac(raw_write_chan,dst,&info,EDMAC_2_BYTES_PER_TRANSFER);
    StartEDmac(raw_write_chan,0);
}

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
#define FRLV_ALIGN(a) (((a)+FRLV_GUARD-1)&~(uintptr_t)(FRLV_GUARD-1))
#define FRLV_VSYNC_RING 32u
#define FRLV_CSV_MAX 999u             /* frames with a CSV line; merging goes on */
#define FRLV_BANDS 8u                /* merge bands, scheduler tick between them */
#define FRLV_PREVIEW_MS 420u         /* measured render + copy, LCM23 */
#define FRLV_MARGIN_MS 80u           /* scheduling before the next vsync */
#define FRLV_PREVIEW_PARTS 4u        /* rolling preview strips when short of time */

enum { FE_ROM=1,FE_STATE=2,FE_MEMORY=4,FE_HOOK=8,FE_ZOOM=16,FE_REGS=32,FE_STALL=64,
       FE_DMA=128,FE_GUARD=256,FE_SIZE=512,FE_IO=1024,FE_RESTORE=2048,FE_CANCEL=4096,
       FE_RAWUSER=8192 };

static struct lc_frr_config cfg;
static struct lc_frr_stats stats;
static volatile unsigned active;           /* overrides may apply */
static volatile unsigned observing;        /* vsync bookkeeping */
static volatile unsigned vsyncs,last_vsync_ms,max_gap_ms;
static volatile unsigned in_flight;
/* Re-arm at every completion while cont_arm; frames_done counts them. */
static volatile unsigned cont_arm,frames_done;
static volatile unsigned done_cb_kind,done_cb_after,done_cb_ptr,done_arm_after_vsync,arm_vsync_delay,done_engio,done_adtg,last_vsync_us;
static volatile unsigned done_state,done_pointer,done_ms,done_vsync;
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
    last_vsync_ms=now;last_vsync_us=(unsigned)get_us_clock();vsync_us[vsyncs%FRLV_VSYNC_RING]=last_vsync_us;vsyncs++;
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
        done_cb_kind=cb_kind;done_cb_after=cb_kind ? cb_us-arm_us:0;done_cb_ptr=cb_ptr;done_arm_after_vsync=arm_vsync_delay;
        done_engio=stats.engio_calls;done_adtg=stats.adtg_calls;
        done_ms=now;done_vsync=vsyncs;in_flight=0;frames_done++;
    }
}

/* Keys seen while a session runs (diagnostics, LOG): ms, code, phase, result. */
#define FRLV_KEYS 24u
static struct { uint32_t ms;uint16_t code;uint8_t phase,result; } key_log[FRLV_KEYS];
static volatile unsigned key_n;
void livecomp_frlv_key_log(unsigned code,unsigned phase,unsigned result)
{
    if (!observing) return;
    unsigned i=key_n++;
    if (i<FRLV_KEYS) { key_log[i].ms=get_ms_clock();key_log[i].code=(uint16_t)code;key_log[i].phase=(uint8_t)phase;key_log[i].result=(uint8_t)result; }
}

struct frlv_result {
    unsigned error,stage,unsafe,rom_engdrvout,hooks;
    unsigned top_left,bottom_right,width,height,pitch,bytes;
    unsigned reg_6804,reg_6008,reg_6010,reg_6014,reg_38024,reg_6804_after;
    unsigned zoom_ms,merged;
    unsigned partials,vsync_count,vsync_first;
    unsigned vsync_us[FRLV_VSYNC_RING];
    unsigned black,ob_stdev_x10,center_mean,max_value;
    unsigned col_band[12],row_band[10];
    unsigned saved,save_stage,save_size;
    unsigned period_ms,timer_fixes,timer_seen_b,pdm[6];
    unsigned preview_frames,clip_x10,battery_start,battery_end,battery_stop,iso,aperture,mode_lost,dma_aborts;
    unsigned limit,overrun_offset,missed,preview_ok,preview_us,preview_copy_us,merge_max_us,merge_sum_ms,preview_skipped,behind,preview_strips;
    int32_t neutral[3];unsigned wb_mode,kelvin,fp_fixed,fp_us,late_frames,zoom_done_ms;
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
    int n=snprintf(t,sizeof(t),LIVECOMP_BUILD_ID "\nFull-res composite\nStatus: %s\n"
        "Stage_error_unsafe: %d,%d,%d\n"
        "Stages: 1=checked 2=allocated 3=hooked 4=zoomed 5=verified 6=captured 7=restored 8=saved\n"
        "Errors: 1=ROM 2=state 4=memory 8=hook 16=zoom 32=registers 64=stall 128=DMA 256=guard 512=size 1024=IO 2048=restore 4096=cancel 8192=raw_user\n"
        "Shutter_ms_timerA_timerB_blanking: %d,%x,%x,%d\n"
        "ISO_aperture_x10_battery_start_end_stop: %d,%d,%d,%d,%d\n"
        "Preview_frames_last_clip_pct_x10_mode_lost_dma_aborts: %d,%d,%d,%d\n"
        "EngDrvOut_instr_hooks: %x,%d\n"
        "Regs_6800_6804_6008_6010_6014_38024: %x,%x,%x,%x,%x,%x\n"
        "Reg_6804_after_restore: %x\n"
        "Width_height_pitch_bytes: %d,%d,%d,%d\n"
        "Zoom_ms_max_vsync_gap_ms: %d,%d\n"
        "Timer_fixes_last_seen_B: %d,%x\n"
        "PathDriveMode_words: %x,%x,%x,%x,%x,%x\n",
        status,r->stage,r->error,r->unsafe,r->period_ms,cfg.timer_a,cfg.timer_b,cfg.blanking,
        r->iso,r->aperture,r->battery_start,r->battery_end,r->battery_stop,r->preview_frames,r->clip_x10,r->mode_lost,r->dma_aborts,
        r->rom_engdrvout,r->hooks,r->top_left,r->bottom_right,r->reg_6008,r->reg_6010,r->reg_6014,r->reg_38024,
        r->reg_6804_after,r->width,r->height,r->pitch,r->bytes,
        r->zoom_ms,max_gap_ms,r->timer_fixes,r->timer_seen_b,
        r->pdm[0],r->pdm[1],r->pdm[2],r->pdm[3],r->pdm[4],r->pdm[5]);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),
        "Hook_cmos_calls_changes: %d,%d\nHook_adtg_calls_changes_blanking_seen: %d,%d,%d\n"
        "Hook_engio_calls_changes_drv_changes_overflow: %d,%d,%d,%d\n"
        "Canon_orig_6804_timerA_timerB_blanking: %x,%x,%x,%x\n"
        "Merged: %d\n"
        "Black_OBstdev_x10_center_mean_max: %d,%d,%d,%d\n"
        "Col_band_means_16px: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Row_band_means_8px: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Saved_stage_size: %d,%d,%d\nDNG: %s\n"
        "Limit_overrun_offset_missed_partials: %d,%d,%d,%d\n"
        "Merge_max_us_sum_ms_preview_ok_render_us_copy_us: %d,%d,%d,%d,%d\n"
        "WB_mode_kelvin_neutral_r_g_b: %d,%d,%d,%d,%d\nFocus_pixels_fixed_us: %d,%d\n"
        "Zoom_done_ms_late_frames: %d,%d\nMerge_kernel_bands_preview_skipped_behind_strips: select,%d,%d,%d,%d\n",
        stats.cmos_calls,stats.cmos_changes,stats.adtg_calls,stats.adtg_changes,stats.blanking_seen,
        stats.engio_calls,stats.engio_changes,stats.drv_changes,stats.overflow,
        stats.orig_6804,stats.orig_timer_a,stats.orig_timer_b,stats.orig_blanking,
        r->merged,r->black,r->ob_stdev_x10,r->center_mean,r->max_value,
        r->col_band[0],r->col_band[1],r->col_band[2],r->col_band[3],r->col_band[4],r->col_band[5],
        r->col_band[6],r->col_band[7],r->col_band[8],r->col_band[9],r->col_band[10],r->col_band[11],
        r->row_band[0],r->row_band[1],r->row_band[2],r->row_band[3],r->row_band[4],
        r->row_band[5],r->row_band[6],r->row_band[7],r->row_band[8],r->row_band[9],
        r->saved,r->save_stage,r->save_size,r->dng,
        r->limit,r->overrun_offset,r->missed,r->partials,
        r->merge_max_us,r->merge_sum_ms,r->preview_ok,r->preview_us,r->preview_copy_us,
        r->wb_mode,r->kelvin,r->neutral[0],r->neutral[1],r->neutral[2],r->fp_fixed,r->fp_us,
        r->zoom_done_ms,r->late_frames,FRLV_BANDS,r->preview_skipped,r->behind,r->preview_strips);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Keys (%d; ms:code:phase:passed, raw events 0x1000+param):",key_n);
    for (unsigned k=0;k<key_n && k<FRLV_KEYS;k++)
        n+=snprintf(t+n,sizeof(t)-n," %d:%x:%d:%d",key_log[k].ms,key_log[k].code,key_log[k].phase,key_log[k].result);
    t[n++]='\n';ok&=write_text(f,t,n);
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
    memcpy(m.neutral,r->neutral,sizeof(m.neutral));
    m.iso=raw2iso(lens_info.raw_iso);m.tvr=MAX(1u,1000000u/r->period_ms);m.exposure_ms=r->period_ms;   /* shutter = frame period */
    m.fps=MAX(1u,1000000u/r->period_ms);m.aperture=lens_info.aperture;m.focal=lens_info.focal_len;
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

/* Arm the next frame from the worker, well after the vsync. LCM15/17 showed
 * occasional partial frames when arming inside the vsync callback: the
 * readout of the frame that just ended can still be finishing (~2 ms margin),
 * so a slurp started then takes its last rows and stops at frame end. */
/* Stop our RAW slurp if it cannot finish (LCM43: Canon left the full-res
 * timing mid-frame, so the armed 32 MB transfer never completed and the
 * session ended "DMA uncertain"). Canon's AbortEDmac; 1 = channel idle,
 * the buffer is ours again (its contents are discarded). */
static unsigned raw_dma_abort(void)
{
    if (!edmac_get_state(raw_write_chan)) { in_flight=0;return 1; }
    AbortEDmac(raw_write_chan);
    for (unsigned i=0;i<30 && edmac_get_state(raw_write_chan);i++) msleep(10);
    if (edmac_get_state(raw_write_chan)) return 0;
    in_flight=0;return 1;
}
static unsigned window_lost(void)
{
    return shamem_read(0xC0F06804)!=((cfg.raw_v<<16)|cfg.raw_h);
}
static void arm_from_task(void)
{
    EngDrvOut(FRLV_RAW_TYPE_REG,FRLV_RAW_TYPE);
    arm_vsync_delay=(unsigned)get_us_clock()-last_vsync_us;
    frlv_slurp((void *)physical(capture_target),capture_pitch,capture_height);
    in_flight=1;
}

/* ---------------------------------------------------------------------
 * Continuous full-res composite (Task: Full-res composite).
 * ------------------------------------------------------------------- */
struct frlv_frame { unsigned seq,done_ms,merge_us,changed_pct,offset_after,state,arm_after_vsync_us,cb_kind,cb_after_arm_us,cb_offset,engio_delta,adtg_delta,lost_bands; };
static struct frlv_frame cont_frames[FRLV_CSV_MAX],cont_overflow;  /* overflow: frames past the CSV table */
static uint16_t *preview_yuv;              /* cached 720x480 UYVY render */
static unsigned preview_w,preview_h,preview_pitch,preview_part;

/* Render the accumulator (cached, packed RAW14) into preview_yuv. Active area
 * left 72, top 28, 5208x3478 (measured, LCM15). Nearest RGGB quad sampling,
 * fixed daylight-ish gains; this is a monitor view, not a colour render. */
/* Two horizontally adjacent samples (even index i = x%8) of a packed RAW14
 * row, straight from the 16-bit words (no full 8-sample unpack). */
static inline __attribute__((always_inline)) void preview_pair(const uint16_t *s,unsigned i,int *p0,int *p1)
{
    switch (i) {
        case 0: *p0=s[0]>>2;*p1=((s[0]&3)<<12)|(s[1]>>4);break;
        case 2: *p0=((s[1]&15)<<10)|(s[2]>>6);*p1=((s[2]&63)<<8)|(s[3]>>8);break;
        case 4: *p0=((s[3]&255)<<6)|(s[4]>>10);*p1=((s[4]&1023)<<4)|(s[5]>>12);break;
        default: *p0=((s[5]&4095)<<2)|(s[6]>>14);*p1=s[6]&16383;break;
    }
}
/* LCM25: 360x240 samples, each written as a 2x2 block. Column positions are
 * computed once (no division per pixel) and only the 4 samples of each RGGB
 * quad are extracted. LCM24: 263 ms per render, mostly skipped at 0.5 fps. */
static uint16_t preview_col_off[360];
static uint8_t preview_col_i[360];
/* parts > 1 renders only strip `part` of `parts` (LCM27 rolling preview).
 * Returns how many rendered RGGB quads have a clipped sample (exposure
 * preview: share of highlights at the white level). */
#define FRLV_CLIP 14550                 /* ~97% of the 15000 white level */
static __attribute__((optimize("O2"))) unsigned preview_render(const struct frlv_result *r,const uint8_t *acc,unsigned part,unsigned parts)
{
    unsigned clipped=0;
    const unsigned x0=72,y0=28,aw=r->width-x0,ah=r->height-y0;
    const int black=r->black ? (int)r->black:2048;
    const unsigned range=15000-(unsigned)black;
    const unsigned scale=(4095u<<16)/range,sw=MIN(preview_w/2,360u),sh=preview_h/2;
    /* white balance gains (x256) = 1/AsShotNeutral */
    const int gr=(int)(256000000u/(unsigned)MAX(r->neutral[0],100000)),gb=(int)(256000000u/(unsigned)MAX(r->neutral[2],100000));
    for (unsigned x=0;x<sw;x++) {
        unsigned rx=(x0+(x*aw/sw))&~1u;
        preview_col_off[x]=(uint16_t)((rx/8)*7);preview_col_i[x]=(uint8_t)(rx%8);
    }
    for (unsigned y=sh*part/parts;y<sh*(part+1)/parts;y++) {
        unsigned ry=(y0+(y*ah/sh))&~1u;
        const uint16_t *row0=(const uint16_t *)(const void *)(acc+ry*r->pitch),*row1=row0+r->pitch/2;
        uint32_t *out0=(uint32_t *)((uint8_t *)preview_yuv+2*y*preview_pitch);
        uint32_t *out1=(uint32_t *)((uint8_t *)out0+preview_pitch);
        for (unsigned x=0;x<sw;x++) {
            const unsigned o=preview_col_off[x],i=preview_col_i[x];
            int r0,g0,g1,b1;
            preview_pair(row0+o,i,&r0,&g0);preview_pair(row1+o,i,&g1,&b1);
            clipped+=(r0|g0|g1|b1)>=FRLV_CLIP ? (r0>=FRLV_CLIP||g0>=FRLV_CLIP||g1>=FRLV_CLIP||b1>=FRLV_CLIP):0;
            int R=(r0-black)*gr>>8,G=((g0+g1)>>1)-black,B=(b1-black)*gb>>8;
            unsigned rr=R>0 ? MIN(4095u,(unsigned)R*scale>>16):0,gg=G>0 ? MIN(4095u,(unsigned)G*scale>>16):0,
                     bb=B>0 ? MIN(4095u,(unsigned)B*scale>>16):0;
            int r8=lc_gamma_lut[rr],g8=lc_gamma_lut[gg],b8=lc_gamma_lut[bb];
            int yy=(77*r8+150*g8+29*b8)>>8,uu=(-43*r8-85*g8+128*b8)>>8,vv=(128*r8-107*g8-21*b8)>>8;
            uint32_t w=(uu&0xFF)|((yy&0xFF)<<8)|((vv&0xFF)<<16)|((unsigned)(yy&0xFF)<<24);
            out0[x]=w;out1[x]=w;
        }
        if (parts==1 && y==sh/2) msleep(10);            /* keep the UI responsive */
    }
    return clipped;
}
/* Overwrite Canon's three LV YUV buffers with the last render. Canon refills
 * one per frame near the vsync; the worker repeats this right after it. */
static void preview_show(void)
{
    if (!preview_yuv) return;
    for (unsigned i=0;i<3;i++) {
        uint8_t *dst=(uint8_t *)livecomp_camera.lv_yuv[i];
        if (dst) memcpy(dst,preview_yuv,preview_pitch*preview_h);
    }
}
static void preview_setup(void)
{
    struct vram_info *v=get_yuv422_vram();
    preview_yuv=NULL;
    if (!v || v->width!=720 || v->height<400 || v->height>480 || v->pitch!=1440) return;
    preview_w=v->width;preview_h=v->height;preview_pitch=v->pitch;
    preview_yuv=fio_malloc(preview_pitch*preview_h);
    if (preview_yuv) memset(preview_yuv,0,preview_pitch*preview_h);
}
static void write_csv(const struct frlv_result *r)
{
    if (ml_shutdown_requested) return;
    char path[FIO_MAX_PATH_LENGTH];strcpy(path,r->log);memcpy(path+strlen(path)-3,"CSV",3);
    FILE *f=FIO_CreateFile(path);
    if (!f) return;
    static char t[256];
    int n=snprintf(t,sizeof(t),"seq,done_ms,merge_us,changed_pct,dma_offset_after_merge,partial_bytes_plus1,arm_after_vsync_us,cb_kind,cb_after_arm_us,cb_offset,engio_calls,adtg_calls,lost_bands\n");
    FIO_WriteFile(f,t,n);
    for (unsigned i=0;i<r->merged && i<FRLV_CSV_MAX;i++) {
        const struct frlv_frame *c=&cont_frames[i];
        n=snprintf(t,sizeof(t),"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",c->seq,c->done_ms,c->merge_us,c->changed_pct,c->offset_after,c->state,
            c->arm_after_vsync_us,c->cb_kind,c->cb_after_arm_us,c->cb_offset,c->engio_delta,c->adtg_delta,c->lost_bands);
        FIO_WriteFile(f,t,n);
    }
    FIO_CloseFile(f);
}
/* Every completed frame is merged. Frames that arrive while a merge is still
 * running are counted as missed (one sub-exposure gap each); the session
 * stops only when that keeps happening (more than 2 + limit/4). */
/* Battery: PROP_BATTERY_POWER reaching 0 (the last, red level) during a
 * session stops it like SET: the exposure in progress completes, then the
 * composite is saved while there is still power for the card write. */
static unsigned battery_low(struct frlv_result *r)
{
    if (r->battery_stop || !r->battery_start || battery_level_bars>0) return 0;
    r->battery_stop=1;NotifyBox(8000,"Battery low: saving");
    return 1;
}
static void continuous_capture(struct frlv_result *r,uint8_t *cap,uint8_t *acc,
    const volatile unsigned *cancel,const char **msg,unsigned *dma_uncertain,unsigned period_ms,
    const struct lc_frlv_options *o)
{
    /* Exposure preview first (if the caller provides a start flag): every
     * frame is shown as it was exposed, nothing is merged. SET (go) starts
     * the composite with the first frame exposed entirely after the press. */
    static volatile unsigned own_phase;
    volatile unsigned *phase=o->phase ? o->phase:&own_phase;
    const volatile unsigned *go=o->go;
    unsigned go_vsync=0,lost_ticks=0;
    *phase=go ? LC_FRLV_PREVIEW:LC_FRLV_COMPOSE;
    preview_setup();r->preview_ok=preview_yuv!=NULL;
    const unsigned end=physical(cap)+r->bytes,pairs=r->bytes/28;
    static char line[64];
    unsigned last_engio=stats.engio_calls,last_adtg=stats.adtg_calls;
    cont_arm=1;
    /* First arm: just after a vsync, never inside it. */
    unsigned seen=frames_done,v=vsyncs,w0=get_ms_clock();
    while (vsyncs==v && get_ms_clock()-w0<period_ms+3000) msleep(10);
    msleep(40);arm_from_task();
    if (*phase==LC_FRLV_PREVIEW) {
        *msg="Preview: first frame...";
        NotifyBox(MIN(period_ms+2000,60000u),"PREVIEW, not saved. SET=start");
    } else { *msg="Composite: first frame...";NotifyBox(8000,"Composite: first frame"); }
    for (;;) {
        if (go && *go && !go_vsync) go_vsync=vsyncs;
        if (*cancel || ml_shutdown_requested || !zoom5() || raw_lv_is_enabled() || r->merged>=r->limit || battery_low(r)) {
            if (cont_arm && *cancel) NotifyBox(5000,*phase==LC_FRLV_PREVIEW ? "Leaving the preview":"Stopping after this frame");
            cont_arm=0;
        }
        w0=get_ms_clock();
        while (frames_done==seen && in_flight && get_ms_clock()-w0<period_ms+3000) {
            if (go && *go && !go_vsync) {
                go_vsync=vsyncs;
                NotifyBox(MIN(period_ms*2+2000,60000u),"Starting composite...");
            }
            if (*cancel && cont_arm) { cont_arm=0;NotifyBox(5000,*phase==LC_FRLV_PREVIEW ? "Leaving the preview":"Stopping after this frame"); }
            if (ml_shutdown_requested || battery_low(r)) cont_arm=0;
            /* Canon reconfigured the sensor (left x5 or reset its timing):
             * the armed frame will never complete. */
            lost_ticks=(!zoom5() || window_lost()) ? lost_ticks+1:0;
            if (lost_ticks>=10) {                       /* 100 ms, not a transient */
                cont_arm=0;r->mode_lost=1;
                if (raw_dma_abort()) { r->error|=FE_STATE;*msg="Camera left full-res mode";break; }
                *dma_uncertain=1;r->error|=FE_DMA;*msg="RAW DMA did not stop";break;
            }
            msleep(10);
        }
        if (r->mode_lost) break;
        if (frames_done==seen) {
            if (!in_flight) break;                       /* clean stop: nothing armed */
            r->error|=FE_STALL;*msg="LV frame missing; see LOG";break;
        }
        /* LCM26: a frame that arrived while we were still busy is skipped
         * (a gap of one sub-exposure), not fatal. The capture buffer holds
         * the latest complete frame. Stop only if it keeps happening. */
        if (frames_done!=seen+1 && *phase==LC_FRLV_COMPOSE) {
            r->missed+=frames_done-seen-1;
            /* Relative rule, so it also works without a frame limit. */
            if (r->missed>2+r->merged/4) { r->error|=FE_DMA;*msg="Merge too slow: frames missed";cont_arm=0;break; }
        }
        seen=frames_done;
        unsigned state=done_state,pointer=done_pointer,done=done_ms;
        unsigned d_kind=done_cb_kind,d_after=done_cb_after,d_ptr=done_cb_ptr,d_arm=done_arm_after_vsync,d_engio=done_engio,d_adtg=done_adtg;
        preview_show();                                  /* hide Canon's new frame */
        if (state) {
            /* Still active one frame later: stop it; uncertain only if it
             * does not stop. The incomplete frame is not used. */
            cont_arm=0;r->error|=FE_DMA;r->dma_aborts++;
            if (raw_dma_abort()) { *msg="Frame lost: DMA stopped";break; }
            *dma_uncertain=1;*msg="RAW DMA did not stop";break;
        }
        if (!guards_ok(cap,r->bytes)) { cont_arm=0;*dma_uncertain=1;r->error|=FE_GUARD;*msg="Buffer guard overwritten";break; }
        if (r->merged>=r->limit) continue;               /* past the limit: not merged */
        if (*phase==LC_FRLV_PREVIEW) {
            if (cont_arm) arm_from_task();
            /* Frame done at vsync n was exposed from vsync n-1 on; SET came
             * after vsync go_vsync, so frames done at go_vsync+2 or later
             * are exposed entirely after the press. */
            if (go_vsync && done_vsync>=go_vsync+2) {
                *phase=LC_FRLV_COMPOSE;
                NotifyBox(MIN(period_ms+2000,60000u),"COMPOSITE 1  SET=stop");
            } else {
                /* Show this frame as exposed (same render as the composite). */
                sync_caches();
                const uint8_t *src=(const uint8_t *)physical(cap);
                if (!r->preview_frames) statistics(r,src);
                if (preview_yuv) {
                    const unsigned clipped=preview_render(r,src,0,1);sync_caches();preview_show();
                    r->clip_x10=clipped*1000u/MAX(1u,(preview_w/2)*(preview_h/2));
                }
                r->preview_frames++;
                if (!cont_arm) continue;
                /* NotifyBox prints one FONT_LARGE line at x=50: keep every
                 * message on one line of at most 30 characters (LCM43: a
                 * longer, two-line text was drawn as garbage). */
                static char info[64];   /* the text itself stays <= 30 chars */
                /* Every message names the phase (LCM44 camera run: the
                 * preview was taken for a composite that did not blend). */
                if (go_vsync) snprintf(info,sizeof(info),"Starting composite...");
                else snprintf(info,sizeof(info),"PREVIEW clip %d.%d%% SET=start",r->clip_x10/10,r->clip_x10%10);
                *msg="Exposure preview";
                NotifyBox(MIN(period_ms+2000,60000u),"%s",info);
                continue;
            }
        }
        /* Next frame: arm now (its readout starts ~period-readout later),
         * never inside the vsync: preview_show (~110 ms) already waited. */
        if (!preview_yuv) msleep(40);
        if (cont_arm && !in_flight) arm_from_task();    /* the preview may have armed it */
        struct frlv_frame *c=r->merged<FRLV_CSV_MAX ? &cont_frames[r->merged]:&cont_overflow;
        memset(c,0,sizeof(*c));                         /* static: reused across sessions */
        c->seq=seen;c->done_ms=done;c->state=pointer==end ? 0:pointer-physical(cap)+1;
        c->arm_after_vsync_us=d_arm;c->cb_kind=d_kind;c->cb_after_arm_us=d_after;c->cb_offset=d_ptr ? d_ptr-physical(cap):0;
        c->engio_delta=d_engio-last_engio;c->adtg_delta=d_adtg-last_adtg;last_engio=d_engio;last_adtg=d_adtg;
        if (pointer!=end) {
            /* Partial frame: the rows that arrived are real exposure, the rest
             * is the previous (already merged) frame. Merge it and log it. */
            r->partials++;
            if (r->partials>2+r->merged/4) { cont_arm=0;r->error|=FE_DMA;*msg="Too many partial frames";break; }
        }
        sync_caches();
        const uint8_t *src=(const uint8_t *)physical(cap);
        unsigned s0=(unsigned)get_us_clock(),changed=0;
        if (!r->merged) { statistics(r,src);s0=(unsigned)get_us_clock(); }
        /* 8 bands with a scheduler tick between them keep the camera UI alive
         * (LCM24: was 16; the 10 ms ticks cost 160 ms per frame). Bands are
         * whole 2-block pairs (28 bytes, 16 pixels) for the select kernel. */
        for (unsigned b=0;b<FRLV_BANDS;b++) {
            unsigned b0=pairs*b/FRLV_BANDS,b1=pairs*(b+1)/FRLV_BANDS;
            /* If the next readout has already written into this band, the
             * rest of this frame is gone: stop here, keep the session going. */
            unsigned off=in_flight ? edmac_get_pointer(raw_write_chan)-physical(cap):0;
            if (off && off<=r->bytes && off>b0*28) { c->lost_bands=FRLV_BANDS-b;r->late_frames++;break; }
            if (!r->merged) memcpy(acc+b0*28,src+b0*28,(b1-b0)*28);
            else changed+=lc_raw14_max_sel((uint32_t *)(void *)(acc+b0*28),(const uint32_t *)(const void *)(src+b0*28),b1-b0);
            msleep(10);
            /* Acknowledge Q/SET during the ~1.7 s merge too (LCM25). */
            if (*cancel && cont_arm) { cont_arm=0;NotifyBox(5000,"Stopping after this frame"); }
        }
        c->changed_pct=r->merged ? (unsigned)((uint64_t)changed*100/pairs):100;
        c->merge_us=(unsigned)get_us_clock()-s0;
        r->merge_max_us=MAX(r->merge_max_us,c->merge_us);r->merge_sum_ms+=c->merge_us/1000;
        /* Did the next frame's readout start writing while we were reading? */
        unsigned off=in_flight ? edmac_get_pointer(raw_write_chan)-physical(cap):0;
        c->offset_after=off;
        r->merged++;
        if (off && off<=r->bytes) r->overrun_offset=MAX(r->overrun_offset,off);
        /* The next frame completed while we merged the last band(s): those
         * rows hold a mix of two real exposures, harmless for a maximum.
         * Merge that frame right away, without preview (LCM26: was fatal;
         * 0.5 fps at 99 % changed pixels needs 1.89 s). */
        if (frames_done!=seen) { r->behind++;r->preview_skipped++;continue; }
        /* The preview reads only the accumulator, so it may overlap the next
         * readout (which writes the capture buffer); it must just end before
         * the next frame arrives, or the next merge starts late. Estimate =
         * last measured render + copy (LCM25), FRLV_PREVIEW_MS before that. */
        unsigned preview_est=r->preview_us ? (r->preview_us+r->preview_copy_us)/1000u+20u:FRLV_PREVIEW_MS;
        const unsigned elapsed=get_ms_clock()-done;
        const unsigned strip_est=(r->preview_us ? r->preview_us/1000u:FRLV_PREVIEW_MS)/FRLV_PREVIEW_PARTS+10u;
        if (preview_yuv && elapsed+preview_est+FRLV_MARGIN_MS>period_ms) {
            /* No time for a full preview (0.5 fps): render one strip of the
             * screen per frame, cycling; the copy that hides Canon's frame at
             * the next vsync puts it on screen (LCM26: 17/18 skipped). */
            if (elapsed+strip_est+FRLV_MARGIN_MS<=period_ms) {
                preview_render(r,acc,preview_part,FRLV_PREVIEW_PARTS);
                preview_part=(preview_part+1)%FRLV_PREVIEW_PARTS;r->preview_strips++;
                sync_caches();
            } else r->preview_skipped++;
        }
        else if (preview_yuv) {
            unsigned p0=(unsigned)get_us_clock();
            preview_render(r,acc,0,1);sync_caches();
            r->preview_us=(unsigned)get_us_clock()-p0;
            p0=(unsigned)get_us_clock();preview_show();r->preview_copy_us=(unsigned)get_us_clock()-p0;
        }
        /* Once a stop is requested, keep saying so (LCM24 replaced the
         * "Stopping" box with the frame count, so Q/SET looked ignored). */
        if (!cont_arm) {
            snprintf(line,sizeof(line),"Stopping: %d frames + last",r->merged);*msg=line;
            NotifyBox(5000,"%s",line);
        } else {
            snprintf(line,sizeof(line),"COMPOSITE %d  SET=stop",r->merged);*msg=line;
            NotifyBox(MIN(period_ms+2000,60000u),"%s",line);
        }
        if (r->merged%10==1) write_log(r,"composing");
    }
    cont_arm=0;
    if (preview_yuv) { fio_free(preview_yuv);preview_yuv=NULL; }
}

unsigned livecomp_frlv_run(struct card_info *card,const struct lc_frlv_options *o,
    const volatile unsigned *cancel,const char **status)
{
    static struct frlv_result r;
    memset(&r,0,sizeof(r));memset(&stats,0,sizeof(stats));
    lc_frr_configure(&cfg,o->fps_index);
    r.period_ms=lc_frr_timing[o->fps_index<LC_FRR_FPS_CHOICES ? o->fps_index:LC_FRR_FPS_DEFAULT].period_ms;
    const unsigned period_ms=r.period_ms;
    r.iso=raw2iso(lens_info.raw_iso);r.aperture=lens_info.aperture;r.battery_start=battery_level_bars;
    r.limit=o->limit ? o->limit:~0u;                /* 0 = no limit */
    r.wb_mode=lens_info.wb_mode;r.kelvin=lens_info.kelvin;
    lc_wb_neutral_for(lens_info.wb_mode,lens_info.kelvin,lens_info.WBGain_R,lens_info.WBGain_G,lens_info.WBGain_B,r.neutral);
    void *alloc[2]={0};uint8_t *cap=NULL,*acc=NULL;
    unsigned hooked[4]={0},power=0,locked=0,old_lock=0,zoomed=0,dma_uncertain=0;
    const char *msg="Checking the camera";
    *status=msg;
    in_flight=0;timer_fixes=0;

    /* 1: state and fingerprints. Only EOS M 2.0.2 reaches this (camera guard). */
    if (!card || !choose_paths(&r,card)) { r.error|=FE_IO;msg="No card/filename";goto out; }
    if (!base_state_ok(cancel,card) || lv_dispsize!=1 || raw_lv_is_enabled() ||
        raw_write_chan!=livecomp_camera.raw_channel) {
        r.error|=FE_STATE|(raw_lv_is_enabled() ? FE_RAWUSER:0);
        msg="Need Movie LV x1, no RAW tools";goto out;
    }
    if (!lens_info.raw_iso) { r.error|=FE_STATE;msg="Set a fixed ISO (not Auto)";goto out; }
    /* Global Draw redraws over the LCD while our preview replaces Canon's
     * image (LCM43: with it on, the composite was not visible). */
    if (get_global_draw_setting()&1) { r.error|=FE_STATE;msg="Turn Global Draw OFF";goto out; }
    if (fps_get_current_x1000()<20000) {
        r.error|=FE_STATE;msg="Turn ML FPS override OFF";goto out;
    }
    r.rom_engdrvout=MEM(FRLV_ENGDRVOUT);
    for (unsigned i=0;i<6;i++) r.pdm[i]=MEM(FRLV_PATHDRIVE+4*i);
    if (MEM(FRLV_CMOS_WRITE)!=FRLV_CMOS_INSTR || MEM(FRLV_ADTG_WRITE)!=FRLV_ADTG_INSTR ||
        MEM(FRLV_ENGIO_WRITE)!=FRLV_ENGIO_INSTR ||
        pdm_zoom()!=1) {
        r.error|=FE_ROM;msg="Firmware mismatch; no change";goto out;
    }
    r.stage=1;

    /* 2: capture buffer + accumulator (exclusively owned, guarded). */
    for (unsigned i=0;i<2;i++) {
        alloc[i]=fio_malloc(FRLV_ALLOCATION);
        if (!alloc[i]) break;
    }
    if (!alloc[1]) { r.error|=FE_MEMORY;msg="Not enough memory";goto out; }
    /* 64-byte aligned pixels with a guard area before (capture only) and
     * after the frame: an overrun of the RAW DMA is detected, never silent.
     * The capture buffer is used through its uncached alias (DMA target),
     * the accumulator through the cached one (CPU merge and DNG writer). */
    cap=(uint8_t *)FRLV_ALIGN((uintptr_t)alloc[0]+FRLV_GUARD);
    acc=(uint8_t *)physical((void *)FRLV_ALIGN((uintptr_t)alloc[1]));
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
    vsyncs=0;max_gap_ms=0;key_n=0;observing=1;active=1;
    write_log(&r,"hooked; entering x5");

    /* 4: x5 zoom makes Canon rewrite the sensor configuration via the hooks. */
    msg="Starting (about 10 s)";*status=msg;
    NotifyBox(8000,"%s",msg);
    unsigned t0=get_ms_clock();
    set_lv_zoom(5);zoomed=1;
    while (!(lv_dispsize==5 && zoom5())) {
        if (*cancel || ml_shutdown_requested || get_ms_clock()-t0>6000) { r.error|=FE_ZOOM;break; }
        msleep(50);
    }
    r.zoom_ms=get_ms_clock()-t0;r.zoom_done_ms=get_ms_clock();
    if (r.error) { msg="x5 zoom did not engage";goto restore; }
    if (!wait_vsyncs(2,3*period_ms+4000,cancel)) { r.error|=*cancel ? FE_CANCEL:FE_STALL;msg="No LV frames after switch";goto restore; }
    r.top_left=shamem_read(0xC0F06800);r.bottom_right=shamem_read(0xC0F06804);
    r.reg_6008=shamem_read(0xC0F06008);r.reg_6010=shamem_read(0xC0F06010);
    r.reg_6014=shamem_read(0xC0F06014);r.reg_38024=shamem_read(0xC0F38024);
    r.stage=4;write_log(&r,"zoomed");
    if (r.bottom_right!=((cfg.raw_v<<16)|cfg.raw_h)) { r.error|=FE_REGS;msg="Sensor window not applied";goto restore; }
    lc_frr_geometry(r.top_left,r.bottom_right,&r.width,&r.height);
    r.pitch=r.width*14/8;r.bytes=r.pitch*r.height;
    if (r.width<4000 || r.width>5280 || r.width%4 || r.height<3000 || r.height>3528 || r.bytes>capacity) {
        r.error|=FE_SIZE;msg="Unexpected geometry; see LOG";goto restore;
    }
    memset(cap+r.bytes,0x5a,FRLV_GUARD);
    capture_pitch=r.pitch;capture_height=r.height;capture_target=cap;
    r.stage=5;write_log(&r,"verified");

    /* 6: compose until SET, the frame limit or an error. */
    msg="Running";*status=msg;
    continuous_capture(&r,cap,acc,cancel,&msg,&dma_uncertain,period_ms,o);
    if (o->phase) *o->phase=LC_FRLV_SETUP;
    if (r.merged && !(r.error&~FE_CANCEL)) r.stage=6;
    *status=msg;

restore:
    {
        unsigned c=vsyncs;r.vsync_count=MIN(c,FRLV_VSYNC_RING);
        r.vsync_first=c>FRLV_VSYNC_RING ? c%FRLV_VSYNC_RING:0;
        for (unsigned k=0;k<FRLV_VSYNC_RING;k++) r.vsync_us[k]=vsync_us[k];
    }
    active=0;
    msg=r.error ? msg:"Full-res composite: restoring";*status=msg;
    /* Let any armed slurp complete before leaving the mode. */
    {
        unsigned w0=get_ms_clock();
        while (in_flight && !r.mode_lost && get_ms_clock()-w0<period_ms+4000) msleep(20);
        if ((in_flight || edmac_get_state(raw_write_chan)) && !raw_dma_abort()) dma_uncertain=1;
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
    r.error|=FE_HOOK;msg="Hook failed; nothing changed";
unhook:
    active=0;
    for (int i=3;i>=0;i--) if (hooked[i]) {
        static const uint32_t sites[4]={FRLV_CMOS_WRITE,FRLV_ADTG_WRITE,FRLV_ENGIO_WRITE,FRLV_ENGDRVOUT};
        if (unpatch_memory(sites[i])) r.unsafe=1;
    }
    observing=0;
out:
    r.timer_fixes=timer_fixes;r.timer_seen_b=timer_seen_b;r.battery_end=battery_level_bars;
    if (dma_uncertain) r.unsafe=1;
    /* The capture buffer is free once no DMA can target it: release it
     * before the (slow) DNG write. A partial composite is still saved. */
    if (!r.unsafe && alloc[0]) { fio_free(alloc[0]);alloc[0]=NULL;cap=NULL; }
    if (acc && r.merged && !r.unsafe && !ml_shutdown_requested && get_ml_card()==card &&
        !(r.error&(FE_GUARD|FE_SIZE|FE_REGS))) {
        if (r.width==5280) {
            unsigned f0=(unsigned)get_us_clock();
            r.fp_fixed=lc_fix_focus_pixels(acc,r.width,r.height,r.pitch);
            r.fp_us=(unsigned)get_us_clock()-f0;
        }
        *status="Saving the DNG";
        NotifyBox(15000,"Saving the DNG...");
        r.saved=frlv_save_dng(&r,acc);
        if (!r.saved) r.error|=FE_IO;
        else r.stage=8;
    }
    if (!r.unsafe) for (unsigned i=0;i<2;i++) if (alloc[i]) fio_free(alloc[i]);
    if (locked) gui_uilock(old_lock);
    if (power) powersave_permit();
    if (r.unsafe) msg="DMA uncertain: reboot camera";
    else if (!r.error) msg=r.saved ? "Full-res composite saved":r.preview_frames ? "Preview ended; nothing saved":"Full-res: no frame saved";
    else if (r.saved) msg="Saved; stopped early, see LOG";
    if (r.saved && !r.error && !r.unsafe) {
        static char done[64];
        if (r.late_frames || r.missed) snprintf(done,sizeof(done),"Composite saved: %d (%d late)",r.merged,r.late_frames+r.missed);
        else if (r.battery_stop) snprintf(done,sizeof(done),"Battery low: saved %d frames",r.merged);
        else snprintf(done,sizeof(done),"Composite saved: %d frames",r.merged);
        msg=done;
    }
    *status=msg;
    if (r.log[0]) write_log(&r,msg);
    if (r.log[0]) write_csv(&r);
    return r.unsafe;
}
