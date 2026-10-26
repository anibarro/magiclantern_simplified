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
#include "vram.h"
#include "fullres-gamma.h"
#include "notify_box.h"
#include "fullres-fix.h"
#include "native-max.h"
#include "fullres-hw.h"
#include "lv-pause-regs.h"

extern int raw_lv_is_enabled(void);
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
/* Canon's max engine reads 4 halo rows before each input band (LCM9-13). */
#define FRLV_HALO (4u*9240u)
#define FRLV_MAX_INPUTS 8u
#define FRLV_MAX_ATTEMPTS 14u        /* inputs + up to 6 partial frames */
#define FRLV_TRACE 24u
#define FRLV_VSYNC_RING 32u
#define FRLV_CONT_MAX 999u
#define FRLV_BANDS 8u                /* continuous merge: bands between ticks */
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
static volatile unsigned want_capture,in_flight,capture_done;
/* Continuous composite: re-arm at every completion; frames_done counts them. */
static volatile unsigned continuous,cont_arm,frames_done;
static volatile unsigned done_cb_kind,done_cb_after,done_cb_ptr,done_arm_after_vsync,arm_vsync_delay,done_engio,done_adtg,last_vsync_us;
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
/* LCM24: every ENGIO register list written while the HW test job runs
 * (Canon's engine start/stop tables and anything Live View writes). */
#define FRLV_REC 24u
#define FRLV_REC_REGS 6u
struct frlv_rec { uint32_t us,list,n,reg[FRLV_REC_REGS],val[FRLV_REC_REGS]; };
static struct frlv_rec rec[FRLV_REC];
static volatile unsigned rec_on,rec_n,rec_t0;
static void FAST engio_record(const uint32_t *list)
{
    if (rec_n>=FRLV_REC) { rec_n++;return; }
    struct frlv_rec *e=&rec[rec_n++];
    e->us=(unsigned)get_us_clock()-rec_t0;e->list=(uint32_t)list;e->n=0;
    for (unsigned i=0;i<512 && list[2*i]!=0xFFFFFFFFu;i++) {
        if (e->n<FRLV_REC_REGS) { e->reg[e->n]=list[2*i];e->val[e->n]=list[2*i+1]; }
        e->n++;
    }
}
static void FAST engio_hook(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;(void)pc;
    if (rec_on) engio_record((const uint32_t *)regs[0]);
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
        done_vsync=vsyncs;done_ms=now;in_flight=0;capture_done=1;frames_done++;
        return;
    }
    if (want_capture && active && zoom5() && !raw_lv_is_enabled()) {
        want_capture=0;
        EngDrvOut(FRLV_RAW_TYPE_REG,FRLV_RAW_TYPE);
        frlv_slurp((void *)physical(capture_target),capture_pitch,capture_height);
        capture_vsync=vsyncs;start_ms=now;start_us=(unsigned)get_us_clock();in_flight=1;
    }
}

struct frlv_hw { unsigned tried,frame,busy,channel,reason[2],engine_us[2],lock[2],setup[2],wait[2],
    completed[2][3],after[2][12],compare_us,mismatch_bytes,first_bad,expected,actual,prepare,close,safe,plane_ok[2];
    struct lc_native_engine_result route[2]; /* LCM23: router evidence per band */
    unsigned paused,lv_before,lv_after; };     /* LCM27: Live View paused run */
static struct frlv_hw hw,hw_paused;   /* LCM27: second run with Live View paused */
/* LCM28 attempt table: one row per engine job, in Live View or paused. */
#define FRLV_TRIES 10u
struct frlv_try { unsigned where,frame,channel,bands,busy,reason,us,completed,out_state,out_bytes,aborted,moved,mismatch_words,first_bad,safe; };
static struct frlv_try tries[FRLV_TRIES];
static unsigned ntries;
/* LCM33: Live View attempts with a register group put in its paused state
 * (lv-pause-regs.h, from the LCM32 whole-window diff) around a borrowed
 * channel-5 job. where 2/3/4 = group A (0xC0F03xxx routing), A+B (plus
 * start/stop flags), A+B+C (plus 0xC0F01xxx). */
static const unsigned pause_group_n[3]={LC_PAUSE_A,LC_PAUSE_AB,LC_PAUSE_ABC};
/* LCM34: Live View steps off (LCM33: step A gave no output and disturbed the
 * display; step B's borrow failed on frame 5 as in LCM29). Instead the
 * reverse test runs with Live View paused: see paused_hw_test. */
#define FRLV_LV_STEPS 0
/* LCM36: LCM35 showed the whole register state of Live View does not stop
 * the paused engine (all 189 values at once: exact). What runs only in
 * Live View is its activity, the obvious part being the LCD refresh. Turn
 * the display off after frame 3, run one borrowed channel-5 job (no
 * register changes) on frame 4 or 6 (never 5: borrow fails there), turn it
 * back on. */
static unsigned disp_off_ms,disp_try_frame;
/* LCM35: 3 = A+B+C together, 4 = A+B+C plus Live View's EDMAC routing
 * (0xC0F05xxx entries of the diff). LCM34: A, B, C alone all stayed exact. */
static const unsigned group_lo[4]={0,LC_PAUSE_A,LC_PAUSE_AB,0},group_hi[4]={LC_PAUSE_A,LC_PAUSE_AB,LC_PAUSE_ABC,LC_PAUSE_ABC};
static unsigned group_write_err[5],pause_ms,resume_ms;
static uint32_t pause_saved[LC_PAUSE_ABC];
static struct lc_native_borrow borrows[6];
static unsigned nborrows,pause_bad[6],pause_write_err[6];
/* LCM32 read-only diagnostic: Canon's ENGIO register shadow is one flat
 * 256 KB RAM copy of 0xC0F00000-0xC0F3FFFF (EngDrvOut FF2C1694 stores the
 * value at shadow_base + (reg & 0x3FFFF); shamem_read FF2C1730 loads it), so
 * reading it never touches a peripheral. Whole window snapshotted on frame 3
 * (Live View running) and compared after PauseLiveView. LCM30's three ranges
 * showed 9 differences that are not the cause (LCM31: Live View rewrites
 * them every frame, and stopping them gave no output). */
#define FRLV_SHADOW_WORDS 0x10000u
#define FRLV_DIFF_MAX 2048u
static uint32_t *snap_lv;                     /* 256 KB, malloc'd when enabled */
static unsigned snap_lv_ok,snap_paused_ok,job_pairs,diff_n,diff_total;
static uint32_t job_list[2*64];
static uint32_t diff_reg[FRLV_DIFF_MAX],diff_lv[FRLV_DIFF_MAX],diff_paused[FRLV_DIFF_MAX];
static uint32_t snap_reg(unsigned i) { return 0xC0F00000u+4u*i; }
static void snap_diff(void)
{
    diff_n=diff_total=0;
    for (unsigned i=0;i<FRLV_SHADOW_WORDS;i++) {
        uint32_t v=shamem_read(snap_reg(i));
        if (v==snap_lv[i]) continue;
        if (diff_n<FRLV_DIFF_MAX) { diff_reg[diff_n]=snap_reg(i);diff_lv[diff_n]=snap_lv[i];diff_paused[diff_n]=v;diff_n++; }
        diff_total++;
    }
}
static void snap_take(uint32_t *v)
{
    for (unsigned i=0;i<FRLV_SHADOW_WORDS;i++) v[i]=shamem_read(snap_reg(i));
}
/* Write-channel activity over one full frame period (frame 2 -> 3): when is
 * each EDMAC write channel busy, and what are channels 3/5 doing. */
#define FRLV_ACT 160u
static const uint8_t act_channels[16]={0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x20,0x21};
struct frlv_act { uint16_t ms,mask;uint32_t ptr3,ptr5; };
static struct frlv_act act[FRLV_ACT];
static unsigned act_n,act_on,act_t0,act_last_mask,act_union,act_info5[6],act_info3[6];
static unsigned act_conn[16],act_state[16];   /* write sources in the idle window */
static void act_sample(void)
{
    if (!act_on || act_n>=FRLV_ACT) return;
    unsigned mask=0;
    for (unsigned i=0;i<16;i++) if (edmac_get_state(act_channels[i])) mask|=1u<<i;
    unsigned p3=edmac_get_pointer(3),p5=edmac_get_pointer(5);
    act_union|=mask;
    if (act_n && mask==act_last_mask && p5==act[act_n-1].ptr5 && p3==act[act_n-1].ptr3) return;
    act[act_n++]=(struct frlv_act){(uint16_t)(get_ms_clock()-act_t0),(uint16_t)mask,p3,p5};
    act_last_mask=mask;
}
static void act_routes(void)
{
    for (unsigned i=0;i<16;i++) {
        act_conn[i]=edmac_get_connection(act_channels[i],EDMAC_DIR_WRITE);
        act_state[i]=edmac_get_state(act_channels[i]);
    }
}
static void act_config(unsigned ch,unsigned *o)
{
    o[0]=edmac_get_address(ch);o[1]=edmac_get_length(ch);o[2]=edmac_get_flags(ch);
    o[3]=edmac_get_connection(ch,1);o[4]=edmac_get_state(ch);o[5]=edmac_get_pointer(ch);
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
    unsigned continuous,limit,overrun_offset,missed,preview_ok,preview_us,preview_copy_us,merge_max_us,merge_sum_ms,preview_skipped,behind,preview_strips;
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
        "Saved_stage_size: %d,%d,%d\nDNG: %s\n"
        "Continuous_limit_overrun_offset_missed: %d,%d,%d,%d\n"
        "Merge_max_us_sum_ms_preview_ok_render_us_copy_us: %d,%d,%d,%d,%d\n"
        "WB_mode_kelvin_neutral_r_g_b: %d,%d,%d,%d,%d\nFocus_pixels_fixed_us: %d,%d\n"
        "Zoom_done_ms_late_frames: %d,%d\nMerge_kernel_bands_preview_skipped_behind_strips: select,%d,%d,%d,%d\n",
        stats.cmos_calls,stats.cmos_changes,stats.adtg_calls,stats.adtg_changes,stats.blanking_seen,
        stats.engio_calls,stats.engio_changes,stats.drv_changes,stats.overflow,
        stats.orig_6804,stats.orig_timer_a,stats.orig_timer_b,stats.orig_blanking,
        r->inputs,r->merged,r->memcpy_us,r->black,r->ob_stdev_x10,r->center_mean,r->max_value,
        r->col_band[0],r->col_band[1],r->col_band[2],r->col_band[3],r->col_band[4],r->col_band[5],
        r->col_band[6],r->col_band[7],r->col_band[8],r->col_band[9],r->col_band[10],r->col_band[11],
        r->row_band[0],r->row_band[1],r->row_band[2],r->row_band[3],r->row_band[4],
        r->row_band[5],r->row_band[6],r->row_band[7],r->row_band[8],r->row_band[9],
        r->saved,r->save_stage,r->save_size,r->dng,
        r->continuous,r->limit,r->overrun_offset,r->missed,
        r->merge_max_us,r->merge_sum_ms,r->preview_ok,r->preview_us,r->preview_copy_us,
        r->wb_mode,r->kelvin,r->neutral[0],r->neutral[1],r->neutral[2],r->fp_fixed,r->fp_us,
        r->zoom_done_ms,r->late_frames,FRLV_BANDS,r->preview_skipped,r->behind,r->preview_strips);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),
        "HW_prepare_close_tried_frame_busy_channel_safe: %d,%d,%d,%d,%x,%d,%d\n"
        "HW_band0_reason_us_lock_setup_wait_completed: %d,%d,%x,%x,%x,%d/%d/%d\n"
        "HW_band1_reason_us_lock_setup_wait_completed: %d,%d,%x,%x,%x,%d/%d/%d\n"
        "HW_band0_after_out_state_flags_addr_ptr: %x,%x,%x,%x\n"
        "HW_compare_us_mismatch_bytes_first_expected_actual: %d,%d,%d,%x,%x\nHW_plane_ok_band0_band1: %x,%x\n",
        hw.prepare,hw.close,hw.tried,hw.frame,hw.busy,hw.channel,hw.safe,
        hw.reason[0],hw.engine_us[0],hw.lock[0],hw.setup[0],hw.wait[0],hw.completed[0][0],hw.completed[0][1],hw.completed[0][2],
        hw.reason[1],hw.engine_us[1],hw.lock[1],hw.setup[1],hw.wait[1],hw.completed[1][0],hw.completed[1][1],hw.completed[1][2],
        hw.after[0][8],hw.after[0][9],hw.after[0][10],hw.after[0][11],
        hw.compare_us,hw.mismatch_bytes,hw.first_bad,hw.expected,hw.actual,hw.plane_ok[0],hw.plane_ok[1]);
    ok&=write_text(f,t,n);
    for (unsigned band=0;band<2;band++) {
        const struct lc_native_engine_result *q=&hw.route[band];
        n=snprintf(t,sizeof(t),"HW_band%d_outconn_listeners_divert_src_mask_restored_aborted: %x,%d,%d,%x,%d,%d\n"
            "HW_band%d_conn_after_setup (ch:src):",band,q->out_conn,q->listeners,q->divert_source,q->diverted,
            q->route_restored,q->aborted,band);
        for (unsigned i=0;i<LC_NATIVE_WRITE_CHANNELS;i++) n+=snprintf(t+n,sizeof(t)-n," %x:%x",lc_native_write_channels[i],q->conn[i]);
        n+=snprintf(t+n,sizeof(t)-n,"\nHW_band%d_listener_ch_state_addr_ptr_before_after:",band);
        for (unsigned k=0;k<q->listeners && k<LC_NATIVE_MAX_LISTENERS;k++)
            n+=snprintf(t+n,sizeof(t)-n," %x:%x:%x:%x:%x",q->lst_ch[k],q->lst_state[k],q->lst_addr[k],q->lst_ptr0[k],q->lst_ptr1[k]);
        n+=snprintf(t+n,sizeof(t)-n,"\nHW_band%d_out_before_state_addr_ptr_after_state_addr_ptr: %x,%x,%x,%x,%x,%x\n",band,
            q->before_start[8],q->before_start[10],q->before_start[11],q->after_wait[8],q->after_wait[10],q->after_wait[11]);
        ok&=write_text(f,t,n);
    }
    for (unsigned k=0;k<2;k++) {
        const struct frlv_hw *h=k ? &hw_paused:&hw;
        const struct lc_native_engine_result *q=&h->route[0];
        n=snprintf(t,sizeof(t),"%s_band0_moved (ch:state:ptr>state:ptr):",k ? "HWP":"HW");
        for (unsigned i=0;i<LC_NATIVE_WRITE_CHANNELS;i++)
            if (q->all_ptr0[i]!=q->all_ptr1[i] || q->all_state0[i]!=q->all_state1[i] || lc_native_write_channels[i]==5)
                n+=snprintf(t+n,sizeof(t)-n," %x:%x:%x>%x:%x",lc_native_write_channels[i],
                    q->all_state0[i],q->all_ptr0[i],q->all_state1[i],q->all_ptr1[i]);
        t[n++]='\n';ok&=write_text(f,t,n);
    }
    {
        const struct frlv_hw *h=&hw_paused;
        n=snprintf(t,sizeof(t),
            "HWP_lv_before_paused_after_prepare_close_tried_busy_channel_safe: %d,%d,%d,%d,%d,%d,%x,%d,%d\n"
            "HWP_band0_reason_us_lock_setup_wait_completed: %d,%d,%x,%x,%x,%d/%d/%d\n"
            "HWP_band1_reason_us_lock_setup_wait_completed: %d,%d,%x,%x,%x,%d/%d/%d\n"
            "HWP_band0_out_after_state_addr_ptr_aborted: %x,%x,%x,%d\n"
            "HWP_compare_us_mismatch_bytes_first_expected_actual: %d,%d,%d,%x,%x\n",
            h->lv_before,h->paused,h->lv_after,h->prepare,h->close,h->tried,h->busy,h->channel,h->safe,
            h->reason[0],h->engine_us[0],h->lock[0],h->setup[0],h->wait[0],h->completed[0][0],h->completed[0][1],h->completed[0][2],
            h->reason[1],h->engine_us[1],h->lock[1],h->setup[1],h->wait[1],h->completed[1][0],h->completed[1][1],h->completed[1][2],
            h->after[0][8],h->after[0][10],h->after[0][11],h->route[0].aborted,
            h->compare_us,h->mismatch_bytes,h->first_bad,h->expected,h->actual);
        ok&=write_text(f,t,n);
    }
    n=snprintf(t,sizeof(t),"HW_tries: where(0=LV,1=paused),frame,ch,bands,busy,reason,us,out_completions,out_state,out_bytes,aborted,moved_mask,mismatch_words,first_bad,safe\n");
    ok&=write_text(f,t,n);
    for (unsigned k=0;k<ntries;k++) {
        const struct frlv_try *q=&tries[k];
        n=snprintf(t,sizeof(t),"T%d: %d,%d,%d,%d,%x,%x,%d,%d,%x,%d,%d,%x,%d,%d,%d\n",k,q->where,q->frame,q->channel,q->bands,
            q->busy,q->reason,q->us,q->completed,q->out_state,q->out_bytes,q->aborted,q->moved,q->mismatch_words,(int)q->first_bad,q->safe);
        ok&=write_text(f,t,n);
    }
    n=snprintf(t,sizeof(t),"HW_borrow5: reason,state,addr,ptr,flags,conn,abort_wait_us,restored,after_state,after_addr,pause_write_err,pause_restore_bad\n");
    ok&=write_text(f,t,n);
    for (unsigned k=0;k<nborrows && k<6;k++) {
        const struct lc_native_borrow *q=&borrows[k];
        n=snprintf(t,sizeof(t),"B%d: %x,%x,%x,%x,%x,%x,%d,%d,%x,%x,%d,%d\n",k,q->reason,q->state,q->addr,q->ptr,
            q->flags,q->conn,q->abort_wait_us,q->restored,q->after_state,q->after_addr,pause_write_err[k],pause_bad[k]);
        ok&=write_text(f,t,n);
    }
    n=snprintf(t,sizeof(t),"Display_off_after_frame3_ms_try_frame: %d,%d (HW_tries where 5)\npause_ms=%d resume_ms=%d\n",
        disp_off_ms,disp_try_frame,pause_ms,resume_ms);
    ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Job_config_list (%d pairs, reg=val):",job_pairs);
    for (unsigned k=0;k<job_pairs;k++) {
        if (n>(int)sizeof(t)-40) { ok&=write_text(f,t,n);n=0; }
        n+=snprintf(t+n,sizeof(t)-n," %x=%x",job_list[2*k],job_list[2*k+1]);
    }
    t[n++]='\n';ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"Reg_diff_LV_vs_paused (window C0F00000-C0F3FFFC; lv_ok %d paused_ok %d; total %d, listed %d; reg:lv:paused:in_job_list):",
        snap_lv_ok,snap_paused_ok,diff_total,diff_n);
    for (unsigned k=0;k<diff_n;k++) {
        unsigned in_job=0;
        for (unsigned q=0;q<job_pairs;q++) if (job_list[2*q]==diff_reg[k]) in_job=1;
        if (n>(int)sizeof(t)-48) { ok&=write_text(f,t,n);n=0; }
        n+=snprintf(t+n,sizeof(t)-n,"%s%x:%x:%x:%d",k%4 ? " ":"\n",diff_reg[k],diff_lv[k],diff_paused[k],in_job);
    }
    t[n++]='\n';ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"HW_engio_lists_during_band0: %d (us:list:count reg=val ...)\n",rec_n);
    ok&=write_text(f,t,n);
    for (unsigned k=0;k<rec_n && k<FRLV_REC;k++) {
        n=snprintf(t,sizeof(t),"%d:%x:%d",rec[k].us,rec[k].list,rec[k].n);
        for (unsigned i=0;i<rec[k].n && i<FRLV_REC_REGS;i++) n+=snprintf(t+n,sizeof(t)-n," %x=%x",rec[k].reg[i],rec[k].val[i]);
        t[n++]='\n';ok&=write_text(f,t,n);
    }
    n=snprintf(t,sizeof(t),"Write_route_idle (ch:src:state):");
    for (unsigned i=0;i<16;i++) n+=snprintf(t+n,sizeof(t)-n," %x:%x:%x",act_channels[i],act_conn[i],act_state[i]);
    t[n++]='\n';ok&=write_text(f,t,n);
    n=snprintf(t,sizeof(t),"DMA_activity_samples_union_mask: %d,%x (bit i = write channel 0..6,0x10..0x16,0x20,0x21)\n"
        "Ch5_addr_len_flags_conn_state_ptr: %x,%x,%x,%x,%x,%x\nCh3_addr_len_flags_conn_state_ptr: %x,%x,%x,%x,%x,%x\n"
        "Activity_ms_after_vsync_mask_ptr3_ptr5:\n",
        act_n,act_union,act_info5[0],act_info5[1],act_info5[2],act_info5[3],act_info5[4],act_info5[5],
        act_info3[0],act_info3[1],act_info3[2],act_info3[3],act_info3[4],act_info3[5]);
    ok&=write_text(f,t,n);
    for (unsigned i=0;i<act_n;i+=4) {
        n=0;
        for (unsigned k=i;k<act_n && k<i+4;k++)
            n+=snprintf(t+n,sizeof(t)-n,"%d:%x:%x:%x ",act[k].ms,act[k].mask,act[k].ptr3,act[k].ptr5);
        t[n++]='\n';ok&=write_text(f,t,n);
    }
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
    memcpy(m.neutral,r->neutral,sizeof(m.neutral));
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

/* Arm the next frame from the worker, well after the vsync. LCM15/17 showed
 * occasional partial frames when arming inside the vsync callback: the
 * readout of the frame that just ended can still be finishing (~2 ms margin),
 * so a slurp started then takes its last rows and stops at frame end. */
static void arm_from_task(void)
{
    EngDrvOut(FRLV_RAW_TYPE_REG,FRLV_RAW_TYPE);
    arm_vsync_delay=(unsigned)get_us_clock()-last_vsync_us;
    frlv_slurp((void *)physical(capture_target),capture_pitch,capture_height);
    capture_vsync=vsyncs;start_ms=get_ms_clock();start_us=(unsigned)get_us_clock();
    in_flight=1;
}

/* ---------------------------------------------------------------------
 * Continuous full-res composite (Task: Full-res composite).
 * ------------------------------------------------------------------- */
struct frlv_frame { unsigned seq,done_ms,merge_us,changed_pct,offset_after,state,arm_after_vsync_us,cb_kind,cb_after_arm_us,cb_offset,engio_delta,adtg_delta,lost_bands,busy_vsync,busy_idle,ch5_state,ch5_addr; };
static struct frlv_frame cont_frames[FRLV_CONT_MAX];
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
/* parts > 1 renders only strip `part` of `parts` (LCM27 rolling preview). */
static __attribute__((optimize("O2"))) void preview_render(const struct frlv_result *r,const uint8_t *acc,unsigned part,unsigned parts)
{
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
    int n=snprintf(t,sizeof(t),"seq,done_ms,merge_us,changed_pct,dma_offset_after_merge,partial_bytes_plus1,arm_after_vsync_us,cb_kind,cb_after_arm_us,cb_offset,engio_calls,adtg_calls,lost_bands,busy_after_vsync,busy_idle,ch5_state,ch5_addr\n");
    FIO_WriteFile(f,t,n);
    for (unsigned i=0;i<r->merged && i<FRLV_CONT_MAX;i++) {
        const struct frlv_frame *c=&cont_frames[i];
        n=snprintf(t,sizeof(t),"%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%x,%x,%x,%x\n",c->seq,c->done_ms,c->merge_us,c->changed_pct,c->offset_after,c->state,
            c->arm_after_vsync_us,c->cb_kind,c->cb_after_arm_us,c->cb_offset,c->engio_delta,c->adtg_delta,c->lost_bands,c->busy_vsync,c->busy_idle,c->ch5_state,c->ch5_addr);
        FIO_WriteFile(f,t,n);
    }
    FIO_CloseFile(f);
}
/* Every completed frame is merged; a frame that cannot be read before the
 * next readout begins stops the session (never a silent gap). */
/* Canon resource registry snapshot (read-only RAM walk, no lock taken):
 * busy mask over Canon's ten engine resources, bit0 = write resource 5. */
static unsigned resource_busy(void)
{
    struct lc_resource_snapshot snap;
    if (!lc_native_resources(&snap)) return 0xFFFFu;
    return snap.busy|(snap.count<<12);
}
/* One-shot hardware max test on real data (LCM20). After the CPU merge,
 * acc = max(previous composite, frame) >= frame everywhere, so Canon's engine
 * computing max(acc, frame) into the capture buffer must reproduce acc
 * exactly. The composite itself is never written by the engine. */
static uint32_t *hw_geometry;
/* One engine max job on real data: out = max(acc, cap) written into cap.
 * After a CPU merge acc >= cap everywhere, so the correct output is acc.
 * The output area is cleared first, so an attempt that writes nothing (or a
 * stale earlier result) cannot pass. bands 1 = top half only. */
static unsigned hw_test(struct frlv_hw *h,struct frlv_result *r,uint8_t *cap,uint8_t *acc,unsigned frame,
    uint8_t *cap_storage,uint8_t *acc_storage,unsigned channel,unsigned bands,unsigned where)
{
    /* where 2/3/4: borrow + register group A/AB/ABC; 5: borrow only (LCM36) */
    const unsigned borrow=where>=2 && where<=5,npause=(where>=2 && where<=4) ? pause_group_n[where-2]:0;
    struct frlv_try scratch,*t=ntries<FRLV_TRIES ? &tries[ntries++]:&scratch;
    memset(t,0,sizeof(*t));t->where=where;t->frame=frame;t->channel=channel;t->bands=bands;t->safe=1;
    h->tried=1;h->frame=frame;h->safe=1;h->busy=t->busy=resource_busy();
    /* The nine non-output engine resources must be free; channel 5 also
     * needs Canon's resource 5 (held by Live View while it runs). */
    if (h->busy==0xFFFFu || (h->busy&0x3FE) || (channel==5 && !borrow && (h->busy&1))) { h->channel=0;t->reason=0xFFFF;return 0; }
    h->channel=channel;
    lc_native_set_wait_ms(600);
    lc_native_set_divert(1);
    /* LCM28's zero fill is gone: Canon's engine maps an all-zero input to 0
     * (values far below black are not plain integers to it). A channel that
     * writes nothing leaves the frame in cap, which differs from acc. */
    unsigned verify_end=0;
    for (unsigned band=0;band<bands;band++) {
        unsigned first,rows=lc_frlv_band(r->height,band,&first);
        verify_end=(first+rows)*r->pitch;
    }
    sync_caches();                                   /* acc written through the cache */
    const unsigned slot=nborrows<6 ? nborrows:5;
    struct lc_native_borrow *bw=&borrows[slot];
    if (borrow) {
        nborrows++;
        unsigned e=lc_native_borrow5(bw);
        if (e) { t->reason=0x10000u|e;t->safe=h->safe=e!=LC_NATIVE_STOP;return e==LC_NATIVE_STOP; }
        for (unsigned i=0;i<npause;i++) pause_saved[i]=shamem_read(lc_pause_regs[i].reg);
        for (unsigned i=0;i<npause;i++)
            if (lc_native_engdrv(&lc_pause_regs[i].reg,&lc_pause_regs[i].paused,1)) pause_write_err[slot]++;
    }
    for (unsigned band=0;band<bands;band++) {
        unsigned first,rows=lc_frlv_band(r->height,band,&first);
        struct lc_native_plane a,b;
        lc_frlv_plane((uint32_t)acc_storage,(uint32_t)acc|0x40000000u,r->pitch,first,rows,&a);
        lc_frlv_plane((uint32_t)cap_storage,(uint32_t)cap|0x40000000u,r->pitch,first,rows,&b);
        h->plane_ok[band]=lc_native_plane_ok(&a,1)|(lc_native_plane_ok(&b,1)<<1)|(lc_native_plane_ok(&b,0)<<2);
        struct lc_native_engine_result res;
        rec_n=0;rec_t0=(unsigned)get_us_clock();rec_on=1;
        unsigned reason=lc_native_engine_max_lock(&a,&b,&b,&res,channel,borrow ? 3:channel);
        rec_on=0;
        h->reason[band]=reason;h->engine_us[band]=res.elapsed_us;h->lock[band]=res.lock;h->setup[band]=res.setup;
        h->wait[band]=res.wait;memcpy(h->completed[band],res.completed,sizeof(res.completed));
        memcpy(h->after[band],res.after_wait,sizeof(res.after_wait));
        h->route[band]=res;
        t->reason|=reason;t->us+=res.elapsed_us;t->completed+=res.completed[2];t->aborted|=res.aborted;
        t->out_state=res.after_wait[8];t->out_bytes+=res.after_wait[11]-res.after_wait[10];
        for (unsigned i=0;i<LC_NATIVE_WRITE_CHANNELS;i++)
            if (res.all_ptr0[i]!=res.all_ptr1[i] && lc_native_write_channels[i]!=channel) t->moved|=1u<<i;
        if (!res.resume_safe) { h->safe=0;t->safe=0;return 1; }
        if (reason) break;
    }
    /* Restore Live View's values (reverse order), check them, give
     * channel 5 back - all before anything slow. */
    if (borrow) {
        for (unsigned i=npause;i-->0;) lc_native_engdrv(&lc_pause_regs[i].reg,&pause_saved[i],1);
        for (unsigned i=0;i<npause;i++) if (shamem_read(lc_pause_regs[i].reg)!=pause_saved[i]) pause_bad[slot]++;
        if (lc_native_return5(bw)) { t->reason|=0x20000u;h->safe=t->safe=0;return 1; }
        if (pause_bad[slot]) t->reason|=0x80000u;
    }
    if (t->reason) return 0;
    /* Word compare (LCM27 byte loop: 3.7 s per frame). */
    sync_caches();
    unsigned c0=(unsigned)get_us_clock();
    const uint32_t *x=(const uint32_t *)(const void *)physical(cap),*y=(const uint32_t *)(const void *)acc;
    h->first_bad=t->first_bad=~0u;
    for (unsigned i=0;i<verify_end/4;i++) if (x[i]!=y[i]) {
        if (!t->mismatch_words) { t->first_bad=h->first_bad=4*i;h->expected=y[i];h->actual=x[i]; }
        t->mismatch_words++;
    }
    h->mismatch_bytes=t->mismatch_words;
    h->compare_us=(unsigned)get_us_clock()-c0;
    return 0;
}

/* LCM27: the only verified engine output (LCM1) ran with Live View stopped,
 * every attempt in Live View wrote nothing. After the session (zoom and
 * hooks restored, composite still unsaved and untouched: the engine only
 * reads the accumulator) pause Live View, run the same max job on the real
 * buffers - on Canon's native channel 5 if resource 5 is now free - verify
 * it byte for byte, resume Live View. Returns nonzero if DMA is uncertain. */
static unsigned paused_hw_test(struct frlv_result *r,uint8_t *cap,uint8_t *acc,uint8_t *cap_storage,uint8_t *acc_storage)
{
    struct frlv_hw *h=&hw_paused;
    memset(h,0,sizeof(*h));
    if (!hw_geometry) hw_geometry=fio_malloc(128);
    if (!hw_geometry) { h->prepare=999;return 0; }
    h->prepare=lc_native_engine_prepare((uint32_t *)(((unsigned)hw_geometry+63)&~63u));
    if (h->prepare) return 0;
    NotifyBox(4000,"HW test: pausing Live View...");
    h->lv_before=lv && !LV_PAUSED;
    unsigned p0=get_ms_clock();
    PauseLiveView();
    for (unsigned i=0;i<40 && !LV_PAUSED;i++) msleep(50);
    pause_ms=get_ms_clock()-p0;
    msleep(500);                                     /* let the LV pipeline drain */
    h->paused=LV_PAUSED ? 1:0;
    unsigned unsafe=0;
    /* Channel 5 (both halves, Canon's route) as the reference, then the
     * replacement channels on the top half: do they work without Live View? */
    /* LCM28: channels 3/4/0 never receive the engine output (paused
     * channel 3 even refused to abort); only the channel 5 reference. */
    if (h->paused) {
        if (snap_lv_ok) { snap_diff();snap_paused_ok=1; }
        unsigned before=ntries;
        unsafe=hw_test(h,r,cap,acc,r->merged,cap_storage,acc_storage,5,2,1);
        job_pairs=lc_native_job_list(job_list,64);
        /* LCM34 reverse test: with the reference exact, give one register
         * group its Live View value (frame-3 snapshot), run the same job,
         * put the paused values back. A group that breaks the job is what
         * blocks the engine in Live View. where 10/11/12 = A/B/C. */
        const unsigned ref_ok=ntries>before && !tries[before].reason && !tries[before].mismatch_words;
        for (unsigned g=3;g<5 && ref_ok && snap_lv_ok && !unsafe && FRLV_LV_STEPS;g++) {
            static uint32_t keep[LC_PAUSE_ABC],keep05[64],reg05[64];
            const unsigned lo=group_lo[g<4 ? g:3],hi=group_hi[g<4 ? g:3];
            unsigned n05=0;
            for (unsigned i=lo;i<hi;i++) {
                const uint32_t reg=lc_pause_regs[i].reg,lvv=snap_lv[(reg-0xC0F00000u)/4];
                keep[i]=shamem_read(reg);
                if (lc_native_engdrv(&reg,&lvv,1)) group_write_err[g]++;
            }
            if (g==4) for (unsigned k=0;k<diff_n && n05<64;k++) if ((diff_reg[k]>>12)==0xC0F05u) {
                /* EngDrvOut directly: lc_native_engdrv refuses EDMAC pages. */
                reg05[n05]=diff_reg[k];keep05[n05]=shamem_read(diff_reg[k]);
                ((void (*)(uint32_t,uint32_t))0xff2c1694)(diff_reg[k],diff_lv[k]);n05++;
            }
            static struct frlv_hw other;
            unsafe=hw_test(&other,r,cap,acc,r->merged,cap_storage,acc_storage,5,2,10+g);
            for (unsigned k=n05;k-->0;) ((void (*)(uint32_t,uint32_t))0xff2c1694)(reg05[k],keep05[k]);
            for (unsigned i=hi;i-->lo;) lc_native_engdrv(&lc_pause_regs[i].reg,&keep[i],1);
            group_write_err[g]|=n05<<16;
        }
    }
    p0=get_ms_clock();
    ResumeLiveView();
    for (unsigned i=0;i<60 && (!lv || LV_PAUSED);i++) msleep(50);
    resume_ms=get_ms_clock()-p0;
    h->lv_after=lv && !LV_PAUSED;
    h->close=lc_native_engine_close();
    return unsafe || h->close;
}

static void continuous_capture(struct frlv_result *r,uint8_t *cap,uint8_t *acc,
    const volatile unsigned *cancel,const char **msg,unsigned *dma_uncertain,unsigned *hw_uncertain,unsigned period_ms,
    unsigned hw_enable,uint8_t *cap_storage,uint8_t *acc_storage)
{
    preview_setup();r->preview_ok=preview_yuv!=NULL;
    const unsigned end=physical(cap)+r->bytes,pairs=r->bytes/28,max_partials=2+r->limit/4;
    static char line[64];
    unsigned last_engio=stats.engio_calls,last_adtg=stats.adtg_calls;
    memset(&hw,0,sizeof(hw));act_on=0;act_n=0;ntries=0;memset(tries,0,sizeof(tries));
    snap_lv_ok=snap_paused_ok=job_pairs=0;
    nborrows=0;memset(borrows,0,sizeof(borrows));memset(group_write_err,0,sizeof(group_write_err));pause_ms=resume_ms=0;memset(pause_bad,0,sizeof(pause_bad));memset(pause_write_err,0,sizeof(pause_write_err));
    unsigned lv_tries=0,lv_step=0,lv_done=0;disp_off_ms=disp_try_frame=0;
    if (hw_enable) {
        if (!snap_lv) snap_lv=malloc(FRLV_SHADOW_WORDS*4);
        hw_geometry=fio_malloc(128);
        hw.prepare=hw_geometry ? lc_native_engine_prepare((uint32_t *)(((unsigned)hw_geometry+63)&~63u)):999;
        if (hw.prepare) hw_enable=0;
    }
    continuous=1;cont_arm=1;
    /* First arm: just after a vsync, never inside it. */
    unsigned seen=frames_done,v=vsyncs,w0=get_ms_clock();
    while (vsyncs==v && get_ms_clock()-w0<period_ms+3000) msleep(10);
    msleep(40);arm_from_task();
    *msg="Full-res composite: first frame...";NotifyBox(8000,"Full-res composite: first exposure");
    for (;;) {
        if (*cancel || ml_shutdown_requested || !zoom5() || raw_lv_is_enabled() || r->merged>=r->limit) {
            if (cont_arm && *cancel) NotifyBox(5000,"Stopping: finishing current exposure");
            cont_arm=0;
        }
        w0=get_ms_clock();
        while (frames_done==seen && in_flight && get_ms_clock()-w0<period_ms+3000) {
            act_sample();
            if (*cancel && cont_arm) { cont_arm=0;NotifyBox(5000,"Stopping: finishing current exposure"); }
            if (ml_shutdown_requested) cont_arm=0;
            msleep(10);
        }
        if (frames_done==seen) {
            if (!in_flight) break;                       /* clean stop: nothing armed */
            r->error|=FE_STALL;*msg="LV frame missing; see LOG";break;
        }
        /* LCM26: a frame that arrived while we were still busy is skipped
         * (a gap of one sub-exposure), not fatal. The capture buffer holds
         * the latest complete frame. Stop only if it keeps happening. */
        if (frames_done!=seen+1) {
            r->missed+=frames_done-seen-1;
            if (r->missed>2+r->limit/4) { r->error|=FE_DMA;*msg="Merge too slow: frames missed";cont_arm=0;break; }
        }
        seen=frames_done;
        unsigned state=done_state,pointer=done_pointer,done=done_ms;
        if (act_on) act_on=0;                            /* frame 3 arrived: map complete */
        if (r->merged==1 && !act_n) { act_union=0;act_t0=done;act_on=1;act_config(5,act_info5);act_config(3,act_info3);act_routes(); }
        unsigned d_kind=done_cb_kind,d_after=done_cb_after,d_ptr=done_cb_ptr,d_arm=done_arm_after_vsync,d_engio=done_engio,d_adtg=done_adtg;
        preview_show();                                  /* hide Canon's new frame */
        if (state) { cont_arm=0;*dma_uncertain=1;r->error|=FE_DMA;*msg="RAW DMA did not finish";break; }
        if (!guards_ok(cap,r->bytes)) { cont_arm=0;*dma_uncertain=1;r->error|=FE_GUARD;*msg="Buffer guard overwritten";break; }
        if (r->merged>=r->limit) continue;               /* past the limit: not merged */
        /* Next frame: arm now (its readout starts ~period-readout later),
         * never inside the vsync: preview_show (~110 ms) already waited. */
        if (!preview_yuv) msleep(40);
        if (cont_arm) arm_from_task();
        struct frlv_frame *c=&cont_frames[r->merged];
        memset(c,0,sizeof(*c));                         /* static: reused across sessions */
        c->seq=seen;c->done_ms=done;c->state=pointer==end ? 0:pointer-physical(cap)+1;
        c->arm_after_vsync_us=d_arm;c->cb_kind=d_kind;c->cb_after_arm_us=d_after;c->cb_offset=d_ptr ? d_ptr-physical(cap):0;
        c->engio_delta=d_engio-last_engio;c->adtg_delta=d_adtg-last_adtg;last_engio=d_engio;last_adtg=d_adtg;
        c->busy_vsync=resource_busy();
        /* Live View's channel 5 after each frame (diagnostic, LCM29) */
        c->ch5_state=edmac_get_state(5);c->ch5_addr=edmac_get_address(5);
        if (pointer!=end) {
            /* Partial frame: the rows that arrived are real exposure, the rest
             * is the previous (already merged) frame. Merge it and log it. */
            r->partials++;
            if (r->partials>max_partials) { cont_arm=0;r->error|=FE_DMA;*msg="Too many partial frames; see CSV";break; }
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
            act_sample();
            msleep(10);
            /* Acknowledge Q/SET during the ~1.7 s merge too (LCM25). */
            if (*cancel && cont_arm) { cont_arm=0;NotifyBox(5000,"Stopping: finishing current exposure"); }
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
        c->busy_idle=resource_busy();
        /* LCM30: no engine job in Live View (LCM29: even a borrowed channel 5
         * gets no output there). Snapshot Canon's register shadow instead. */
        if (hw_enable && snap_lv && !snap_lv_ok && r->merged>=3) { snap_take(snap_lv);snap_lv_ok=1; }
        /* LCM33: escalating Live View attempts A, A+B, A+B+C on frames
         * where channel 5 can be borrowed; stop at the first exact one. */
        if (hw_enable && !disp_off_ms && r->merged==3) { display_off();disp_off_ms=get_ms_clock()-done; }
        if (hw_enable && disp_off_ms && !lv_done && lv_tries<2 && (r->merged==4 || r->merged==6) && !off && !c->lost_bands &&
            get_ms_clock()-done+900<period_ms-600) {
            lv_tries++;
            unsigned before=ntries;
            unsigned bad=hw_test(&hw,r,cap,acc,r->merged,cap_storage,acc_storage,5,2,5);
            if (ntries>before && !(tries[ntries-1].reason&0x10000u)) { lv_done=1;disp_try_frame=r->merged; }
            if (lv_done || lv_tries>=2 || bad) display_on();
            if (bad) { *hw_uncertain=1;cont_arm=0;r->error|=FE_DMA;*msg="HW merge test: DMA uncertain; reboot";break; }
            if (lv_done && !tries[ntries-1].reason && !tries[ntries-1].mismatch_words)
                NotifyBox(8000,"HW merge WORKS with the display off!");
            write_log(r,"hw merge tested");
            continue;
        }
        if (FRLV_LV_STEPS && hw_enable && !lv_done && lv_tries<6 && r->merged>=3 && !off && !c->lost_bands &&
            get_ms_clock()-done+900<period_ms-600) {
            NotifyBox(2000,"HW merge test, step %c...",'A'+lv_step);
            lv_tries++;
            unsigned before=ntries;
            if (hw_test(&hw,r,cap,acc,r->merged,cap_storage,acc_storage,5,2,2+lv_step)) {
                *hw_uncertain=1;cont_arm=0;r->error|=FE_DMA;*msg="HW merge test: DMA uncertain; reboot";break;
            }
            if (ntries>before) {
                const struct frlv_try *q=&tries[ntries-1];
                if (!q->reason && !q->mismatch_words) { lv_done=1;NotifyBox(5000,"HW merge WORKS in Live View (step %c)",'A'+lv_step); }
                else if (!(q->reason&0x10000u)) { if (++lv_step>2) lv_done=1; }   /* refused borrow: retry same step */
            }
            write_log(r,"hw merge tested");
            continue;
        }
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
            snprintf(line,sizeof(line),"Stopping: %d frames, last exposure...",r->merged);*msg=line;
            NotifyBox(5000,"%s",line);
        } else {
            snprintf(line,sizeof(line),"Full-res composite: %d frames",r->merged);*msg=line;
            NotifyBox(1500,"%s",line);
        }
        if (r->merged%10==1) write_log(r,"composing");
    }
    continuous=0;cont_arm=0;
    if (disp_off_ms) display_on();
    if (preview_yuv) { fio_free(preview_yuv);preview_yuv=NULL; }
    if (hw_enable) { hw.close=lc_native_engine_close();if (hw.close) *hw_uncertain=1; }
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
    r.continuous=o->continuous;r.limit=MIN(MAX(o->limit,1u),FRLV_CONT_MAX);
    r.wb_mode=lens_info.wb_mode;r.kelvin=lens_info.kelvin;
    lc_wb_neutral_for(lens_info.wb_mode,lens_info.kelvin,lens_info.WBGain_R,lens_info.WBGain_G,lens_info.WBGain_B,r.neutral);
    void *alloc[2]={0};uint8_t *cap=NULL,*acc=NULL;
    unsigned hooked[4]={0},power=0,locked=0,old_lock=0,zoomed=0,dma_uncertain=0,hw_uncertain=0;
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
    /* Halo rows before the pixels (Canon engine input), then guards. */
    cap=(uint8_t *)lc_frlv_pixels((uint32_t)alloc[0],1);
    if (alloc[1]) acc=(uint8_t *)physical((void *)lc_frlv_pixels((uint32_t)alloc[1],0));
    /* halo before and after (engine input bands), guards around */
    unsigned capacity=FRLV_ALLOCATION-2*FRLV_HALO-3*FRLV_GUARD;
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
    msg=o->continuous ? "Full-res composite: starting (about 10 s)":"Full-res readout: switching sensor";*status=msg;
    if (o->continuous) NotifyBox(8000,"%s",msg);
    unsigned t0=get_ms_clock();
    set_lv_zoom(5);zoomed=1;
    while (!(lv_dispsize==5 && zoom5())) {
        if (*cancel || ml_shutdown_requested || get_ms_clock()-t0>6000) { r.error|=FE_ZOOM;break; }
        msleep(50);
    }
    r.zoom_ms=get_ms_clock()-t0;r.zoom_done_ms=get_ms_clock();
    if (r.error) { msg="x5 zoom did not engage";goto restore; }
    if (!wait_vsyncs(o->continuous ? 2:3,3*period_ms+4000,cancel)) { r.error|=*cancel ? FE_CANCEL:FE_STALL;msg="No LV frames after switch";goto restore; }
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
    /* Cadence over a few frames (timer B may need one frame to settle).
     * The composite skips this: at 4 s per frame it cost ~12 s of startup. */
    if (!o->continuous) {
        unsigned frames=MAX(3u,cfg.timer_b<10000 ? 6u:3u);
        wait_vsyncs(1,period_ms+4000,cancel);
        unsigned v0=vsyncs,m0=get_ms_clock();max_gap_ms=0;
        if (!wait_vsyncs(frames,frames*period_ms+4000,cancel)) { r.error|=*cancel ? FE_CANCEL:FE_STALL;msg="LV stalled at full res";goto restore; }
        r.cadence_frames=vsyncs-v0;r.cadence_ms=get_ms_clock()-m0;r.max_gap=max_gap_ms;
    }
    r.stage=5;write_log(&r,"verified");

    /* 6: capture inputs. One frame per request; merges happen between. */
    msg="Full-res readout: capturing";*status=msg;
    if (r.continuous) {
        if (!acc) { r.error|=FE_MEMORY;msg="Composite needs two full-size buffers";goto restore; }
        continuous_capture(&r,cap,acc,cancel,&msg,&dma_uncertain,&hw_uncertain,period_ms,o->hw_test,alloc[0],alloc[1]);
        if (r.merged && !(r.error&~FE_CANCEL)) r.stage=6;
        *status=msg;
        goto restore;
    }
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
    if (o->hw_test && r.continuous && r.merged>=2 && !r.unsafe && !hw_uncertain && alloc[0] && alloc[1] &&
        cap && acc && !ml_shutdown_requested)
        hw_uncertain|=paused_hw_test(&r,cap,acc,alloc[0],alloc[1]);
    /* HW-test uncertainty concerns only the capture buffer (engine output):
     * keep every buffer allocated, but the accumulator may still be saved. */
    const unsigned save_ok=!r.unsafe && (!hw_uncertain || (acc && cap && acc!=(uint8_t *)physical(cap)));
    if (hw_uncertain) r.unsafe=1;
    /* With a single allocation, the last captured frame is the output. */
    if (!acc && cap && !r.unsafe) acc=(uint8_t *)physical(cap);
    else if (!r.unsafe && alloc[0]) { fio_free(alloc[0]);alloc[0]=NULL;cap=NULL; }
    if (acc && r.merged && save_ok && !ml_shutdown_requested && get_ml_card()==card &&
        !(r.error&(FE_GUARD|FE_SIZE|FE_REGS)) && (r.continuous || !(r.error&FE_DMA))) {
        if (o->fix_focus_pixels && r.width==5280) {
            unsigned f0=(unsigned)get_us_clock();
            r.fp_fixed=lc_fix_focus_pixels(acc,r.width,r.height,r.pitch);
            r.fp_us=(unsigned)get_us_clock()-f0;
        }
        *status="Full-res readout: saving DNG";
        if (r.continuous) NotifyBox(15000,"Saving full-res composite DNG...");
        r.saved=frlv_save_dng(&r,acc);
        if (!r.saved) r.error|=FE_IO;
        else r.stage=8;
    }
    if (!r.unsafe) for (unsigned i=0;i<2;i++) if (alloc[i]) fio_free(alloc[i]);
    if (locked) gui_uilock(old_lock);
    if (power) powersave_permit();
    if (r.unsafe) msg=r.saved ? "Composite saved; HW test left DMA busy: reboot":"Full-res: ownership uncertain; keep LOG, reboot";
    else if (!r.error) msg=r.saved ? "Full-res DNG saved; see LVFR LOG":"Full-res: no frame saved";
    else if (r.saved) msg=r.continuous ? "Composite saved; stopped early, see LOG":"Full-res DNG saved with warnings; see LOG";
    if (r.continuous && r.saved && !r.error && !r.unsafe) {
        static char done[64];
        if (r.late_frames || r.missed) snprintf(done,sizeof(done),"Composite saved: %d frames, %d late, %d skipped",r.merged,r.late_frames,r.missed);
        else snprintf(done,sizeof(done),"Full-res composite saved: %d frames",r.merged);
        msg=done;
    }
    *status=msg;
    if (r.log[0]) write_log(&r,msg);
    if (r.log[0] && r.continuous) write_csv(&r);
    return r.unsafe;
}
