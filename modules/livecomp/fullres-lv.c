/* GPL-2.0-or-later. Full-sensor Live Composite / Live Bulb for the EOS M 2.0.2.
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
 *     Live Bulb (LC_MODE_BULB) adds the frames instead (raw14-add.h): the
 *     accumulator is one long exposure of frames x shutter, built up live.
 *  5. Leave x5, remove the hooks, fix the PDAF pixels, save the DNG in
 *     DCIM/LIVE (+ LOG and CSV when Debug is ON).
 * If the RAW DMA does not finish, buffer ownership is uncertain: the buffers
 * are kept allocated and the user is told to reboot.
 *
 * Canon's own image-max engine was investigated as a faster merge (LCM9-40,
 * LCX1-2) and cannot merge while the sensor runs; see HARDWARE-MERGE.md. */
#include "dryos.h"
#include "module.h"
#include "lens.h"
#include "raw.h"
#include "mem.h"
#include "patch.h"
#include "edmac.h"
#ifdef __ARM__
#include "edmac-memcpy.h"                   /* LCB3: preview copy by EDMAC */
#endif
#include "fps.h"
#include "shoot.h"
#include "propvalues.h"
#include "gui-common.h"
#include "powersave.h"
#include "camera-profile.h"
#include "fullres-lv.h"
#include "fullres-regs.h"
#include "raw14-max.h"
#include "raw14-add.h"
#include "raw14-max-asm.h"                  /* ARM merge kernel (camera build only) */
#include "raw14-add-asm.h"                  /* ARM Live Bulb kernels (camera build only) */
#include "lv-dng.h"
#include "lv-mlv.h"                       /* LCM67: optional MLV output */
#include "vram.h"
#include "fullres-gamma.h"
#include "notify_box.h"
#include "zebra.h"
#include "bmp.h"
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
/* LCM62: AbortEDmac (stubs.S), hooked for a trace only (who aborts our RAW
 * channel 0x12 at the start of the 5th frame's readout, every session since
 * LCM18). Installed only if it starts like a normal function (STMFD). */
#define FRLV_ABORT_FN 0x1CA60u
/* LCM62: frames (vsyncs) kept at the 3 fps entry timing after x5, each with
 * a throwaway RAW slurp, before the chosen shutter starts. The recurring
 * abort (frame 5 counted by our slurps before LCM48 and after it, with a
 * different number of 3 fps frames; LCM21: Canon's Evf VD handler runs a
 * "LastRamClear" around frame 4) then falls on a throwaway frame, whether
 * Canon counts VDs after x5 or slurps on channel 0x12. ~4 s more startup. */
#define FRLV_WARMUP_VDS 12u
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
/* Timer switch at a frame boundary (LCM50): the worker sets the pending
 * pair, the vsync callback adopts it; the timer reapply below then writes
 * it (same registers and order as ML's FPS override on this camera). The
 * frame starting at that vsync is a whole exposure at the new timing. */
static volatile unsigned pending_timers,pend_a,pend_b,timers_switched_vsync;
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
    if (pending_timers && active && zoom5()) {
        cfg.timer_a=pend_a;cfg.timer_b=pend_b;pending_timers=0;timers_switched_vsync=vsyncs;
    }
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

/* LCM62: AbortEDmac calls on our RAW channel (LOG): vsyncs since x5, caller
 * (LR), ms, our own abort, slurp in flight. */
#define FRLV_ABORTS 8u
static struct { uint32_t vd,lr,ms;uint8_t own,flight; } abort_log[FRLV_ABORTS];
static volatile unsigned abort_n,own_abort,vd_zoom;
static void FAST abort_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;(void)pc;
    if (!observing || regs[0]!=raw_write_chan) return;
    const unsigned i=abort_n++;
    if (i<FRLV_ABORTS) {
        abort_log[i].vd=vsyncs-vd_zoom;abort_log[i].lr=regs[13];abort_log[i].ms=get_ms_clock();
        abort_log[i].own=(uint8_t)own_abort;abort_log[i].flight=(uint8_t)in_flight;
    }
}
/* Keys seen while a session runs (diagnostics, LOG): ms, code, phase, result. */
#define FRLV_KEYS 64u
static struct { uint32_t ms;uint16_t code;uint8_t phase,result; } key_log[FRLV_KEYS];
static volatile unsigned key_n;
void livecomp_frlv_key_log(unsigned code,unsigned phase,unsigned result)
{
    if (!observing) return;
    unsigned i=key_n++;
    if (i<FRLV_KEYS) { key_log[i].ms=get_ms_clock();key_log[i].code=(uint16_t)code;key_log[i].phase=(uint8_t)phase;key_log[i].result=(uint8_t)result; }
}

struct frlv_result {
    unsigned error,stage,unsafe,rom_engdrvout,hooks,rom_abort,abort_hooked;
    unsigned warmup_slurps,warmup_done,warmup_ms,warmup_aborts;
    unsigned top_left,bottom_right,width,height,pitch,bytes;
    unsigned reg_6804,reg_6008,reg_6010,reg_6014,reg_38024,reg_6804_after;
    unsigned zoom_ms,merged;
    unsigned partials,vsync_count,vsync_first;
    unsigned vsync_us[FRLV_VSYNC_RING];
    unsigned black,ob_stdev_x10,center_mean,max_value;
    unsigned col_band[12],row_band[10];
    unsigned saved,save_stage,save_size;
    /* LCM67: file format (0 DNG, 1 DNG + MLV, 2 MLV) and save timing */
    unsigned format,save_ms,save_swap_ms,save_write_ms,save_chunk,save_in_place,mlv_saved,mlv_ms,mlv_write_ms,mlv_size;
    unsigned orig_save_ms,dark_save_ms,load_ms;
    unsigned period_ms,timer_fixes,timer_seen_b,pdm[6];
    unsigned preview_only,clip_x10,battery_start,battery_end,battery_stop,iso,aperture,mode_lost,dma_aborts,first_arm_after_vsync_ms,switch_ms,exit_switched,target_a,target_b,short_skipped;
    unsigned limit,overrun_offset,missed,preview_ok,preview_us,preview_copy_us,merge_max_us,merge_sum_ms,preview_skipped,behind,preview_strips;
    int32_t neutral[3];unsigned wb_mode,kelvin,fp_fixed,fp_us,late_frames,zoom_done_ms;
    unsigned mode;        /* LC_MODE_COMPOSITE / LC_MODE_BULB */
    unsigned acc_black;   /* Live Bulb: black level measured on the accumulator */
    unsigned too_slow;    /* Live Bulb stopped: merge overtaken by the next readout */
    unsigned colfix_rms_x10,colfix_us;   /* Live Bulb column correction (LCM56) */
    unsigned colfix_img,colfix_dark,colfix_vob,colfix_vpix,colfix_scan_us;   /* LCM57 image refinement */
    int32_t colfix_lane_x10[8];
    /* LCM59 dark frames (menu option): as many as light frames, same shutter */
    unsigned dark_want,save_orig,dark_state,dark_frames,dark_partials,dark_lost,dark_wait_ms,dark_sep;
    unsigned orig_saved,dark_saved,orig_read,dark_black,dark_merge_us;
    unsigned end_wait_ms;
    unsigned bulb_partials;               /* Live Bulb: partial frames not added (LCM56) */
    char dng[FIO_MAX_PATH_LENGTH],log[FIO_MAX_PATH_LENGTH],mlv[FIO_MAX_PATH_LENGTH];
};

/* Output folder: DCIM/LIVE on the ML card (created if missing). Canon's
 * playback ignores it (not a DCF folder name). Debug logs, when enabled, go
 * next to the DNGs with the same number. */
static int output_dir(struct card_info *card,char *dir,unsigned size)
{
    char dcim[16];
    snprintf(dcim,sizeof(dcim),"%s:/DCIM",card->drive_letter);
    snprintf(dir,size,"%s:/DCIM/LIVE",card->drive_letter);
    FIO_CreateDirectory(dcim);                     /* no-op when present */
    FIO_CreateDirectory(dir);
    return is_dir(dir);
}
static int is_bulb(const struct frlv_result *r) { return r->mode==LC_MODE_BULB; }
static const char *mode_word(const struct frlv_result *r) { return is_bulb(r) ? "Bulb":"Composite"; }
/* Exposure of the result: one frame for a maximum, all merged frames for a
 * sum (missed frames are gaps and do not count). */
static unsigned total_exposure_ms(const struct frlv_result *r)
{ return is_bulb(r) ? r->merged*r->period_ms:r->period_ms; }
/* White level of the DNG; Live Bulb clamps its sum to the same value. */
static unsigned frlv_white(unsigned black)
{ return raw_info.white_level>(int)black && raw_info.white_level<=16383 ? (unsigned)raw_info.white_level:15000u; }
/* Composite LVFRnnnn, Live Bulb LVBLnnnn (each with its own numbering). */
static int choose_paths(struct frlv_result *r,const char *dir,unsigned debug)
{
    uint32_t size;
    const char *prefix=is_bulb(r) ? "LVBL":"LVFR";
    for (unsigned i=0;i<10000;i++) {
        snprintf(r->dng,sizeof(r->dng),"%s/%s%04d.DNG",dir,prefix,i);
        snprintf(r->log,sizeof(r->log),"%s/%s%04d.LOG",dir,prefix,i);
        snprintf(r->mlv,sizeof(r->mlv),"%s/%s%04d.MLV",dir,prefix,i);
        if (FIO_GetFileSize(r->dng,&size) && FIO_GetFileSize(r->mlv,&size) && (!debug || FIO_GetFileSize(r->log,&size))) {
            if (!debug) r->log[0]=0;               /* Debug OFF: no LOG/CSV */
            return 1;
        }
    }
    return 0;
}
static int write_text(FILE *f,const char *text,int n)
{ return n>0 && FIO_WriteFile(f,text,n)==n; }
/* Rewritten at each stage so an interrupted run still leaves evidence. */
static void write_log(const struct frlv_result *r,const char *status)
{
    if (ml_shutdown_requested || !r->log[0]) return;
    FILE *f=FIO_CreateFile(r->log);
    if (!f) return;
    static char t[2048];int ok=1;
    int n=snprintf(t,sizeof(t),LIVECOMP_BUILD_ID "\nFull-res %s\nStatus: %s\n"
        "Stage_error_unsafe: %d,%d,%d\n"
        "Stages: 1=checked 2=allocated 3=hooked 4=zoomed 5=verified 6=captured 7=restored 8=saved\n"
        "Errors: 1=ROM 2=state 4=memory 8=hook 16=zoom 32=registers 64=stall 128=DMA 256=guard 512=size 1024=IO 2048=restore 4096=cancel 8192=raw_user\n"
        "Shutter_ms_timerA_timerB_blanking: %d,%x,%x,%d\n"
        "ISO_aperture_x10_battery_start_end_stop: %d,%d,%d,%d,%d\n"
        "Preview_only_clip_pct_x10_mode_lost_dma_aborts_first_arm_ms: %d,%d,%d,%d,%d\nTimer_switch_after_zoom_ms_exit_switched_short_skipped: %d,%d,%d\n"
        "EngDrvOut_instr_hooks: %x,%d\n"
        "Regs_6800_6804_6008_6010_6014_38024: %x,%x,%x,%x,%x,%x\n"
        "Reg_6804_after_restore: %x\n"
        "Width_height_pitch_bytes: %d,%d,%d,%d\n"
        "Zoom_ms_max_vsync_gap_ms: %d,%d\n"
        "Timer_fixes_last_seen_B: %d,%x\n"
        "PathDriveMode_words: %x,%x,%x,%x,%x,%x\n",
        is_bulb(r) ? "live bulb":"composite",status,r->stage,r->error,r->unsafe,r->period_ms,r->target_a,r->target_b,cfg.blanking,
        r->iso,r->aperture,r->battery_start,r->battery_end,r->battery_stop,r->preview_only,r->clip_x10,r->mode_lost,r->dma_aborts,r->first_arm_after_vsync_ms,r->switch_ms,r->exit_switched,r->short_skipped,
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
        "Zoom_done_ms_late_frames: %d,%d\nMerge_kernel_bands_preview_skipped_behind_strips: %s,%d,%d,%d,%d\n"
        "Mode_total_exposure_ms_acc_black_too_slow: %s,%d,%d,%d\n",
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
        r->zoom_done_ms,r->late_frames,is_bulb(r) ? "add":"select",FRLV_BANDS,r->preview_skipped,r->behind,r->preview_strips,
        is_bulb(r) ? "bulb":"composite",total_exposure_ms(r),r->acc_black,r->too_slow);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Column_fix_rms_x10_us_bulb_partials_skipped: %d,%d,%d\n",r->colfix_rms_x10,r->colfix_us,r->bulb_partials);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Column_fix_img_dark_vob_vpix_scan_us_lanes_x10: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        r->colfix_img,r->colfix_dark,r->colfix_vob,r->colfix_vpix,r->colfix_scan_us,
        r->colfix_lane_x10[0],r->colfix_lane_x10[1],r->colfix_lane_x10[2],r->colfix_lane_x10[3],
        r->colfix_lane_x10[4],r->colfix_lane_x10[5],r->colfix_lane_x10[6],r->colfix_lane_x10[7]);
    ok&=write_text(f,t,n);
#ifdef __ARM__
    n=snprintf(t,sizeof(t),"Merge_code_preview_copy: arm-ldm,edmac\n");
#else
    n=snprintf(t,sizeof(t),"Merge_code_preview_copy: c,memcpy\n");
#endif
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Dark_want_orig_state_frames_partials_lost_wait_ms_sep_origsaved_darksaved_origread_black_merge_us_end_wait_ms: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
        r->dark_want,r->save_orig,r->dark_state,r->dark_frames,r->dark_partials,r->dark_lost,r->dark_wait_ms,r->dark_sep,
        r->orig_saved,r->dark_saved,r->orig_read,r->dark_black,r->dark_merge_us,r->end_wait_ms);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Save_format_dng_ms_swap_ms_write_ms_chunk_kb_in_place_mlv_saved_ms_write_ms_size: %d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n"
        "Save_orig_ms_dark_ms_load_ms: %d,%d,%d\nMLV: %s\n",
        r->format,r->save_ms,r->save_swap_ms,r->save_write_ms,r->save_chunk>>10,r->save_in_place,r->mlv_saved,r->mlv_ms,r->mlv_write_ms,r->mlv_size,
        r->orig_save_ms,r->dark_save_ms,r->load_ms,r->format ? r->mlv:"");
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Warmup_vds_slurps_done_ms_aborts: %d,%d,%d,%d,%d\n",FRLV_WARMUP_VDS,r->warmup_slurps,r->warmup_done,r->warmup_ms,r->warmup_aborts);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Abort_hook_instr_hooked_calls (vd:lr:ms:own:inflight): %x,%d,%d",r->rom_abort,r->abort_hooked,abort_n);
    for (unsigned k=0;k<abort_n && k<FRLV_ABORTS;k++)
        n+=snprintf(t+n,sizeof(t)-n," %d:%x:%d:%d:%d",abort_log[k].vd,abort_log[k].lr,abort_log[k].ms,abort_log[k].own,abort_log[k].flight);
    t[n++]='\n';ok&=write_text(f,t,n);
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
/* Mean of the left optical-black columns (16..63, every 8th row). */
static unsigned ob_mean(const struct frlv_result *r,const uint8_t *buf)
{
    uint64_t sum=0;unsigned n=0;
    for (unsigned y=40;y+40<r->height;y+=8) for (unsigned x=16;x<64;x+=3) { sum+=sample(buf,r->pitch,x,y);n++; }
    return n ? (unsigned)((sum+n/2)/n):0;
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
/* DNG and MLV metadata of a result. */
static void frlv_metadata(const struct frlv_result *r,struct lc_dng_metadata *m)
{
    memset(m,0,sizeof(*m));
    m->width=r->width;m->height=r->height;m->pitch=r->pitch;m->bytes=r->bytes;
    /* Live Bulb: the black level as it is in the sum (the per-frame estimate
     * is an integer; its rounding error would add up over many frames). */
    m->black=is_bulb(r) && r->acc_black ? r->acc_black:r->black ? r->black:2048;
    m->white=frlv_white(m->black);
    m->cfa=raw_info.cfa_pattern;
    /* Crop guess only (left OB ~72 px, top ~28 rows); full raster is stored. */
    m->active[0]=MIN(28u,r->height-2);m->active[1]=MIN(72u,r->width-8);
    m->active[2]=r->height;m->active[3]=r->width;
    memcpy(m->matrix,raw_info.color_matrix1,sizeof(m->matrix));
    m->illuminant=raw_info.calibration_illuminant1;
    memcpy(m->neutral,r->neutral,sizeof(m->neutral));
    m->iso=raw2iso(lens_info.raw_iso);m->tvr=MAX(1u,1000000u/r->period_ms);m->exposure_ms=total_exposure_ms(r);   /* composite: frame period; bulb: frames x period */
    m->fps=MAX(1u,1000000u/r->period_ms);m->aperture=lens_info.aperture;m->focal=lens_info.focal_len;
    m->raw_api=raw_info.api_version;m->dynamic_range=raw_info.dynamic_range;
}
/* LCM67: the DNG staging buffer. Up to 4 MiB: each FIO_WriteFile is a
 * round trip to Canon's file task and its own card command, so the writes
 * are as large as memory allows (LCM66 and before: 16 KiB, ~2000 calls). */
static uint8_t *frlv_scratch(unsigned *size)
{
    static const unsigned sizes[]={4u<<20,1u<<20,256u<<10,16u<<10};
    for (unsigned i=0;i<COUNT(sizes);i++) {
        uint8_t *p=fio_malloc(sizes[i]);
        if (p) { *size=sizes[i];return p; }
    }
    *size=0;return NULL;
}
/* in_place (LCM68, the final DNG only: nothing reads acc afterwards): swap
 * the cached accumulator itself and write from it, like the MLV. LCM67 on
 * the camera: the swap into the uncached fio_malloc staging buffer took
 * 1.24 s of the 2.3 s save, the card write 0.99 s. acc is left in DNG byte
 * order. Otherwise acc is untouched (LVBR/LVDK, still needed). */
static int frlv_save_dng(struct frlv_result *r,uint8_t *acc,unsigned in_place)
{
    const unsigned t0=get_ms_clock();
    struct lc_dng_metadata m;frlv_metadata(r,&m);
    r->save_stage=1;r->save_swap_ms=r->save_write_ms=0;r->save_in_place=in_place;
    unsigned chunk=4u<<20;
    uint8_t *scratch=in_place ? fio_malloc(LC_DNG_HEADER_BYTES):frlv_scratch(&chunk);
    if (!scratch) return 0;
    r->save_chunk=chunk;
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
    unsigned swap_us=0,write_us=0;
    for (unsigned off=0;ok && off<r->bytes;off+=chunk) {
        const unsigned n=MIN(chunk,r->bytes-off),u0=(unsigned)get_us_clock();
        uint8_t *out=in_place ? acc+off:scratch;
        lc_dng_swap_words(out,acc+off,n);
        const unsigned u1=(unsigned)get_us_clock();
        ok=FIO_WriteFile(f,out,n)==(int)n;
        swap_us+=u1-u0;write_us+=(unsigned)get_us_clock()-u1;
        if (ml_shutdown_requested) ok=0;
    }
    FIO_CloseFile(f);fio_free(scratch);
    r->save_swap_ms=swap_us/1000u;r->save_write_ms=write_us/1000u;r->save_ms=get_ms_clock()-t0;
    if (!ok) return 0;
    r->save_stage=5;
    uint32_t size=0;
    if (FIO_GetFileSize(r->dng,&size) || size!=header+r->bytes) return 0;
    r->save_size=size;r->save_stage=6;r->save_ms=get_ms_clock()-t0;return 1;
}
/* LCM67: the result as a one-frame MLV (lv-mlv.h). The frame is written
 * straight from the accumulator, Canon byte order, no copy and no swap. */
static int frlv_save_mlv(struct frlv_result *r,const uint8_t *acc)
{
    const unsigned t0=get_ms_clock();
    struct lc_dng_metadata m;frlv_metadata(r,&m);
    struct lc_mlv_extra x;memset(&x,0,sizeof(x));
    x.guid=((uint64_t)get_us_clock()<<32)^get_ms_clock()^r->merged;x.camera_model=camera_model_id;
    memcpy(x.lens,lens_info.name,MIN(sizeof(x.lens),sizeof(lens_info.name)));x.lens[sizeof(x.lens)-1]=0;
    struct tm t;LoadCalendarFromRTC(&t);
    const int tm[9]={t.tm_sec,t.tm_min,t.tm_hour,t.tm_mday,t.tm_mon,t.tm_year,t.tm_wday,t.tm_yday,t.tm_isdst};
    for (unsigned i=0;i<9;i++) x.tm[i]=(uint16_t)tm[i];
    uint8_t *h=fio_malloc(LC_MLV_HEADER_BYTES);
    if (!h) return 0;
    const unsigned header=lc_mlv_header(h,&m,&x);
    if (!header) { fio_free(h);return 0; }
    FILE *f=FIO_CreateFile(r->mlv);
    if (!f || f==(void *)-1) { fio_free(h);return 0; }
    int ok=FIO_WriteFile(f,h,header)==(int)header;
    fio_free(h);
    const unsigned u0=(unsigned)get_us_clock();
    for (unsigned off=0;ok && off<r->bytes;off+=4u<<20) {   /* 4 MiB: shutdown checked between */
        const unsigned n=MIN(4u<<20,r->bytes-off);
        ok=FIO_WriteFile(f,acc+off,n)==(int)n;
        if (ml_shutdown_requested) ok=0;
    }
    r->mlv_write_ms=((unsigned)get_us_clock()-u0)/1000u;
    FIO_CloseFile(f);
    r->mlv_ms=get_ms_clock()-t0;
    if (!ok) return 0;
    uint32_t size=0;
    if (FIO_GetFileSize(r->mlv,&size) || size!=header+r->bytes) return 0;
    r->mlv_size=size;r->mlv_ms=get_ms_clock()-t0;return 1;
}
/* LCM59: read a DNG written by frlv_save_dng back into a raster (Canon
 * byte order). 1 = all r->bytes read. */
static int frlv_load_dng(const struct frlv_result *r,const char *path,uint8_t *dst)
{
    if (r->width%8) return 0;
    unsigned chunk;
    uint8_t *scratch=frlv_scratch(&chunk);
    if (!scratch) return 0;
    FILE *f=FIO_OpenFile(path,O_RDONLY);
    if (!f || f==(void *)-1) { fio_free(scratch);return 0; }
    int ok=1;
    for (unsigned h=0;ok && h<LC_DNG_HEADER_BYTES;) {           /* skip the header */
        const unsigned n=MIN(chunk,LC_DNG_HEADER_BYTES-h);
        ok=FIO_ReadFile(f,scratch,n)==(int)n;h+=n;
    }
    for (unsigned off=0;ok && off<r->bytes;off+=chunk) {
        const unsigned n=MIN(chunk,r->bytes-off);
        ok=FIO_ReadFile(f,scratch,n)==(int)n;
        if (ok) lc_dng_swap_words(dst+off,scratch,n);          /* the swap is its own inverse */
        if (ml_shutdown_requested) ok=0;
    }
    FIO_CloseFile(f);fio_free(scratch);
    return ok;
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
    own_abort=1;AbortEDmac(raw_write_chan);own_abort=0;
    for (unsigned i=0;i<30 && edmac_get_state(raw_write_chan);i++) msleep(10);
    if (edmac_get_state(raw_write_chan)) return 0;
    in_flight=0;return 1;
}
/* Length of the frame that ended at vsync number n (1-based count). */
static unsigned frame_interval_ms(unsigned n)
{
    if (n<2) return 0;
    return (vsync_us[(n-1)%FRLV_VSYNC_RING]-vsync_us[(n-2)%FRLV_VSYNC_RING])/1000u;
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
    const unsigned bl=r->acc_black ? r->acc_black:r->black;
    const int black=bl ? (int)bl:2048;
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
/* The copy into Canon's three LV buffers is done by EDMAC (LCM64; camera:
 * ~6 ms instead of ~110-150 ms by memcpy from the uncached preview buffer),
 * while the session holds edmac_memcpy's resources; memcpy otherwise. */
static unsigned preview_dma_on __attribute__((unused));
static void preview_show(void)
{
    if (!preview_yuv) return;
    for (unsigned i=0;i<3;i++) {
        uint8_t *dst=(uint8_t *)livecomp_camera.lv_yuv[i];
        if (!dst) continue;
#ifdef __ARM__
        if (preview_dma_on) { edmac_memcpy(dst,preview_yuv,preview_pitch*preview_h);continue; }
#endif
        memcpy(dst,preview_yuv,preview_pitch*preview_h);
    }
}
/* Composite merge (LCM64): the hand-written ARM select-max with LDM loads
 * on the camera (raw14-max-asm.h; 1.46-1.52 s per frame vs 1.54-1.78 s for
 * the C kernel, LCB3), same result; it does not count changed pairs, so
 * the CSV column changed_pct is 0. The host simulator runs the C kernel. */
static unsigned merge_max(uint32_t *d,const uint32_t *s,unsigned pairs)
{
#ifdef __ARM__
    return lc_raw14_max_ldm(d,s,pairs);
#else
    return lc_raw14_max_sel(d,s,pairs);
#endif
}
/* Live Bulb add and dark-frame subtract (LCM65): the ARM kernels with LDM
 * loads on the camera (raw14-add-asm.h; 199 instructions per 16 pixels vs
 * 241 for lc_raw14_add_sel, emulated), same result; C in the simulator. */
static void merge_add(uint8_t *d,const uint8_t *s,unsigned pairs,unsigned black,unsigned white)
{
#ifdef __ARM__
    lc_raw14_add_ldm((uint32_t *)(void *)d,(const uint32_t *)(const void *)s,pairs,black,white);
#else
    lc_raw14_add_sel((uint32_t *)(void *)d,(const uint32_t *)(const void *)s,pairs,black,white);
#endif
}
static void merge_sub(uint8_t *d,const uint8_t *s,unsigned pairs,unsigned black,unsigned white)
{
#ifdef __ARM__
    lc_raw14_sub_ldm((uint32_t *)(void *)d,(const uint32_t *)(const void *)s,pairs,black,white);
#else
    lc_raw14_sub_sel((uint32_t *)(void *)d,(const uint32_t *)(const void *)s,pairs,black,white);
#endif
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
    if (ml_shutdown_requested || !r->log[0]) return;
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
/* Session messages are drawn straight onto the BMP overlay. NotifyBox
 * calls redraw() whenever its text changes or times out, and that redraw
 * makes Canon draw its x5 zoom box and arrows again (LCM50 camera test).
 * Each call also clears the overlay first, so a Canon redraw never stays. */
static char session_line[48];
static void show_text(const char *text)
{
    snprintf(session_line,sizeof(session_line),"%s",text);
    clrscr();
    bmp_printf(FONT(FONT_LARGE,COLOR_WHITE,COLOR_BLACK),30,20,"%s",session_line);
}
static unsigned battery_low(struct frlv_result *r)
{
    if (r->battery_stop || !r->battery_start || battery_level_bars>0) return 0;
    r->battery_stop=1;show_text("Battery low: saving");
    return 1;
}
/* ---- Preview only (menu option) -------------------------------------
 * One frame with the current shutter, ISO and aperture, shown on the LCD
 * with its settings until SET (or 60 s); nothing is saved. The user then
 * adjusts with the normal Canon/ML controls and starts again. (LCM43-47 had
 * a live preview with its own controls inside the session; on the camera
 * it mixed with Canon's UI and was replaced.) */
static void preview_info(const struct frlv_result *r)
{
    static char a[80];
    const unsigned iso=r->iso,ap=r->aperture,ms=r->period_ms;
    snprintf(a,sizeof(a),"PREVIEW  %d.%d s  ISO %d  f/%d.%d  clipped %d.%d%%     ",
        ms/1000,ms%1000/100,iso,ap/10,ap%10,r->clip_x10/10,r->clip_x10%10);
    bmp_printf(FONT(FONT_MED,COLOR_WHITE,COLOR_BLACK),10,8,"%s",a);
    bmp_printf(FONT(FONT_MED,COLOR_WHITE,COLOR_BLACK),10,40,"%s","Not saved.  SET: back to Live View   ");
}
static void preview_hold(struct frlv_result *r,const uint8_t *acc,const volatile unsigned *cancel)
{
    if (!preview_yuv) return;
    const unsigned clipped=preview_render(r,acc,0,1);sync_caches();
    r->clip_x10=clipped*1000u/MAX(1u,(preview_w/2)*(preview_h/2));
    preview_show();preview_info(r);
    /* Canon keeps refilling its LV buffers once per (long) frame period:
     * cover each refill right after the vsync, redraw the text now and then. */
    unsigned v=vsyncs,t0=get_ms_clock(),drawn=t0;
    while (!*cancel && !ml_shutdown_requested && zoom5() && get_ms_clock()-t0<60000) {
        if (vsyncs!=v) { v=vsyncs;preview_show(); }
        if (get_ms_clock()-drawn>1000) { clrscr();preview_info(r);drawn=get_ms_clock(); }
        msleep(20);
    }
    clrscr();
}
/* LCM59: dark frames after the last light frame (Live Bulb, menu option),
 * like Canon's long exposure noise reduction: as many frames as the bulb
 * has, with the same shutter, so the dark lasts as long as the exposure.
 * (LCM58 used one dark frame: LVBL0017/19 showed that the column pattern
 * changes from frame to frame - one dark frame x N added ~13 DN of bars -
 * and warm pixels too weak to see in one frame add up over N.)
 * The sensor keeps running at the session shutter (no exit switch). A 10 s
 * countdown asks to cover the lens (SET skips); the first dark frame is one
 * whose every row started after the countdown (rolling shutter: the first
 * rows of the frame ending at vsync V+1 started up to one readout before V).
 * Only two 32 MB buffers exist (LCM9-13), so:
 *  - Save unprocessed OFF: each dark frame is subtracted from the sum as it
 *    arrives (merge_sub, like the add, during the next exposure).
 *  - Save unprocessed ON: the light sum is first saved (LVBRnnnn.DNG), the
 *    dark frames are summed in the same buffer (saved as LVDKnnnn.DNG at
 *    the end) and the light sum is read back from the card to subtract.
 * SET during the dark frames stops after the current one (fewer darks). */
/* LCM61, Live Composite: ONE dark frame at the session shutter. A maximum
 * keeps each pixel from one frame, so hot pixels and dark current are in it
 * once, not N times; max(L_k - D) = max(L_k) - D, so subtracting it at the
 * end equals cleaning every frame. The column correction then removes the
 * column pattern the dark frame may carry and the composite not (it
 * switches from frame to frame). Unprocessed: LVCR, dark: LVCK. */
enum { DK_OFF=0,DK_SKIPPED=1,DK_FAILED=2,DK_DONE=3,DK_PARTIAL=4 };
static const char *orig_prefix(const struct frlv_result *r) { return r->mode==LC_MODE_BULB ? "LVBR":"LVCR"; }
static const char *dark_prefix(const struct frlv_result *r) { return r->mode==LC_MODE_BULB ? "LVDK":"LVCK"; }
#define FRLV_DARK_COUNTDOWN_MS 10000u
static void path_prefix(char *dst,unsigned size,const char *src,const char *prefix)
{
    snprintf(dst,size,"%s",src);
    const unsigned L=strlen(dst);
    if (L>=12) memcpy(dst+L-12,prefix,4);
}
/* One frame into dst in FRLV_BANDS bands: kind 0 copy, 1 add, 2 subtract.
 * Returns the bands lost to the next readout (only when it is armed). */
static unsigned merge_frame(const struct frlv_result *r,uint8_t *dst,const uint8_t *src,const uint8_t *cap,unsigned kind,unsigned black)
{
    const unsigned pairs=r->bytes/28,white=frlv_white(black);
    for (unsigned b=0;b<FRLV_BANDS;b++) {
        const unsigned b0=pairs*b/FRLV_BANDS,b1=pairs*(b+1)/FRLV_BANDS;
        unsigned off=in_flight ? edmac_get_pointer(raw_write_chan)-physical(cap):0;
        if (off && off<=r->bytes && off>b0*28) return FRLV_BANDS-b;
        if (kind==0) memcpy(dst+b0*28,src+b0*28,(b1-b0)*28);
        else if (kind==1) merge_add(dst+b0*28,src+b0*28,b1-b0,black,white);
        else merge_sub(dst+b0*28,src+b0*28,b1-b0,black,white);
        msleep(10);
    }
    return 0;
}
static void dark_capture(struct frlv_result *r,uint8_t *cap,uint8_t *acc,const volatile unsigned *cancel,unsigned period_ms,
    unsigned *dma_uncertain)
{
    static char line[64];
    const unsigned N=r->mode==LC_MODE_BULB ? r->merged:1u,t0=get_ms_clock();
    unsigned v=vsyncs,shown=~0u;
    r->dark_state=DK_SKIPPED;
    /* Save unprocessed: the light sum first (the buffer is reused). */
    if (r->save_orig) {
        static struct frlv_result orig;
        show_text("Saving the unprocessed DNG...");
        orig=*r;path_prefix(orig.dng,sizeof(orig.dng),r->dng,orig_prefix(r));
        r->orig_saved=frlv_save_dng(&orig,acc,0);r->orig_save_ms=orig.save_ms;
        r->dark_sep=r->orig_saved;          /* else: subtract in place */
    }
    const unsigned c0=*cancel,tc=get_ms_clock();
    for (;;) {
        const unsigned el=get_ms_clock()-tc;
        if (el>=FRLV_DARK_COUNTDOWN_MS) break;
        if (*cancel!=c0 || ml_shutdown_requested || !zoom5() || window_lost()) { r->dark_sep=0;show_text("Dark frames skipped");return; }
        if (vsyncs!=v) { v=vsyncs;preview_show(); }
        const unsigned left=(FRLV_DARK_COUNTDOWN_MS-el+999u)/1000u;
        if (left!=shown) { shown=left;snprintf(line,sizeof(line),"COVER THE LENS: dark frames in %d s  SET=skip",left);show_text(line); }
        msleep(50);
    }
    const unsigned covered=get_ms_clock(),readout_ms=150u*((cfg.timer_a+1u)/1388u)+300u;
    r->dark_state=DK_FAILED;
    snprintf(line,sizeof(line),"Dark: keep the lens covered (%dm%02ds)",(N+1)*period_ms/60000u,(N+1)*period_ms/1000u%60u);show_text(line);
    for (;;) {                                       /* the frame boundary to start at */
        v=vsyncs;
        while (vsyncs==v && get_ms_clock()-covered<2*period_ms+3000u) {
            if (ml_shutdown_requested || !zoom5() || window_lost()) return;
            msleep(10);
        }
        if (vsyncs==v) return;
        preview_show();
        if (last_vsync_ms-covered>=readout_ms) break;
    }
    /* Arm the next dark frame before adding this one only if the add ends
     * before the next readout starts writing (else every other frame). */
    const unsigned merge_ms=r->merge_max_us/1000u+300u;
    const unsigned overlap=merge_ms+readout_ms+300u<period_ms;
    const unsigned kind_first=r->dark_sep ? 0u:2u,kind_next=r->dark_sep ? 1u:2u;
    const unsigned black=r->black ? r->black:2048;
    unsigned stop=0,last_switch=0;
    msleep(40);
    unsigned seen=frames_done,a0=get_ms_clock();
    arm_from_task();
    shown=~0u;
    while (r->dark_frames<N) {
        while (frames_done==seen && in_flight && get_ms_clock()-a0<period_ms+3000u) {
            if (vsyncs!=v) { v=vsyncs;preview_show(); }
            if (*cancel!=c0 && !stop) stop=1;
            const unsigned left=(N-r->dark_frames)*period_ms/1000u-MIN((get_ms_clock()-a0)/1000u,period_ms/1000u);
            if (left!=shown) {
                shown=left;
                snprintf(line,sizeof(line),stop ? "DARK %d/%d  last one  (lens covered)":"DARK %d/%d  %ds  SET=stop",r->dark_frames+1,N,left);
                show_text(line);
            }
            /* Back to 3 fps at the vsync that ends the last dark frame. */
            if ((r->dark_frames+1>=N || stop) && !r->exit_switched && get_ms_clock()-a0>2*LC_FRR_ENTRY_MS+100) {
                const uint32_t irq=cli();pend_a=LC_FRR_ENTRY_A;pend_b=LC_FRR_ENTRY_B;pending_timers=1;sei(irq);
                r->exit_switched=1;last_switch=1;
            }
            if (!zoom5() || window_lost()) break;
            msleep(20);
        }
        if (frames_done==seen || in_flight) {        /* not completed: stop our DMA */
            if (!raw_dma_abort()) *dma_uncertain=1;
            break;
        }
        seen=frames_done;
        const unsigned complete=!done_state && done_pointer==physical(cap)+r->bytes && guards_ok(cap,r->bytes);
        const unsigned last=r->dark_frames+1>=N || stop;
        if (!complete) {
            r->dark_partials++;
            if (r->dark_partials>2+N/4 || last_switch) break;
            msleep(40);a0=get_ms_clock();arm_from_task();
            continue;
        }
        if (!last && overlap) { msleep(40);a0=get_ms_clock();arm_from_task(); }
        sync_caches();
        const unsigned m0=(unsigned)get_us_clock();
        const unsigned lost=merge_frame(r,acc,(const uint8_t *)physical(cap),cap,r->dark_frames ? kind_next:kind_first,black);
        r->dark_merge_us=MAX(r->dark_merge_us,(unsigned)get_us_clock()-m0);
        if (lost) { r->dark_lost++;break; }          /* rows already mixed: stop here */
        r->dark_frames++;
        if (last) break;
        if (!overlap) {                              /* next frame boundary, then arm */
            v=vsyncs;
            while (vsyncs==v && get_ms_clock()-a0<2*period_ms+3000u) msleep(10);
            msleep(40);a0=get_ms_clock();arm_from_task();
        }
    }
    if (in_flight && !raw_dma_abort()) *dma_uncertain=1;
    r->dark_wait_ms=get_ms_clock()-t0;
    r->dark_state=r->dark_frames>=N ? DK_DONE:r->dark_frames ? DK_PARTIAL:DK_FAILED;
    show_text(r->dark_frames ? "Dark frames done: lens can be uncovered":"Dark frames failed");
}
static void continuous_capture(struct frlv_result *r,uint8_t *cap,uint8_t *acc,
    const volatile unsigned *cancel,const char **msg,unsigned *dma_uncertain,unsigned period_ms,
    unsigned preview_only)
{
    unsigned lost_ticks=0;
    preview_setup();r->preview_ok=preview_yuv!=NULL;
#ifdef __ARM__
    if (preview_yuv && preview_pitch*preview_h%16u==0) { edmac_memcpy_res_lock();preview_dma_on=1; }
#endif
    const unsigned end=physical(cap)+r->bytes,pairs=r->bytes/28;
    static char line[64];
    unsigned last_engio=stats.engio_calls,last_adtg=stats.adtg_calls;
    cont_arm=1;
    /* First arm. Any time from 40 ms after a vsync (LCM15/17: never inside
     * it, the previous readout may still be ending) until the readout at
     * the end of the period (~150 ms x k, k = timer A / 1388) captures the
     * next whole frame. The caller returns right after the x5 switch's
     * transitional frame, so normally we arm at once instead of waiting a
     * full period (LCM48: one shutter time of the "Starting" delay). */
    unsigned seen=frames_done,w0=get_ms_clock();
    const unsigned readout_ms=150u*((cfg.timer_a+1u)/1388u)+300u;
    unsigned since=((unsigned)get_us_clock()-last_vsync_us)/1000u;
    if (since<40u) msleep(40u-since);
    else if (since+readout_ms>=period_ms) {
        const unsigned v=vsyncs;
        while (vsyncs==v && get_ms_clock()-w0<period_ms+3000) msleep(10);
        msleep(40);
    }
    arm_from_task();
    r->first_arm_after_vsync_ms=((unsigned)get_us_clock()-last_vsync_us)/1000u;
    write_log(r,"capturing");                    /* after arming: not on the critical path */
    {
        static char t[48];
        snprintf(t,sizeof(t),"%s: first frame in %d.%d s",preview_only ? "Preview":mode_word(r),
            period_ms/1000,period_ms%1000/100);
        *msg=preview_only ? "Preview: exposing":is_bulb(r) ? "Bulb: first frame...":"Composite: first frame...";show_text(t);
    }
    for (;;) {
        if (*cancel || ml_shutdown_requested || !zoom5() || raw_lv_is_enabled() || r->merged>=r->limit || battery_low(r)) {
            if (cont_arm && *cancel) show_text("Stopping after this frame");
            cont_arm=0;
        }
        w0=get_ms_clock();
        while (frames_done==seen && in_flight && get_ms_clock()-w0<period_ms+3000) {
            if (*cancel && cont_arm) { cont_arm=0;show_text("Stopping after this frame"); }
            if (ml_shutdown_requested || battery_low(r)) cont_arm=0;
            /* Last frame in flight: go back to 3 fps at the vsync where it
             * completes, so leaving x5 does not wait another long frame. */
            /* Only once the frame in flight has clearly outlasted a 3 fps frame
             * (so it is a full-shutter frame whose end is the right moment;
             * a late-latched short frame is never followed by the switch). */
            /* LCM59: before the first full frame only within a real long
             * frame (not the short transitional one: SET during it now
             * still captures the first full frame). */
            if ((!cont_arm || r->merged+1>=r->limit) && !r->exit_switched && !r->dark_want &&
                get_ms_clock()-last_vsync_ms>2*LC_FRR_ENTRY_MS+100 &&
                (r->merged || get_ms_clock()-last_vsync_ms>period_ms*9u/10u)) {
                const uint32_t irq=cli();pend_a=LC_FRR_ENTRY_A;pend_b=LC_FRR_ENTRY_B;pending_timers=1;sei(irq);
                r->exit_switched=1;
            }
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
        if (frames_done!=seen+1) {
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
        /* First frame after the timer switch: if the camera latched the new
         * timers one frame late, this frame is still a short 3 fps one -
         * not a full exposure. Measure it at the vsyncs and skip it. */
        if (!r->merged && frame_interval_ms(done_vsync)*10u<period_ms*9u) {
            r->short_skipped++;
            if (r->short_skipped>3) { cont_arm=0;r->error|=FE_STALL;*msg="Shutter not applied";break; }
            /* LCM59: SET during the first frame means "stop after it" (a
             * single-frame bulb, LVBL0014), not "no frame": still capture
             * the first full one. */
            if ((cont_arm || (!r->merged && !ml_shutdown_requested && !battery_low(r))) && !in_flight) arm_from_task();
            continue;
        }
        /* Next frame: arm now (its readout starts ~period-readout later),
         * never inside the vsync (>= 40 ms after it). preview_show took
         * ~110 ms with memcpy; with the EDMAC copy (LCM64) it is ~6 ms. */
        while (((unsigned)get_us_clock()-last_vsync_us)/1000u<40u) msleep(1);
        /* No extra frame after the last one the limit allows. */
        if (cont_arm && !in_flight && r->merged+1<r->limit) arm_from_task();
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
            /* LCM56: Live Bulb does not add a partial frame at all (LCM55:
             * the 4th frame of every session delivered 6 rows and was
             * counted as a full sub-exposure: the DNG had N-1 frames of
             * light but claimed N). It is a gap of one sub-exposure, the
             * same for every row; the frame count stays exact. */
            if (is_bulb(r)) {
                r->bulb_partials++;
                if (cont_arm && !in_flight && r->merged<r->limit) arm_from_task();
                continue;
            }
        }
        sync_caches();
        const uint8_t *src=(const uint8_t *)physical(cap);
        unsigned s0=(unsigned)get_us_clock(),changed=0;
        if (!r->merged) { statistics(r,src);s0=(unsigned)get_us_clock(); }
        /* Live Bulb adds only the 2-block pairs this readout delivered: in a
         * partial frame the remaining rows still hold the previous frame,
         * which is harmless for a maximum but would be counted twice in a
         * sum. Those rows then lack one sub-exposure (logged as partial). */
        const unsigned got=pointer-(unsigned)physical(cap);   /* wraps if below: 0 pairs */
        const unsigned valid=pointer==end ? pairs:got<=r->bytes ? got/28u:0u;
        const unsigned add_black=r->black ? r->black:2048,add_white=frlv_white(add_black);
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
            else if (is_bulb(r)) {
                const unsigned e=MIN(b1,valid);
                if (b0<e) merge_add(acc+b0*28,src+b0*28,e-b0,add_black,add_white);
                changed+=b1-b0;
            }
            else changed+=merge_max((uint32_t *)(void *)(acc+b0*28),(const uint32_t *)(const void *)(src+b0*28),b1-b0);
            msleep(10);
            /* Acknowledge Q/SET during the ~1.7 s merge too (LCM25). */
            if (*cancel && cont_arm) { cont_arm=0;show_text("Stopping after this frame"); }
        }
        /* LCM55: in Live Bulb a lost band is not harmless (LCM54 LVBL0003:
         * 26 of 30 frames lost the bottom quarter, which then had ~4 frames
         * of exposure instead of 30). Stop after the frame already armed,
         * which is complete, and say the frames are too short. */
        if (is_bulb(r) && c->lost_bands && cont_arm) { cont_arm=0;r->too_slow=1; }
        c->changed_pct=r->merged ? (unsigned)((uint64_t)changed*100/pairs):100;
        if (is_bulb(r)) r->acc_black=ob_mean(r,acc);   /* ~7k samples: cheap */
        c->merge_us=(unsigned)get_us_clock()-s0;
        r->merge_max_us=MAX(r->merge_max_us,c->merge_us);r->merge_sum_ms+=c->merge_us/1000;
        /* Did the next frame's readout start writing while we were reading? */
        unsigned off=in_flight ? edmac_get_pointer(raw_write_chan)-physical(cap):0;
        c->offset_after=off;
        r->merged++;
        if (off && off<=r->bytes) r->overrun_offset=MAX(r->overrun_offset,off);
        /* The next frame completed while we merged the last band(s): those
         * rows hold a mix of two real exposures, harmless for a maximum
         * (for Live Bulb such rows may get one sub-exposure too many or too
         * few; the band check above keeps this to the band in progress).
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
        if (preview_only) { *msg="Preview";continue; }
        if (!cont_arm) {
            if (r->too_slow) snprintf(line,sizeof(line),"Too slow for %d s frames: stopping",r->period_ms/1000u);
            else snprintf(line,sizeof(line),"Stopping: %d frames + last",r->merged);
            *msg=line;
            show_text(line);
        } else {
            if (is_bulb(r)) {
                const unsigned s=total_exposure_ms(r)/1000u;
                if (s>=60) snprintf(line,sizeof(line),"BULB %d  %dm%02ds  SET=stop",r->merged,s/60,s%60);
                else snprintf(line,sizeof(line),"BULB %d  %ds  SET=stop",r->merged,s);
            }
            else snprintf(line,sizeof(line),"COMPOSITE %d  SET=stop",r->merged);
            *msg=line;
            show_text(line);
        }
        if (r->merged%10==1) write_log(r,"composing");
    }
    cont_arm=0;
    if (r->dark_want && r->merged && !r->error && !r->mode_lost && !r->battery_stop && !*dma_uncertain &&
        !ml_shutdown_requested && zoom5() && !window_lost())
        dark_capture(r,cap,acc,cancel,period_ms,dma_uncertain);
    if (preview_only && r->merged && !r->error && !*dma_uncertain) preview_hold(r,acc,cancel);
#ifdef __ARM__
    if (preview_dma_on) { edmac_memcpy_res_unlock();preview_dma_on=0; }
#endif
    if (preview_yuv) { fio_free(preview_yuv);preview_yuv=NULL; }
}

/* Let an armed slurp complete (or stop it). Nonzero: DMA ownership uncertain. */
static unsigned finish_dma(struct frlv_result *r,unsigned period_ms)
{
    unsigned w0=get_ms_clock();
    while (in_flight && !r->mode_lost && get_ms_clock()-w0<period_ms+4000) msleep(20);
    return (in_flight || edmac_get_state(raw_write_chan)) && !raw_dma_abort();
}
/* Back to x1; Canon may need a full slow frame to finish the switch.
 * Returns 1 when Canon's own sensor window is back. */
static unsigned leave_x5(struct frlv_result *r,const volatile unsigned *cancel)
{
    static const volatile unsigned no_cancel;
    (void)cancel;
    set_lv_zoom(1);
    unsigned w0=get_ms_clock();
    while (lv && lv_dispsize!=1 && get_ms_clock()-w0<6000) msleep(50);
    if (lv && lv_dispsize!=1) r->error|=FE_RESTORE;
    w0=get_ms_clock();
    while (shamem_read(0xC0F06804)==((cfg.raw_v<<16)|cfg.raw_h) && get_ms_clock()-w0<12000) msleep(50);
    wait_vsyncs(3,4000,&no_cancel);                 /* settle; not cancellable */
    r->reg_6804_after=shamem_read(0xC0F06804);
    if (r->reg_6804_after==((cfg.raw_v<<16)|cfg.raw_h)) r->error|=FE_RESTORE;
    return !(r->error&FE_RESTORE);
}
unsigned livecomp_frlv_run(struct card_info *card,const struct lc_frlv_options *o,
    const volatile unsigned *cancel,const char **status)
{
    static struct frlv_result r;
    /* LCM56: never two sessions at once. LCM55 LVBL0010/11: a second start
     * while a session was still running cleared this shared state and lost
     * the first session's DNG. */
    static volatile unsigned busy;
    if (busy) { *status="Session already running";return 0; }
    busy=1;
    memset(&r,0,sizeof(r));memset(&stats,0,sizeof(stats));
    lc_frr_configure(&cfg,o->fps_index);
    /* Enter at 3 fps; the chosen shutter is switched in at a vsync once the
     * mode is verified (startup = shutter + ~1.5 s instead of 2 shutters). */
    const unsigned target_a=cfg.timer_a,target_b=cfg.timer_b;
    cfg.timer_a=LC_FRR_ENTRY_A;cfg.timer_b=LC_FRR_ENTRY_B;pending_timers=0;
    r.target_a=target_a;r.target_b=target_b;
    r.period_ms=lc_frr_timing_of(o->fps_index)->period_ms;
    const unsigned period_ms=r.period_ms;
    r.iso=raw2iso(lens_info.raw_iso);r.aperture=lens_info.aperture;r.battery_start=battery_level_bars;
    r.limit=o->preview_only ? 1u:o->limit ? o->limit:~0u;   /* 0 = no limit */
    r.preview_only=o->preview_only;
    r.mode=o->mode==LC_MODE_BULB ? LC_MODE_BULB:LC_MODE_COMPOSITE;
    r.dark_want=o->dark_frame && !o->preview_only;      /* LCM61: composite too */
    r.save_orig=o->save_orig && !o->preview_only;
    r.format=o->format<=LC_FORMAT_MLV ? o->format:LC_FORMAT_DNG;
    r.wb_mode=lens_info.wb_mode;r.kelvin=lens_info.kelvin;
    lc_wb_neutral_for(lens_info.wb_mode,lens_info.kelvin,lens_info.WBGain_R,lens_info.WBGain_G,lens_info.WBGain_B,r.neutral);
    void *alloc[2]={0};uint8_t *cap=NULL,*acc=NULL;
    unsigned hooked[5]={0},power=0,locked=0,old_lock=0,zoomed=0,dma_uncertain=0,gui_hidden=0;
    const char *msg="Checking the camera";
    *status=msg;
    in_flight=0;timer_fixes=0;

    /* 1: state and fingerprints. Only EOS M 2.0.2 reaches this (camera guard). */
    if (o->preview_only && card) {
        /* Preview: nothing saved; with Debug one log, overwritten each time. */
        r.dng[0]=r.log[0]=0;
        if (o->debug) {
            char dir[24];
            if (output_dir(card,dir,sizeof(dir))) snprintf(r.log,sizeof(r.log),"%s/LVPREVW.LOG",dir);
        }
    } else {
        char dir[24];
        if (!card) { r.error|=FE_IO;msg="No card";goto out; }
        if (!output_dir(card,dir,sizeof(dir))) { r.error|=FE_IO;msg="Cannot create DCIM/LIVE";goto out; }
        if (!choose_paths(&r,dir,o->debug)) { r.error|=FE_IO;msg="DCIM/LIVE full (10000 files)";goto out; }
    }
    if (!base_state_ok(cancel,card) || lv_dispsize!=1 || raw_lv_is_enabled() ||
        raw_write_chan!=livecomp_camera.raw_channel) {
        r.error|=FE_STATE|(raw_lv_is_enabled() ? FE_RAWUSER:0);
        msg="Need Movie LV x1, no RAW tools";goto out;
    }
    if (!lens_info.raw_iso) { r.error|=FE_STATE;msg="Set a fixed ISO (not Auto)";goto out; }
    /* Global Draw must be off here (it draws over the LCD composite); the
     * controller turns it off for the session and restores it afterwards. */
    if (get_global_draw_setting()&1) { r.error|=FE_STATE;msg="Global Draw still on";goto out; }
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
    abort_n=0;own_abort=0;vd_zoom=0;
    r.rom_abort=MEM(FRLV_ABORT_FN);
    if ((r.rom_abort&0xFFFF0000u)==0xE92D0000u &&
        !patch_hook_function(FRLV_ABORT_FN,r.rom_abort,abort_hook,"livecomp: abort trace"))
        hooked[4]=r.abort_hooked=1;
    r.hooks=3+hooked[3]+hooked[4];r.stage=3;
    vsyncs=0;max_gap_ms=0;key_n=0;observing=1;active=1;
    write_log(&r,"hooked; entering x5");

    /* 4: x5 zoom makes Canon rewrite the sensor configuration via the hooks. */
    /* x5 at 3 fps (~1 s incl. the short transitional frame), then the
     * first frame at the chosen shutter. */
    {
        static char start[40];
        snprintf(start,sizeof(start),"First frame in about %d s",(period_ms+1999+(o->preview_only ? 0u:FRLV_WARMUP_VDS*LC_FRR_ENTRY_MS))/1000);
        msg=start;*status=msg;
    }
    unsigned t0=get_ms_clock();
    set_lv_zoom(5);zoomed=1;
    while (!(lv_dispsize==5 && zoom5())) {
        if (*cancel || ml_shutdown_requested || get_ms_clock()-t0>6000) { r.error|=FE_ZOOM;break; }
        msleep(50);
    }
    r.zoom_ms=get_ms_clock()-t0;r.zoom_done_ms=get_ms_clock();vd_zoom=vsyncs;
    if (r.error) { msg="x5 zoom did not engage";goto restore; }
    if (!wait_vsyncs(2,3*LC_FRR_ENTRY_MS+4000,cancel)) { r.error|=*cancel ? FE_CANCEL:FE_STALL;msg="No LV frames after switch";goto restore; }
    r.top_left=shamem_read(0xC0F06800);r.bottom_right=shamem_read(0xC0F06804);
    r.reg_6008=shamem_read(0xC0F06008);r.reg_6010=shamem_read(0xC0F06010);
    r.reg_6014=shamem_read(0xC0F06014);r.reg_38024=shamem_read(0xC0F38024);
    r.stage=4;
    if (r.bottom_right!=((cfg.raw_v<<16)|cfg.raw_h)) { r.error|=FE_REGS;msg="Sensor window not applied";goto restore; }
    lc_frr_geometry(r.top_left,r.bottom_right,&r.width,&r.height);
    r.pitch=r.width*14/8;r.bytes=r.pitch*r.height;
    if (r.width<4000 || r.width>5280 || r.width%4 || r.height<3000 || r.height>3528 || r.bytes>capacity) {
        r.error|=FE_SIZE;msg="Unexpected geometry; see LOG";goto restore;
    }
    memset(cap+r.bytes,0x5a,FRLV_GUARD);
    capture_pitch=r.pitch;capture_height=r.height;capture_target=cap;
    r.stage=5;
    /* Hide Canon's zoom overlay (box and arrows): Canon stops drawing to the
     * visible BMP buffer; ML's own text still shows. Restored at the end. */
    canon_gui_disable_front_buffer();gui_hidden=1;clrscr();
    /* LCM62 warm-up (FRLV_WARMUP_VDS): 3 fps frames with throwaway slurps.
     * Not for Preview: its single frame ends before the aborted one. */
    if (!o->preview_only) {
        const unsigned w0=get_ms_clock(),limit=FRLV_WARMUP_VDS*LC_FRR_ENTRY_MS+3000u;
        unsigned v=vsyncs,shown=~0u;
        static char tl[48];
        while (vsyncs-vd_zoom<FRLV_WARMUP_VDS && get_ms_clock()-w0<limit) {
            if (*cancel || ml_shutdown_requested || !zoom5() || window_lost()) break;
            /* LCM63: say why nothing happens yet (first frame = rest + shutter) */
            const unsigned done_vd=vsyncs-vd_zoom,left_vd=done_vd<FRLV_WARMUP_VDS ? FRLV_WARMUP_VDS-done_vd:0u;
            const unsigned left=(left_vd*LC_FRR_ENTRY_MS+period_ms+999u)/1000u;
            if (left!=shown) { shown=left;snprintf(tl,sizeof(tl),"Tactical delay... first frame in %d s",left);show_text(tl); }
            if (vsyncs!=v) {
                v=vsyncs;
                if (!in_flight) { msleep(40);arm_from_task();r.warmup_slurps++; }
            }
            msleep(5);
        }
        if (in_flight) {                             /* the last one ends at its vsync */
            const unsigned v1=vsyncs;
            while (vsyncs==v1 && get_ms_clock()-w0<limit+1000u) msleep(5);
        }
        if (in_flight && !raw_dma_abort()) { dma_uncertain=1;r.error|=FE_DMA;msg="RAW DMA did not stop";goto restore; }
        r.warmup_done=frames_done;r.warmup_ms=get_ms_clock()-w0;r.warmup_aborts=abort_n;
    }
    /* Switch to the chosen shutter at the next vsync. */
    {
        unsigned w0=get_ms_clock();
        const uint32_t irq=cli();pend_a=target_a;pend_b=target_b;pending_timers=1;sei(irq);
        while (pending_timers && get_ms_clock()-w0<3*LC_FRR_ENTRY_MS+2000) msleep(2);
        if (pending_timers) { pending_timers=0;r.error|=FE_STALL;msg="Shutter switch failed";goto restore; }
        r.switch_ms=get_ms_clock()-r.zoom_done_ms;
    }

    /* 6: compose until SET, the frame limit or an error. */
    msg="Running";*status=msg;
    continuous_capture(&r,cap,acc,cancel,&msg,&dma_uncertain,period_ms,o->preview_only);
    if (r.merged && !(r.error&~FE_CANCEL)) r.stage=6;
    *status=msg;

restore:
    /* LCM58/59: a long frame may still be exposing (LVBL0014: SET 3 s into
     * the first 50 s frame; a skipped dark frame). Canon cannot leave x5
     * before it ends (LCM57 gave up after 18 s: FE_RESTORE). And the timers
     * latch one frame late on the camera (short_skipped=1): after the switch
     * back to 3 fps one more long frame may run (LCM58 LVBL0018: FE_RESTORE
     * after a 50 s dark frame). So: switch back to 3 fps if not yet done and
     * wait until a frame is actually short again. */
    if (zoomed && r.stage>=5 && zoom5() && !r.mode_lost && !ml_shutdown_requested) {
        static char wl[48];
        if (cfg.timer_a!=LC_FRR_ENTRY_A || cfg.timer_b!=LC_FRR_ENTRY_B || pending_timers) {
            const uint32_t irq=cli();pend_a=LC_FRR_ENTRY_A;pend_b=LC_FRR_ENTRY_B;pending_timers=1;sei(irq);
        }
        const unsigned w0=get_ms_clock(),limit=2*period_ms+3000u;
        unsigned shown=~0u,v0=vsyncs;
        while (zoom5() && get_ms_clock()-w0<limit) {
            if (!pending_timers && vsyncs!=v0 && frame_interval_ms(vsyncs)<2*LC_FRR_ENTRY_MS) break;
            const unsigned el=get_ms_clock()-w0,since=get_ms_clock()-last_vsync_ms;
            const unsigned left=since<period_ms ? (period_ms-since+999u)/1000u:1u;
            if (el>1500 && left!=shown) { shown=left;snprintf(wl,sizeof(wl),"Ending the current frame: up to %d s",left);show_text(wl); }
            msleep(20);
        }
        r.end_wait_ms=get_ms_clock()-w0;
    }
    {
        unsigned c=vsyncs;r.vsync_count=MIN(c,FRLV_VSYNC_RING);
        r.vsync_first=c>FRLV_VSYNC_RING ? c%FRLV_VSYNC_RING:0;
        for (unsigned k=0;k<FRLV_VSYNC_RING;k++) r.vsync_us[k]=vsync_us[k];
    }
    active=0;
    msg=r.error ? msg:is_bulb(&r) ? "Live Bulb: restoring":"Full-res composite: restoring";*status=msg;
    if (finish_dma(&r,period_ms)) dma_uncertain=1;
    /* LCM60: Canon's drawing back only once at x1. Before, it came back
     * while still in x5 and Canon could draw its zoom/focus box for the
     * (up to one long frame) it took to leave x5. */
    if (zoomed) leave_x5(&r,cancel);
    if (gui_hidden) { canon_gui_enable_front_buffer(0);gui_hidden=0; }
    if (zoomed) { clrscr();redraw(); }
    if (zoomed && !(r.error&FE_RESTORE)) r.stage=MAX(r.stage,7u);
    goto unhook;
hook_fail:
    r.error|=FE_HOOK;msg="Hook failed; nothing changed";
unhook:
    active=0;
    for (int i=4;i>=0;i--) if (hooked[i]) {
        static const uint32_t sites[5]={FRLV_CMOS_WRITE,FRLV_ADTG_WRITE,FRLV_ENGIO_WRITE,FRLV_ENGDRVOUT,FRLV_ABORT_FN};
        if (unpatch_memory(sites[i])) r.unsafe=1;
    }
    observing=0;
out:
    r.timer_fixes=timer_fixes;r.timer_seen_b=timer_seen_b;r.battery_end=battery_level_bars;
    if (dma_uncertain) r.unsafe=1;
    /* The capture buffer is free once no DMA can target it: release it
     * before the (slow) DNG write. A partial composite is still saved. */
    /* LCM59: with separate dark frames (Save unprocessed) the accumulator
     * holds the dark sum; the light sum is read back into the capture
     * buffer, so that one stays allocated until then. */
    const unsigned dark_sep=r.dark_sep && r.dark_frames && !r.unsafe && cap;
    if (!r.unsafe && alloc[0] && !dark_sep) { fio_free(alloc[0]);alloc[0]=NULL;cap=NULL; }
    if (dark_sep && acc && !ml_shutdown_requested && get_ml_card()==card) {
        static struct frlv_result dr;
        dr=r;dr.merged=r.dark_frames;dr.acc_black=ob_mean(&r,acc);r.dark_black=dr.acc_black;
        path_prefix(dr.dng,sizeof(dr.dng),r.dng,dark_prefix(&r));
        *status="Saving the dark frames";NotifyBox(60000,"Saving the dark frames...");
        r.dark_saved=frlv_save_dng(&dr,acc,0);r.dark_save_ms=dr.save_ms;
        static char lpath[FIO_MAX_PATH_LENGTH];
        path_prefix(lpath,sizeof(lpath),r.dng,orig_prefix(&r));
        *status="Reading the unprocessed DNG";NotifyBox(60000,"Subtracting the dark frames...");
        uint8_t *light=(uint8_t *)physical(cap);
        const unsigned l0=get_ms_clock();
        r.orig_read=frlv_load_dng(&r,lpath,light);r.load_ms=get_ms_clock()-l0;
        if (r.orig_read) {
            /* light - (dark sum - its black): the light black level stays */
            const unsigned lb=is_bulb(&r) ? r.acc_black:r.black,pairs=r.bytes/28,white=frlv_white(lb ? lb:2048);
            for (unsigned b=0;b<FRLV_BANDS;b++) {
                const unsigned b0=pairs*b/FRLV_BANDS,b1=pairs*(b+1)/FRLV_BANDS;
                merge_sub(light+b0*28,acc+b0*28,b1-b0,r.dark_black,white);
                msleep(10);
            }
            acc=light;                                   /* the result; both buffers freed below */
        } else {
            r.error|=FE_IO;r.dark_state=DK_FAILED;acc=NULL; /* no light sum in memory: LVBR is the result */
            msg=is_bulb(&r) ? "Dark subtraction failed: see LVBR":"Dark subtraction failed: see LVCR";
        }
    }
    /* Save unprocessed without dark frames: the sum before any correction. */
    if (r.save_orig && !r.orig_saved && acc && r.merged && is_bulb(&r) && !r.unsafe && !ml_shutdown_requested && get_ml_card()==card &&   /* composite: no processing without dark */
        !(r.error&(FE_GUARD|FE_SIZE|FE_REGS)) && !(r.dark_frames && !r.dark_sep)) {
        static struct frlv_result orig;
        orig=r;path_prefix(orig.dng,sizeof(orig.dng),r.dng,orig_prefix(&r));
        *status="Saving the unprocessed DNG";NotifyBox(60000,"Saving the unprocessed DNG...");
        r.orig_saved=frlv_save_dng(&orig,acc,0);r.orig_save_ms=orig.save_ms;
    }
    if (acc && r.merged && !r.preview_only && !r.unsafe && !ml_shutdown_requested && get_ml_card()==card &&
        !(r.error&(FE_GUARD|FE_SIZE|FE_REGS))) {
        if (r.width==5280 && r.height>LC_CF_Y1+8 &&
            (is_bulb(&r) ? r.merged>1:r.dark_state==DK_DONE)) {   /* LCM61: composite after its dark frame */
            /* LCM56: remove the column pattern the sum built up (fullres-fix.h).
             * LCM57: OB rows give the non-periodic part only; the 8-column
             * lanes and a per-column refinement come from flat dark image
             * areas (rows y%8<2), weighted against the OB estimate.
             * LCM59: after dark frames too: the column pattern changes from
             * frame to frame, so light and dark sums do not cancel it fully. */
            static int32_t col_off[5280],cf_s8[5280],cf_lane_ob[8];
            static uint16_t cf_n[5280];
            static int16_t cf_row[5280];
            *status="Column correction";
            unsigned c0=(unsigned)get_us_clock();
            /* LCM57: tell the user once the correction has taken more than 1 s
             * (LCM59: 60 s box; LCM58's 15 s one expired: black screen). */
            unsigned cf_told=0;
            #define CF_TELL() do { if (!cf_told && (unsigned)get_us_clock()-c0>1000000u) { \
                cf_told=1;*status="Applying denoise";NotifyBox(60000,"Applying denoise..."); } } while (0)
            const unsigned cf_lb=is_bulb(&r) ? r.acc_black:r.black,cf_black=cf_lb ? cf_lb:2048;
            r.colfix_rms_x10=lc_colfix_measure(acc,r.pitch,r.width,LC_CF_Y0,LC_CF_Y1,col_off);
            lc_colfix_split_lanes(col_off,r.width,cf_lane_ob);
            r.colfix_vob=lc_colfix_ob_var(acc,r.pitch,r.width,LC_CF_Y0,LC_CF_Y1);
            for (unsigned x=0;x<5280;x++) { cf_s8[x]=0;cf_n[x]=0; }
            struct lc_colfix_scan sc={cf_s8,cf_n,cf_row,0,0,lc_colfix_lr_max(r.colfix_vob)};
            for (unsigned y=28u;y<r.height;y++) {           /* active rows */
                if ((y&7)>=2) continue;
                lc_colfix_scan_row(&sc,acc,r.pitch,r.width,y,col_off,cf_black);
                if ((y&127)==1) { CF_TELL();msleep(1); }   /* keep the camera UI alive */
            }
            r.colfix_dark=sc.cnt;
            r.colfix_img=lc_colfix_combine(col_off,r.width,&sc,cf_lane_ob,r.colfix_vob,r.colfix_lane_x10,&r.colfix_vpix);
            r.colfix_scan_us=(unsigned)get_us_clock()-c0;
            const unsigned white=frlv_white(cf_black);
            for (unsigned y=0;y<r.height;y+=128) {
                lc_colfix_apply(acc,r.pitch,r.width,y,MIN(r.height,y+128),col_off,white);
                CF_TELL();msleep(1);                /* keep the camera UI alive */
            }
            #undef CF_TELL
            r.colfix_us=(unsigned)get_us_clock()-c0;
        }
        if (r.width==5280) {
            *status="Fixing focus pixels";
            unsigned f0=(unsigned)get_us_clock();
            r.fp_fixed=lc_fix_focus_pixels(acc,r.width,r.height,r.pitch);
            r.fp_us=(unsigned)get_us_clock()-f0;
        }
        /* LCM67: MLV first (the same data, no swap), then the DNG, swapped
         * in place in acc (LCM68). */
        if (r.format) {
            *status="Saving the MLV";
            NotifyBox(60000,"Saving the MLV...");
            r.mlv_saved=frlv_save_mlv(&r,acc);
        }
        if (r.format!=LC_FORMAT_MLV) {
            *status="Saving the DNG";
            NotifyBox(60000,"Saving the DNG...");
            r.saved=frlv_save_dng(&r,acc,1);          /* LCM68: last use of acc */
        }
        else r.saved=r.mlv_saved;
        if (!r.saved || (r.format && !r.mlv_saved)) r.error|=FE_IO;
        else r.stage=8;
    }
    if (!r.unsafe) for (unsigned i=0;i<2;i++) if (alloc[i]) fio_free(alloc[i]);
    if (locked) gui_uilock(old_lock);
    if (power) powersave_permit();
    if (r.unsafe) msg="DMA uncertain: reboot camera";
    else if (r.preview_only && r.merged && !(r.error&~FE_CANCEL)) {
        static char pv[64];
        snprintf(pv,sizeof(pv),"Preview: clipped %d.%d%%",r.clip_x10/10,r.clip_x10%10);
        msg=pv;
    }
    else if (!r.error) msg=r.saved ? (is_bulb(&r) ? "Live Bulb saved":"Full-res composite saved"):"Full-res: no frame saved";
    else if (r.saved) msg="Saved; stopped early, see LOG";
    if (r.saved && !r.error && !r.unsafe) {
        static char done[96];
        const unsigned s=total_exposure_ms(&r)/1000u;
        if (r.too_slow) snprintf(done,sizeof(done),"Bulb saved: %d frames; use longer frames",r.merged);
        else if (r.late_frames || r.missed) snprintf(done,sizeof(done),"%s saved: %d (%d late)",mode_word(&r),r.merged,r.late_frames+r.missed);
        else if (r.battery_stop) snprintf(done,sizeof(done),"Battery low: saved %d frames",r.merged);
        else if (is_bulb(&r)) snprintf(done,sizeof(done),"Bulb saved: %d frames, %d s%s",r.merged,s,
            r.dark_state==DK_DONE ? ", dark subtracted":r.dark_state==DK_PARTIAL ? ", dark partial":
            r.dark_state==DK_FAILED ? ", dark failed":"");
        else snprintf(done,sizeof(done),"Composite saved: %d frames%s",r.merged,
            r.dark_state==DK_DONE ? ", dark subtracted":r.dark_state==DK_FAILED ? ", dark failed":"");
        /* LCM67: how long the files took to write (Debug ON; also in the LOG) */
        unsigned k=r.log[0] ? strlen(done):sizeof(done);
        if (r.format!=LC_FORMAT_MLV && k<sizeof(done)) k+=snprintf(done+k,sizeof(done)-k,"; DNG %d.%d s",r.save_ms/1000u,r.save_ms/100u%10u);
        if (r.format && k<sizeof(done)) snprintf(done+k,sizeof(done)-k,"; MLV %d.%d s",r.mlv_ms/1000u,r.mlv_ms/100u%10u);
        msg=done;
    }
    *status=msg;
    if (r.log[0]) write_log(&r,msg);
    if (r.log[0] && !r.preview_only) write_csv(&r);
    busy=0;
    return r.unsafe;
}
