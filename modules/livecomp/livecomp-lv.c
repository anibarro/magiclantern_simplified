/* GPL-2.0-or-later. LCM10 continuous LiveView RAW module, AI-assisted.
 * Uses the repository's silent/mlv_lite capture API. No sensor register patch,
 * factory job, native max engine or native JPEG decoder is used in this mode.
 * See LV_CAMERA_TEST.md: consecutive buffers do not prove zero sensor dead time.
 */
#include "dryos.h"
#include "module.h"
#include "menu.h"
#include "lens.h"
#include "raw.h"
#include "fps.h"
#include "edmac.h"
#include "mem.h"
#include "vram.h"
#include "zebra.h"
#include "gui-common.h"
#include "propvalues.h"
#include "powersave.h"
#include "notify_box.h"
#include "lv-dng.h"
#include "livecomp-lv.h"
#include "camera-profile.h"
#include "config.h"
#include "lv-ring.h"
#include "raw14-max.h"
#include "fullres-check.h"
#include "fullres-lv.h"

extern uint32_t raw_write_chan;
extern int raw_lv_is_enabled(void);
/* Exported by the existing EOS M core. Called only from its EVF callback. */
extern void fps_update_timers_from_evfstate(void);
#define LV_FRAME_CAP 120u
#define GUARD 64u
enum { E_SETTINGS=1,E_MEMORY=2,E_DMA=4,E_GUARD=8,E_QUEUE=16,
       E_TIMEOUT=32,E_IO=64,E_ORDER=128,E_SHUTDOWN=256 };
struct lv_frame_log { uint32_t sequence,slot,start_ms,done_ms,merge_us,dma_state,dma_pointer; };
/* Keep the order aligned with settings_valid. Values are sampled once per check;
 * no filesystem work or formatting is performed inside the frame callback. */
static const char *const setting_names[]={
    "lv","paused","movie","zoom","recording","job","raw_channel","raw_valid",
    "bits","width","height","pitch","bytes","iso","aperture","shutter",
    "wb","kelvin","gain_r","gain_b","wb_gm","wb_ba","frame_iso","fps","tvr"
};
struct lv_settings_failure {
    uint32_t phase,mask,ms,vsync,issued;
    int expected[COUNT(setting_names)],observed[COUNT(setting_names)];
};
struct lv_session {
    struct raw_info info;
    struct lc_ring ring;
    struct lv_frame_log frames[LV_FRAME_CAP];
    void *allocation[LC_LV_SLOTS];
    uint8_t *input[LC_LV_SLOTS],*accumulator,*preview;
    struct card_info *card;
    int iso,aperture,shutter,fps,tvr,frame_iso;
    unsigned wb,kelvin,gain_r,gain_b;
    int wb_gm,wb_ba;
    uint32_t bytes,merged,limit,preview_interval;
    volatile uint32_t vsyncs,last_vsync,error,quarantined;
    uint32_t stop_kind,stop_issued,preview_frames,preview_calls,preview_last_ms;
    uint32_t start_ms,end_ms,saved;
    uint32_t timing_syncs,preview_rendered;
    uint32_t preview_snapshot_ms,save_stage,save_written,save_size;
    int save_last_return;
    int timing_before_fps,timing_before_tvr,timing_after_fps,timing_after_tvr;
    void *preview_destination;
    struct lv_settings_failure settings_failure;
    char path[FIO_MAX_PATH_LENGTH];
};
static struct lv_session *session;
static volatile unsigned requested,attempted,running,armed,stop_requested,display_setup;
static volatile unsigned preview_enabled,preview_writing,preview_reading,preview_ready;
static volatile unsigned capture_reading;
static unsigned menu_added;
static CONFIG_INT("livecomp.enabled",enabled,0);
static CONFIG_INT("livecomp.limit",frame_limit,2);
static CONFIG_INT("livecomp.preview",preview_interval,0);
/* Task selection is deliberately not persisted across boots:
 * 0=Live composite, 1=Full-res check (native engine), 2=Full-res readout. */
static int fullres_check;
static int frlv_fps,frlv_expo,frlv_inputs=1;
static const unsigned frlv_input_counts[]={1,4,8};
static unsigned trigger_ms;
static const unsigned frame_limits[]={30,60,120};
static const unsigned preview_intervals[]={1000,2000,4000};
static const char *status="Movie M, normal view, 3 fps, slow shutter";

int livecomp_lv_busy(void) { return requested || attempted; }
static void stop(unsigned kind)
{
    unsigned irq=cli();
    if (!stop_requested) {
        if (session) { session->stop_kind=kind;session->stop_issued=session->ring.issued; }
        stop_requested=1;
    }
    sei(irq);
}
void livecomp_lv_shutdown(void) { stop(3); }
/* Only an intentional Q/SET in idle Movie Live View starts capture. Menu,
 * playback, recording and normal shutter operation are never start triggers. */
static int trigger_ready(void)
{
    return enabled && !attempted && !running && !requested &&
        !ml_shutdown_requested && !gui_menu_shown() && gui_state==livecomp_camera.gui_idle &&
        lv && !LV_PAUSED && is_movie_mode() && lv_dispsize==1 &&
        !RECORDING && !lens_info.job_state && !get_halfshutter_pressed();
}
int livecomp_lv_key(unsigned code)
{
    int trigger=code==MODULE_KEY_PRESS_SET || code==MODULE_KEY_Q;
    if (!running) {
        if (trigger && trigger_ready()) {
            requested=1;trigger_ms=get_ms_clock();
            status="Starting composite; Q/SET stops and saves";
            return 0;
        }
        if (requested && (trigger || code==MODULE_KEY_UNPRESS_SET)) return 0;
        return 1;
    }
    /* A single physical Q/SET may produce multiple GUI events. Do not turn
     * the initiating press into an immediate Stop. */
    if (trigger) {
        if (get_ms_clock()-trigger_ms>=500) stop(1);
        return 0;
    }
    if (display_setup && code==MODULE_KEY_INFO) return 1;
    /* Unmapped/internal events must continue to the core. */
    return code==0 || (int)code<0;
}
static unsigned physical(const void *p) { return (unsigned)p&~livecomp_camera.cache_alias_mask; }
static int prepare_display(void)
{
    /* Core display filters require clean Canon LV, even when Global Draw is on.
     * Use the same INFO mechanism as the core's display-presets task. */
    if (!(get_global_draw_setting()&1)) return 0;
    display_setup=1;
    for (unsigned i=0;i<10 && !stop_requested && !ml_shutdown_requested;i++) {
        if (get_global_draw()) { display_setup=0;return 1; }
        if (!lv || LV_PAUSED) break;
        if (lv_disp_mode && i<4) module_send_keypress(MODULE_KEY_INFO);
        msleep(200);
    }
    display_setup=0;return 0;
}
static int geometry_valid(const struct raw_info *r)
{
    return r->bits_per_pixel==14 && r->width>=640 && r->width<=2048 &&
        !(r->width%8) && r->height>=400 && r->height<=1200 &&
        r->pitch==r->width*14/8 && r->frame_size==r->pitch*r->height &&
        r->active_area.x1>=0 && r->active_area.y1>=0 &&
        r->active_area.x2>r->active_area.x1 && r->active_area.x2<=r->width &&
        r->active_area.y2>r->active_area.y1 && r->active_area.y2<=r->height;
}
/* Canon restores its default timer B before CBR_VSYNC; the core normally
 * reapplies ML FPS later in that same callback. LCM3a measured 29973/33336 here
 * versus 3000/3030 in the worker. Reapply the existing core override before
 * checking/admitting an input, without inventing or changing sensor settings.
 * The core's later identical write remains in place. Never call from preview. */
static void sync_capture_timing(struct lv_session *s)
{
    if (!lv || LV_PAUSED || !is_movie_mode() || lv_dispsize!=1 || RECORDING ||
        lens_info.job_state || ml_shutdown_requested) return;
    if (!s->timing_syncs) {
        s->timing_before_fps=fps_get_current_x1000();
        s->timing_before_tvr=get_current_shutter_reciprocal_x1000();
    }
    fps_update_timers_from_evfstate();
    if (!s->timing_syncs) {
        s->timing_after_fps=fps_get_current_x1000();
        s->timing_after_tvr=get_current_shutter_reciprocal_x1000();
    }
    s->timing_syncs++;
}
/* phase: 0=preview (no failure latch), 1=worker before arm, 2=frame callback.
 * Exact effective FPS/shutter comparisons belong only to phase 2, after the
 * override is reapplied. Asynchronous worker/display reads can see Canon's
 * temporary values. Geometry and user exposure/WB stay checked in every phase. */
static int settings_valid(struct lv_session *s,unsigned phase)
{
    /* -1 in hardware-derived diagnostic fields means not sampled because the
     * LV stream is unavailable. Avoid hardware queries after a mode change. */
    int mode_ok=lv && !LV_PAUSED && is_movie_mode() && lv_dispsize==1 &&
        !RECORDING && !lens_info.job_state && raw_write_chan==livecomp_camera.raw_channel;
    int stream_ok=mode_ok && raw_lv_settings_still_valid();
    const int expected[]={1,0,1,1,0,0,livecomp_camera.raw_channel,1,14,
        s->info.width,s->info.height,s->info.pitch,s->info.frame_size,
        s->iso,s->aperture,s->shutter,s->wb,s->kelvin,s->gain_r,s->gain_b,
        s->wb_gm,s->wb_ba,s->frame_iso,s->fps,s->tvr};
    const int observed[]={!!lv,!!LV_PAUSED,!!is_movie_mode(),lv_dispsize,
        !!RECORDING,!!lens_info.job_state,raw_write_chan,mode_ok ? stream_ok:-1,
        raw_info.bits_per_pixel,raw_info.width,raw_info.height,raw_info.pitch,raw_info.frame_size,
        lens_info.raw_iso,lens_info.raw_aperture,lens_info.raw_shutter,lens_info.wb_mode,
        lens_info.kelvin,lens_info.WBGain_R,lens_info.WBGain_B,lens_info.wbs_gm,
        lens_info.wbs_ba,stream_ok ? get_frame_iso():-1,
        phase==2 ? (stream_ok ? fps_get_current_x1000():-1):s->fps,
        phase==2 ? (stream_ok ? get_current_shutter_reciprocal_x1000():-1):s->tvr};
    _Static_assert(COUNT(expected)==COUNT(setting_names) &&
                   COUNT(observed)==COUNT(setting_names) && COUNT(setting_names)<=32,
                   "settings diagnostic layout");
    uint32_t mask=0;
    for (unsigned i=0;i<COUNT(expected);i++)
        if (observed[i]!=expected[i]) mask|=1u<<i;
    if (mask && phase) {
        unsigned irq=cli();
        struct lv_settings_failure *f=&s->settings_failure;
        if (!f->phase) {
            f->mask=mask;f->ms=get_ms_clock();f->vsync=s->vsyncs;f->issued=s->ring.issued;
            memcpy(f->expected,expected,sizeof(expected));
            memcpy(f->observed,observed,sizeof(observed));
            f->phase=phase;
        }
        sei(irq);
    }
    return !mask;
}
static int guards_valid(struct lv_session *s,unsigned slot)
{
    const uint8_t *p=s->input[slot];
    for (unsigned i=0;i<GUARD;i++)
        if (p[(int)i-(int)GUARD]!=0xa5 || p[s->bytes+i]!=0xa5) return 0;
    return 1;
}
static int tail_written(struct lv_session *s,unsigned slot)
{
    const uint8_t *p=s->input[slot]+s->bytes-8;
    for (unsigned i=0;i<8;i++) if (p[i]!=0xa5) return 1;
    return 0;
}

/* Runs before core raw_lv_vsync starts the next slurp. All operations are
 * bounded bookkeeping. Worker never consumes the still-capturing slot. */
static unsigned capture_vsync(unsigned unused)
{
    (void)unused;
    if (!armed || !session) return CBR_RET_CONTINUE;
    struct lv_session *s=session;
    s->last_vsync=get_ms_clock();s->vsyncs++;
    int prior=s->ring.pending;
    if (prior>=0) {
        struct lv_frame_log *f=&s->frames[s->ring.sequence[prior]-1];
        f->done_ms=s->last_vsync;f->dma_state=edmac_get_state(raw_write_chan);
        f->dma_pointer=edmac_get_pointer(raw_write_chan);
        unsigned lo=physical(s->input[prior]);
        /* Like mlv_lite's sentinel checks, plus channel-idle and ownership.
         * Pointer is diagnostic: accept either start or advanced in-frame/end
         * position, never a different allocation. All CPU reads occur later. */
        if (f->dma_state || f->dma_pointer<lo || f->dma_pointer>lo+s->bytes ||
            !tail_written(s,prior)) { s->error|=E_DMA;s->quarantined=1; }
        if (!guards_valid(s,prior)) { s->error|=E_GUARD;s->quarantined=1; }
        if (s->error) { armed=0;stop(4);return CBR_RET_CONTINUE; }
        unsigned irq=cli();lc_ring_complete(&s->ring);sei(irq);
    }
    if (ml_shutdown_requested) { s->error|=E_SHUTDOWN;stop(3); }
    if (stop_requested) { armed=0;return CBR_RET_CONTINUE; }
    sync_capture_timing(s);
    if (!settings_valid(s,2)) { s->error|=E_SETTINGS;armed=0;stop(4);return CBR_RET_CONTINUE; }
    if (s->ring.issued>=s->limit) { armed=0;stop(2);return CBR_RET_CONTINUE; }
    /* Serialize the final Stop check and admission with the GUI callback.
     * A Stop arriving during the settings checks must not admit a new frame. */
    unsigned irq=cli();
    if (stop_requested || !armed) { armed=0;sei(irq);return CBR_RET_CONTINUE; }
    int slot=lc_ring_arm(&s->ring);
    if (slot<0) {
        /* Stop at the first capacity miss; do not silently drop an exposure
         * and later present a discontinuous sequence as continuous. */
        s->error|=E_QUEUE;armed=0;sei(irq);stop(4);return CBR_RET_CONTINUE;
    }
    struct lv_frame_log *f=&s->frames[s->ring.issued-1];
    f->sequence=s->ring.issued;f->slot=slot;f->start_ms=s->last_vsync;
    memset(s->input[slot]+s->bytes-8,0xa5,8);
    raw_lv_redirect_edmac(s->input[slot]);
    sei(irq);
    return CBR_RET_CONTINUE;
}

/* Hold a session reference until the whole frame callback has returned.
 * Cleanup disables admission, then waits for this reference before freeing. */
unsigned livecomp_lv_vsync(unsigned context)
{
    livecomp_fullres_vsync();
    livecomp_frlv_vsync();
    unsigned irq=cli();
    if (!armed || !session) { sei(irq);return CBR_RET_CONTINUE; }
    capture_reading++;
    sei(irq);
    unsigned result=capture_vsync(context);
    irq=cli();capture_reading--;sei(irq);
    return result;
}

/* Snapshot ownership is separate from capture/merge. If the display is busy,
 * skip a preview refresh, never a RAW input. No sensor pause for preview. */
static void update_preview(struct lv_session *s,unsigned force)
{
    unsigned irq=cli();
    if (!force && preview_ready && get_ms_clock()-s->preview_snapshot_ms<s->preview_interval) {
        sei(irq);return;
    }
    if (preview_reading) { sei(irq);return; }
    preview_writing=1;sei(irq);
    memcpy(s->preview,s->accumulator,s->bytes);
    irq=cli();preview_ready=s->merged;preview_writing=0;sei(irq);
    s->preview_frames++;
    s->preview_snapshot_ms=get_ms_clock();
}
unsigned livecomp_lv_preview(unsigned context)
{
    if (!context) return preview_enabled && preview_ready;
    unsigned irq=cli();
    if (!preview_enabled || !preview_ready || preview_writing || !session) {
        sei(irq);return CBR_RET_CONTINUE;
    }
    preview_reading=1;sei(irq);
    struct lv_session *s=session;
    struct display_filter_buffers *buffers=(void *)context;
    /* The snapshot is published at most once per second. Every RAW is merged;
     * avoid spending capture CPU on copies/renders of intermediate previews. */
    if (s->preview_rendered==preview_ready && s->preview_destination==buffers->dst_buf) {
        preview_reading=0;return CBR_RET_STOP;
    }
    if (settings_valid(s,0)) {
        raw_set_preview_rect(s->info.active_area.x1,s->info.active_area.y1,
            s->info.active_area.x2-s->info.active_area.x1,
            s->info.active_area.y2-s->info.active_area.y1,1);
        raw_preview_fast_ex(s->preview,buffers->dst_buf,-1,-1,RAW_PREVIEW_COLOR_HALFRES);
        sync_caches(); /* LCD consumes the core's uncached display-buffer alias. */
        s->preview_rendered=preview_ready;s->preview_destination=buffers->dst_buf;
        s->preview_calls++;s->preview_last_ms=get_ms_clock();
    }
    preview_reading=0;return CBR_RET_STOP;
}
static int choose_path(struct lv_session *s)
{
    const char *extensions[]={"DNG","LOG","CSV"};
    for (unsigned n=0;n<10000;n++) {
        int occupied=0;
        for (unsigned e=0;e<COUNT(extensions);e++) {
            uint32_t size;
            snprintf(s->path,sizeof(s->path),"%s:/LVCP%04d.%s",s->card->drive_letter,n,extensions[e]);
            if (!FIO_GetFileSize(s->path,&size)) { occupied=1;break; }
        }
        if (!occupied) { memcpy(s->path+strlen(s->path)-3,"DNG",3);return 1; }
    }
    return 0;
}
static int save_write_all(struct lv_session *s,FILE *file,const uint8_t *data,unsigned bytes)
{
    unsigned offset=0;
    while (offset<bytes && !ml_shutdown_requested && get_ml_card()==s->card) {
        int result=FIO_WriteFile(file,data+offset,bytes-offset);s->save_last_return=result;
        if (result<=0 || (unsigned)result>bytes-offset) return 0;
        offset+=result;s->save_written+=result;
    }
    return offset==bytes;
}
static int save_lv_dng(struct lv_session *s)
{
    struct lc_dng_metadata m={0};
    m.width=s->info.width;m.height=s->info.height;m.pitch=s->info.pitch;m.bytes=s->bytes;
    m.black=s->info.black_level;m.white=s->info.white_level;m.cfa=s->info.cfa_pattern;
    memcpy(m.active,s->info.dng_active_area,sizeof(m.active));
    memcpy(m.matrix,s->info.color_matrix1,sizeof(m.matrix));
    m.illuminant=s->info.calibration_illuminant1;
    m.iso=raw2iso(s->frame_iso ? s->frame_iso:s->iso);m.tvr=s->tvr;m.fps=s->fps;
    m.aperture=lens_info.aperture;m.focal=lens_info.focal_len;
    s->save_stage=1;
    uint8_t *scratch=fio_malloc(16384);
    if (!scratch) { s->save_stage=2;return 0; }
    if (!lc_dng_header(scratch,&m)) { fio_free(scratch);return 0; }
    s->save_stage=3;FILE *file=FIO_CreateFile(s->path);
    if (!file || file==(void *)-1) { fio_free(scratch);return 0; }
    s->save_stage=4;int ok=save_write_all(s,file,scratch,LC_DNG_HEADER_BYTES);
    if (ok) {
        s->save_stage=5;
        for (unsigned offset=0;offset<s->bytes && ok;offset+=16384) {
            unsigned bytes=MIN(16384u,s->bytes-offset);
            lc_dng_swap_copy(scratch,s->accumulator+offset,bytes);
            ok=save_write_all(s,file,scratch,bytes);
        }
    }
    FIO_CloseFile(file);fio_free(scratch);
    if (!ok) return 0;
    s->save_stage=6;
    if (FIO_GetFileSize(s->path,&s->save_size) ||
        s->save_size!=LC_DNG_HEADER_BYTES+s->bytes) return 0;
    s->save_stage=7;return 1;
}
static void evidence(struct lv_session *s)
{
    if (!s->path[0] || ml_shutdown_requested || get_ml_card()!=s->card) return;
    char path[FIO_MAX_PATH_LENGTH],line[1024];strcpy(path,s->path);
    memcpy(path+strlen(path)-3,"LOG",3);
    FILE *file=FIO_CreateFile(path);
    if (file) {
        int n=snprintf(line,sizeof(line),
            LIVECOMP_BUILD_ID "\nStatus: %s\n"
            "Error_quarantined_saved: %d,%d,%d\n"
            "Width_height_pitch_bytes: %d,%d,%d,%d\n"
            "Active_x1_y1_x2_y2: %d,%d,%d,%d\n"
            "FPS_x1000_shutter_reciprocal_x1000: %d,%d\n"
            "ISO_aperture_shutter_WB_kelvin: %d,%d,%d,%d,%d\n"
            "Issued_merged_vsyncs: %d,%d,%d\n"
            "Stop_kind_issued: %d,%d\n"
            "Preview_snapshots_calls: %d,%d\n"
            "Session_start_end_ms: %d,%d\n"
            "Error bits: 1=settings 2=memory 4=DMA 8=guard 16=queue 32=timeout 64=IO 128=order 256=shutdown\n"
            "Stop kinds: 1=user 2=cap 3=shutdown 4=error\n"
            "No factory capture, native JPEG decoder or per-input SD write.\n"
            "Frame callbacks and DMA checks do not prove zero sensor dead time.\n",
            status,s->error,s->quarantined,s->saved,s->info.width,s->info.height,s->info.pitch,s->bytes,
            s->info.active_area.x1,s->info.active_area.y1,s->info.active_area.x2,s->info.active_area.y2,
            s->fps,s->tvr,s->iso,s->aperture,s->shutter,s->wb,s->kelvin,
            s->ring.issued,s->merged,s->vsyncs,s->stop_kind,s->stop_issued,
            s->preview_frames,s->preview_calls,s->start_ms,s->end_ms);
        if (n<=0 || n>=(int)sizeof(line) || FIO_WriteFile(file,line,n)!=n) s->error|=E_IO;
        n=snprintf(line,sizeof(line),"Save_stage_written_last_return_size: %d,%d,%d,%d\n",
            s->save_stage,s->save_written,s->save_last_return,s->save_size);
        if (n<=0 || n>=(int)sizeof(line) || FIO_WriteFile(file,line,n)!=n) s->error|=E_IO;
        const struct lv_settings_failure *f=&s->settings_failure;
        n=snprintf(line,sizeof(line),"Settings_failure_phase_mask_ms_vsync_issued: %d,%d,%d,%d,%d\n",
            f->phase,f->mask,f->ms,f->vsync,f->issued);
        if (n<=0 || n>=(int)sizeof(line) || FIO_WriteFile(file,line,n)!=n) s->error|=E_IO;
        if (f->phase) for (unsigned i=0;i<COUNT(setting_names);i++) {
            n=snprintf(line,sizeof(line),"Setting_%s_expected_observed: %d,%d\n",
                setting_names[i],f->expected[i],f->observed[i]);
            if (n<=0 || n>=(int)sizeof(line) || FIO_WriteFile(file,line,n)!=n) s->error|=E_IO;
        }
        n=snprintf(line,sizeof(line),
            "Timing_syncs_before_fps_tvr_after_fps_tvr: %d,%d,%d,%d,%d\nPreview_rendered: %d\n",
            s->timing_syncs,s->timing_before_fps,s->timing_before_tvr,
            s->timing_after_fps,s->timing_after_tvr,s->preview_rendered);
        if (n<=0 || n>=(int)sizeof(line) || FIO_WriteFile(file,line,n)!=n) s->error|=E_IO;
        FIO_CloseFile(file);
    } else s->error|=E_IO;
    memcpy(path+strlen(path)-3,"CSV",3);file=FIO_CreateFile(path);
    if (!file) { s->error|=E_IO;return; }
    const char *header="sequence,slot,start_ms,done_ms,merge_us,dma_state,dma_pointer\n";
    if (FIO_WriteFile(file,header,strlen(header))!=(int)strlen(header)) s->error|=E_IO;
    for (unsigned i=0;i<s->ring.issued;i++) {
        struct lv_frame_log *f=&s->frames[i];
        int n=snprintf(line,sizeof(line),"%d,%d,%d,%d,%d,%d,%x\n",
            f->sequence,f->slot,f->start_ms,f->done_ms,f->merge_us,f->dma_state,f->dma_pointer);
        if (FIO_WriteFile(file,line,n)!=n) { s->error|=E_IO;break; }
    }
    FIO_CloseFile(file);
}
/* Called only after RAW release, preview drain, evidence and UI restoration.
 * A quarantined session stays resident; its callbacks may still hold pointers. */
static void finish_session(struct lv_session *s)
{
    unsigned irq=cli();
    if ((s && s->quarantined) || capture_reading || preview_reading) {
        if (s) s->quarantined=1;
        attempted=1;requested=0;running=0;
        sei(irq);return;
    }
    session=NULL;
    armed=preview_enabled=preview_ready=preview_writing=display_setup=0;
    stop_requested=requested=0;
    sei(irq);
    if (s) free(s);
    /* Keep running set until all old worker cleanup is complete. */
    irq=cli();attempted=0;running=0;sei(irq);
}

static void run(void *unused)
{
    (void)unused;
    if (fullres_check==2) {
        struct lc_frlv_options o={(unsigned)frlv_fps,(unsigned)frlv_expo,
            frlv_input_counts[frlv_inputs>=0 && frlv_inputs<3 ? frlv_inputs:1]};
        unsigned unsafe=livecomp_frlv_run(get_ml_card(),&o,&stop_requested,&status);
        NotifyBox(5000,"%s",status);
        if (unsafe) { requested=0;attempted=1;running=0; }
        else finish_session(NULL);
        return;
    }
    if (fullres_check) {
        unsigned unsafe=livecomp_fullres_check(get_ml_card(),&stop_requested,&status);
        NotifyBox(5000,"%s",status);
        if (unsafe) { requested=0;attempted=1;running=0; }
        else finish_session(NULL);
        return;
    }
    unsigned raw_requested=0,power=0,locked=0,old_lock=0;
    struct lv_session *s=malloc(sizeof(*s));
    if (!s) { status="No session memory; retry";goto finished; }
    memset(s,0,sizeof(*s));lc_ring_init(&s->ring);session=s;
    s->limit=frame_limits[frame_limit>=0 && frame_limit<3 ? frame_limit:2];
    s->preview_interval=preview_intervals[preview_interval>=0 && preview_interval<3 ? preview_interval:0];
    s->card=get_ml_card();s->start_ms=get_ms_clock();
    if (!s->card || !choose_path(s)) { s->error=E_IO;goto cleanup; }
    if (!lv || !is_movie_mode() || lv_dispsize!=1 || RECORDING || lens_info.job_state ||
        !lens_info.raw_iso || !lens_info.wb_mode || raw_lv_is_enabled() || raw_write_chan!=livecomp_camera.raw_channel) {
        status="Use movie M, fixed ISO/WB, normal view; disable other RAW tools";
        s->error=E_SETTINGS;goto cleanup;
    }
    s->fps=fps_get_current_x1000();s->tvr=get_current_shutter_reciprocal_x1000();
    if (s->fps<1000 || s->fps>5000 || s->tvr<=0) {
        status="Set ML FPS override to 3 fps, Low light, slowest shutter";
        s->error=E_SETTINGS;goto cleanup;
    }
    powersave_prohibit();power=1;
    status="Preparing clean LCD composite display";
    if (!prepare_display()) {
        status="LCD unavailable; enable Global Draw and clean LiveView";
        s->error=E_SETTINGS;goto cleanup;
    }
    /* Standard EOS M Q/SET handling synthesizes SET via fake_simple_button.
     * Any non-shutter UI lock suppresses that event and would make Stop fail.
     * Reject an already locked UI; keep only the shutter locked. Mode changes
     * are rejected by settings_valid before another input can be admitted. */
    old_lock=icu_uilock&0xffffu;
    if (old_lock&~UILOCK_SHUTTER) {
        status="Camera controls busy; close other controls and retry";s->error=E_SETTINGS;goto cleanup;
    }
    gui_uilock(old_lock|UILOCK_SHUTTER);locked=1;
    raw_lv_request();raw_requested=1;
    if (!raw_update_params() || !geometry_valid(&raw_info)) {
        status="Unsupported LV RAW size; use normal unzoomed LiveView";
        s->error=E_SETTINGS;goto cleanup;
    }
    s->info=raw_info;s->bytes=s->info.frame_size;
    s->iso=lens_info.raw_iso;s->aperture=lens_info.raw_aperture;s->shutter=lens_info.raw_shutter;
    s->wb=lens_info.wb_mode;s->kelvin=lens_info.kelvin;
    s->gain_r=lens_info.WBGain_R;s->gain_b=lens_info.WBGain_B;
    s->wb_gm=lens_info.wbs_gm;s->wb_ba=lens_info.wbs_ba;s->frame_iso=get_frame_iso();
    for (unsigned i=0;i<LC_LV_SLOTS;i++) {
        s->allocation[i]=fio_malloc(s->bytes+3*GUARD);
        if (!s->allocation[i]) { s->error=E_MEMORY;goto cleanup; }
        s->input[i]=(void *)(((unsigned)s->allocation[i]+2*GUARD-1)&~(GUARD-1));
        memset(s->input[i]-GUARD,0xa5,GUARD);
        memset(s->input[i]+s->bytes,0xa5,GUARD);
    }
    s->accumulator=malloc(s->bytes);s->preview=malloc(s->bytes);
    if (!s->accumulator || !s->preview) { s->error=E_MEMORY;goto cleanup; }
    if (!settings_valid(s,1) || stop_requested || ml_shutdown_requested) {
        status="Stopped before LV capture; see LOG and retry";
        s->error=E_SETTINGS;goto cleanup;
    }
    sync_caches();s->last_vsync=get_ms_clock();preview_enabled=1;
    status="Continuous RAW composite; Q/SET stops";armed=1;
    for (;;) {
        unsigned irq=cli();int slot=lc_ring_take(&s->ring);unsigned active=armed;sei(irq);
        if (slot>=0) {
            unsigned sequence=s->ring.sequence[slot];
            if (sequence!=s->merged+1) { s->error|=E_ORDER;stop(4);break; }
            /* DMA writes only uncached aliases. Cached input aliases are read
             * only; invalidate them AFTER the preceding frame's idle fence. */
            sync_caches();unsigned started=(unsigned)get_us_clock();
            const void *source=(const void *)(uintptr_t)physical(s->input[slot]);
            if (!s->merged) memcpy(s->accumulator,source,s->bytes);
            else lc_raw14_max((void *)s->accumulator,source,s->bytes/14);
            s->frames[sequence-1].merge_us=(unsigned)get_us_clock()-started;
            s->merged++;
            irq=cli();lc_ring_release(&s->ring,slot);sei(irq);
            update_preview(s,0);
            continue;
        }
        if (!active) break;
        if (ml_shutdown_requested) { s->error|=E_SHUTDOWN;stop(3); }
        if (get_ml_card()!=s->card) { s->error|=E_IO;stop(4); }
        if (get_ms_clock()-s->last_vsync>4000000u/(unsigned)s->fps+1000) {
            /* No callback fence: retain ALL capture allocations until reboot. */
            s->error|=E_TIMEOUT;s->quarantined=1;stop(4);break;
        }
        msleep(10);
    }
cleanup:
    /* Stop new callback entrants and drain any callback already holding s.
     * A pending DMA slot or callback timeout makes all ownership uncertain. */
    {
        unsigned irq=cli();armed=0;sei(irq);
        unsigned started=get_ms_clock();
        while (capture_reading && get_ms_clock()-started<2000) msleep(10);
        irq=cli();
        if (capture_reading) { s->quarantined=1;s->error|=E_TIMEOUT; }
        if (s->ring.pending>=0) s->quarantined=1;
        sei(irq);
    }
    s->end_ms=get_ms_clock();
    /* The last callback fenced every admitted input. Release those allocations
     * before saving, leaving room for the filesystem and DNG staging buffer. */
    if (!s->quarantined) for (unsigned i=0;i<LC_LV_SLOTS;i++) if (s->allocation[i]) {
        fio_free(s->allocation[i]);s->allocation[i]=NULL;s->input[i]=NULL;
    }
    /* Publish the final accumulator even if its normal refresh was skipped
     * because the display worker still held the previous snapshot. */
    if (s->merged && !s->quarantined && preview_ready!=s->merged) {
        preview_enabled=0;
        unsigned started=get_ms_clock();
        while (preview_reading && get_ms_clock()-started<2000) msleep(10);
        if (preview_reading) { s->quarantined=1;s->error|=E_TIMEOUT; }
        else { update_preview(s,1);preview_enabled=1; }
    }
    if (s->merged && !s->quarantined && !(s->error&(E_DMA|E_GUARD|E_SETTINGS|E_SHUTDOWN|E_ORDER)) &&
        !ml_shutdown_requested && get_ml_card()==s->card) {
        /* The diagnostic output remains RAW. Native TTJ would contend with
         * ongoing LV hardware and its recipe has only been proved full-res. */
        status="Saving LV composite DNG";
        s->saved=save_lv_dng(s);
        if (!s->saved) s->error|=E_IO;
    }
    /* Let the final snapshot reach the LCD before restoring normal Live View.
     * Saving leaves both accumulator and display snapshot unchanged. */
    if (s->saved && !s->quarantined && !ml_shutdown_requested) {
        unsigned started=get_ms_clock();
        while (s->preview_rendered!=preview_ready && get_ms_clock()-started<2000 &&
               !ml_shutdown_requested) msleep(20);
        if (s->preview_rendered==preview_ready && !ml_shutdown_requested) msleep(1000);
    }
    preview_enabled=0;
    {
        unsigned started=get_ms_clock();
        while (preview_reading && get_ms_clock()-started<2000) msleep(10);
        if (preview_reading) { s->quarantined=1;s->error|=E_TIMEOUT; }
    }
    if (raw_requested) raw_lv_release();
    if (!s->quarantined) {
        for (unsigned i=0;i<LC_LV_SLOTS;i++) if (s->allocation[i]) fio_free(s->allocation[i]);
        if (s->accumulator) free(s->accumulator);
        if (s->preview) free(s->preview);
        s->accumulator=s->preview=NULL;
    }
    if (locked) gui_uilock(old_lock);
    if (power) powersave_permit();
    if (s->quarantined) status="LV capture uncertain; keep logs and reboot";
    else if (s->settings_failure.phase) status="LV settings changed; see LOG and retry";
    else if (s->error&E_QUEUE) status=s->saved ? "CPU fell behind; partial DNG saved":"CPU fell behind; save failed";
    else if (!s->error) status=s->saved ? "Composite saved; press SET for next capture":"No frames captured; press SET to retry";
    else if (!(s->error&E_SETTINGS)) status="LV capture error; keep logs and retry";
    evidence(s);
    if (!s->quarantined && (s->error&E_IO)) status=s->saved ? "DNG saved; evidence incomplete":"Save or evidence failed; check card";
finished:
    NotifyBox(3000,"%s",status);
    finish_session(s);
}
static MENU_SELECT_FUNC(toggle_enabled)
{
    (void)priv;(void)delta;
    enabled=!enabled;
    if (!enabled) { requested=0;if (running) stop(1); }
}
static MENU_UPDATE_FUNC(update_lv)
{
    MENU_SET_HELP("%s",running ? "Press SET to stop and save the DNG.":
        attempted ? "Capture ownership uncertain. Reboot required.":
        enabled ? "Armed. Close menu, then press SET in Movie Live View.":
        "Enable, close menu, then press SET to start. Normal photos do not start capture.");
}
static MENU_UPDATE_FUNC(update_status)
{
    MENU_SET_VALUE("%s",running ? "Running":attempted ? "Reboot":requested ? "Starting":enabled ? "Armed":"Off");
    MENU_SET_HELP("%s",status);
}
static struct menu_entry lv_menu[]={
    {.name="Live Composite",.priv=&enabled,.max=1,.select=toggle_enabled,.update=update_lv,
     .help="Continuous light painting with LCD composite and DNG output.",
     .submenu_width=700,
     .children=(struct menu_entry[]) {
        {.name="Task",.priv=&fullres_check,.max=2,
         .choices=CHOICES("Live composite","Full-res check","Full-res readout"),
         .help="Full-res readout: x5 full-sensor RAW test, saves LVFR DNG+LOG.",
         .help2="Full-res check: old native-engine diagnostic (paused)."},
        {.name="Full-res FPS",.priv=&frlv_fps,.max=4,
         .choices=CHOICES("3 fps","1 fps","0.5 fps (B)","0.5 fps (A)","0.33 fps (A)"),
         .help="Full-res readout only: sensor frame rate.",
         .help2="(B): longer frame via timer B. (A): slower line clock via timer A."},
        {.name="Full-res exposure",.priv=&frlv_expo,.max=3,
         .choices=CHOICES("100% of frame","50% of frame","25% of frame","12% of frame"),
         .help="Full-res readout only: shutter as a fraction of the frame period."},
        {.name="Full-res frames",.priv=&frlv_inputs,.max=2,
         .choices=CHOICES("1 frame","4 frames","8 frames"),
         .help="Full-res readout only: frames captured and max-merged into the DNG."},
        {.name="Capture limit",.priv=&frame_limit,.max=2,
         .choices=CHOICES("30 frames","60 frames","120 frames"),
         .help="Stop and save automatically at this limit. Q/SET stops earlier."},
        {.name="LCD refresh",.priv=&preview_interval,.max=2,
         .choices=CHOICES("Every second","Every 2 seconds","Every 4 seconds"),
         .help="Every RAW frame is merged. First and final composites are displayed."},
        {.name="Status",.update=update_status,
         .help="Press SET to start/stop. After saving, press SET for another capture."},
        {.name="Exposure setup",
         .help="Movie M, manual ISO/WB, normal view. FPS override: 3 fps, Low light; slowest shutter."},
        {.name="Output",
         .help="LCD composite and LVCPnnnn.DNG at card root. LOG/CSV contain capture diagnostics."},
        MENU_EOL
     }}
};
void livecomp_lv_poll(struct card_info *card)
{
    if (!menu_added) { menu_add("Shoot",lv_menu,COUNT(lv_menu));menu_added=1; }
    if (!enabled || ml_shutdown_requested) { requested=0;return; }
    if (requested && !attempted && card && !gui_menu_shown() &&
        gui_state==livecomp_camera.gui_idle && lv && !LV_PAUSED && !lens_info.job_state) {
        attempted=running=1;requested=0;
        if ((int)task_create("lvcomposite",0x1e,0x4000,run,0)<=0) {
            status="Could not create LV worker; retry";finish_session(NULL);
        }
    }
}
MODULE_CONFIGS_START()
    MODULE_CONFIG(enabled)
    MODULE_CONFIG(frame_limit)
    MODULE_CONFIG(preview_interval)
MODULE_CONFIGS_END()
