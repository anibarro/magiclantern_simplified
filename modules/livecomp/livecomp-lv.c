/* GPL-2.0-or-later. Live Composite / Live Bulb controller: menu, start/stop
 * keys and the worker task. The capture itself (full-sensor x5 readout, CPU merge, LCD
 * preview, DNG) is fullres-lv.c. AI-assisted implementation
 * (OpenAI Codex LCM1-LCM14, Anthropic Claude LCM15 onwards).
 *
 * Mode: Live Composite keeps the brightest value of each pixel (moving
 * lights accumulate, the static scene stays at one frame's exposure); Live
 * Bulb adds the frames, like one bulb exposure of frames x shutter that the
 * LCD shows building up.
 *
 * Exposure: the shutter is the length of each frame (menu); ISO and aperture
 * are Canon's own Movie M settings and stay fixed for the whole session.
 *
 * While a session runs, SET (or Q) stops it; every other key is held, so
 * Canon never acts on a key in the full-res x5 mode. Settings are changed
 * before a session with the normal Canon and ML controls; "Preview only"
 * shows one frame with them. Global Draw may be on: it is switched off for
 * the session and restored afterwards.
 */
#include "dryos.h"
#include "module.h"
#include "menu.h"
#include "lens.h"
#include "gui-common.h"
#include "propvalues.h"
#include "notify_box.h"
#include "config.h"
#include "bmp.h"
#include "livecomp-lv.h"
#include "camera-profile.h"
#include "fullres-lv.h"
#include "fullres-regs.h"

static volatile unsigned requested,attempted,running,stop_requested;
/* LCM56: set by the worker itself. task_create() returns ((x<<16)|id)<<1,
 * which can be negative as an int: LCM55 then reported "Worker failed"
 * and cleared running while the session ran, so SET went to Canon (zoom
 * box) and a second SET started a second session. */
static volatile unsigned worker_alive;
static unsigned create_ms;
static unsigned menu_added;
static CONFIG_INT("livecomp.enabled",enabled,0);
/* Mode: LC_MODE_COMPOSITE (0, maximum) or LC_MODE_BULB (1, sum). */
static CONFIG_INT("livecomp.mode",live_mode,LC_MODE_COMPOSITE);
/* Shutter: index into lc_frr_timing (fullres-regs.h). */
/* New key in LCM51 (2 s removed from the table: old indices are invalid). */
static CONFIG_INT("livecomp.shutter",shutter,LC_FRR_FPS_DEFAULT);
static CONFIG_INT("livecomp.frames",frame_limit,0);
/* Preview only: one frame with the current settings on the LCD, not saved. */
static CONFIG_INT("livecomp.preview",preview_only,0);
/* Debug: LOG/CSV diagnostics next to the DNG. OFF: no log files at all. */
static CONFIG_INT("livecomp.debug",debug,0);
/* Dark frame (LCM58): Live Bulb ends with one frame with the lens covered. */
static CONFIG_INT("livecomp.dark",dark_frame,0);
/* Save unprocessed (LCM59): also the DNG before in-camera noise reduction. */
static CONFIG_INT("livecomp.orig",save_orig,0);
/* LCB3 speed experiments (branch livecomp-speed): merge code, preview copy,
 * 2 s test shutter (Live Composite only). */
static CONFIG_INT("livecomp.mcode",merge_code,1);
static CONFIG_INT("livecomp.pvdma",preview_dma,1);
static CONFIG_INT("livecomp.t2s",shutter_2s,0);
static const unsigned frame_limits[]={0,30,60,120,240};   /* 0 = no limit */
static unsigned trigger_ms;
/* Speed benchmark: requested from the menu, started once the menu is closed. */
static volatile unsigned bench_requested;
static const char *status;   /* last result; NULL = show the setup hint */
static int bulb(void) { return live_mode==(int)LC_MODE_BULB; }
static const char *mode_name(void) { return bulb() ? "Live Bulb":"Live Composite"; }

int livecomp_lv_busy(void) { return requested || attempted; }
int livecomp_lv_running(void) { return running; }
/* A counter (LCM58): the dark-frame countdown skips on a further press. */
static void stop(unsigned kind) { (void)kind;if (stop_requested<1000u) stop_requested++; }
void livecomp_lv_shutdown(void) { stop(3); }
/* Only an intentional Q/SET in idle Movie Live View starts a session. Menu,
 * playback, recording and normal shutter operation are never start triggers. */
static int trigger_ready(void)
{
    return enabled && !attempted && !running && !requested &&
        !ml_shutdown_requested && !gui_menu_shown() && gui_state==livecomp_camera.gui_idle &&
        lv && !LV_PAUSED && is_movie_mode() && lv_dispsize==1 &&
        !RECORDING && !lens_info.job_state && !get_halfshutter_pressed();
}
static int session_key(unsigned code);
int livecomp_lv_key(unsigned code)
{
    if (!running) return session_key(code);
    const int pass=session_key(code);
    livecomp_frlv_key_log(code,preview_only,(unsigned)pass);
    return pass;
}
static int session_key(unsigned code)
{
    int trigger=code==MODULE_KEY_PRESS_SET || code==MODULE_KEY_Q;
    if (!running) {
        if (trigger && trigger_ready()) {
            requested=1;trigger_ms=get_ms_clock();
            status=preview_only ? "Starting the preview":bulb() ? "Starting the bulb":"Starting the composite";
            return 0;
        }
        if (requested && (trigger || code==MODULE_KEY_UNPRESS_SET)) return 0;
        return 1;
    }
    /* A single physical Q/SET may produce multiple GUI events: ignore the
     * ones within 500 ms of the press that started the session. */
    if (trigger) {
        if (get_ms_clock()-trigger_ms>=500) stop(1);
        return 0;
    }
    /* Unmapped/internal events must continue to the core. */
    return code==0 || (int)code<0;
}
unsigned livecomp_lv_vsync(unsigned context)
{
    (void)context;
    livecomp_frlv_vsync();
    return CBR_RET_CONTINUE;
}
static void run(void *unused)
{
    (void)unused;
    worker_alive=1;
    const unsigned limit=frame_limits[frame_limit>=0 && frame_limit<(int)COUNT(frame_limits) ? frame_limit:0];
    struct lc_frlv_options o={shutter_2s && !bulb() ? LC_FRR_FPS_2S:(unsigned)shutter,limit,(unsigned)preview_only,(unsigned)debug,
        bulb() ? LC_MODE_BULB:LC_MODE_COMPOSITE,(unsigned)dark_frame,(unsigned)save_orig,
        (unsigned)merge_code,(unsigned)preview_dma};
    /* Global Draw would draw over the LCD composite: off for the session,
     * then back to the user's setting. */
    /* A message still on screen would call redraw() when it expires and
     * bring Canon's overlay back during the session: close it now. */
    NotifyBoxHide();msleep(100);
    const int gd=get_config_var("global.draw");
    if (gd&1) { set_config_var("global.draw",gd&~1);msleep(300);clrscr(); }
    unsigned unsafe=livecomp_frlv_run(get_ml_card(),&o,&stop_requested,&status);
    if (gd&1) set_config_var("global.draw",gd);
    NotifyBox(5000,"%s",status);
    /* Uncertain DMA ownership: buffers stay allocated, no new session. */
    if (unsafe) { requested=0;attempted=1;running=0;return; }
    stop_requested=requested=0;
    attempted=0;running=0;
}

static MENU_SELECT_FUNC(toggle_enabled)
{
    (void)priv;(void)delta;
    enabled=!enabled;
    if (!enabled) { requested=0;if (running) stop(1); }
}
static MENU_UPDATE_FUNC(update_lv)
{
    MENU_SET_NAME("%s",mode_name());
    MENU_SET_HELP("%s",running ? (bulb() ? "SET stops (and saves the bulb exposure).":"SET stops (and saves the composite)."):
        attempted ? "Capture ownership uncertain. Reboot required.":
        enabled ? "Armed. Close the menu, then press SET. DNG: DCIM/LIVE.":
        "Enable, close the menu, then press SET. Normal photos never start a session.");
    /* Sessions need Movie mode (the full-res x5 readout is a movie-mode
     * Live View); in photo mode the module stays inert (SET is Canon's). */
    if (!is_movie_mode() && !running)
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Switch the camera to Movie mode to use %s.",mode_name());
}
static MENU_UPDATE_FUNC(update_shutter)
{
    const unsigned iso=lens_info.raw_iso ? (unsigned)raw2iso(lens_info.raw_iso):0;
    if (iso) MENU_SET_RINFO("ISO %d f/%d.%d",iso,lens_info.aperture/10,lens_info.aperture%10);
    else MENU_SET_RINFO("Auto ISO!");
    MENU_SET_HELP("%s",bulb() ? "Length of each frame, no gaps. Bulb total = frames x shutter.":
        "Length of each frame, no gaps. The LCD shows the composite after every frame.");
    if (shutter_2s && !bulb()) MENU_SET_VALUE("2 s (test)");
    if (!iso) MENU_SET_WARNING(MENU_WARN_ADVICE,"Auto ISO: set a fixed ISO on the camera (Movie M).");
    else if (shutter_2s && !bulb()) MENU_SET_WARNING(MENU_WARN_ADVICE,"2 s test shutter ON (Speed tests): this setting is ignored.");
    /* LCM55 camera: adding a frame takes ~2.3 s, plus ~0.25 s preview; at
     * 2.5 s the next readout overtakes it and the session stops. */
    else if (bulb() && lc_frr_timing[shutter>=0 && shutter<(int)LC_FRR_FPS_CHOICES ? shutter:(int)LC_FRR_FPS_DEFAULT].period_ms<3000u)
        MENU_SET_WARNING(MENU_WARN_ADVICE,"Live Bulb needs 3 s or longer: adding a frame takes ~2.3 s.");
    else if (shutter>=(int)LC_FRR_TESTED && lc_frr_timing[shutter].period_ms!=10000u) MENU_SET_WARNING(MENU_WARN_ADVICE,"This shutter is not yet tested on the camera.");
}
static MENU_UPDATE_FUNC(update_mode)
{
    MENU_SET_HELP("%s",bulb() ? "Live Bulb: frames add up like one long bulb exposure, built up on the LCD.":
        "Live Composite: only brighter light is added (trails); the scene keeps one frame's exposure.");
    if (bulb()) MENU_SET_WARNING(MENU_WARN_ADVICE,"Expose for the total time (frames x shutter). Shutter 3 s or longer.");
}
static MENU_UPDATE_FUNC(update_frames)
{
    const unsigned n=frame_limits[frame_limit>=0 && frame_limit<(int)COUNT(frame_limits) ? frame_limit:0];
    const unsigned ms=lc_frr_timing[shutter>=0 && shutter<(int)LC_FRR_FPS_CHOICES ? shutter:(int)LC_FRR_FPS_DEFAULT].period_ms;
    if (bulb() && n) {
        const unsigned s=n*ms/1000u;
        if (s>=60) MENU_SET_RINFO("= %dm%02ds bulb",s/60,s%60);
        else MENU_SET_RINFO("= %ds bulb",s);
    }
    MENU_SET_HELP("%s",bulb() ? "Stop and save after this many frames (total = frames x shutter). SET stops earlier.":
        "Stop and save automatically after this many frames. SET stops earlier.");
}
static MENU_UPDATE_FUNC(update_preview)
{
    if (preview_only) MENU_SET_WARNING(MENU_WARN_ADVICE,"Preview only: SET shows one frame, no composite is recorded.");
}
static MENU_UPDATE_FUNC(update_dark)
{
    if (dark_frame) MENU_SET_WARNING(MENU_WARN_ADVICE,"%s",bulb() ? "At the end: cover the lens when asked; dark frames take as long as the bulb.":
        "At the end: cover the lens when asked; one dark frame (one shutter).");
}
static MENU_UPDATE_FUNC(update_orig)
{
    if (save_orig) MENU_SET_WARNING(MENU_WARN_ADVICE,"%s",bulb() ? "LVBRnnnn.DNG: no in-camera correction; LVDKnnnn.DNG: dark frames (sum).":
        "With Dark frame: LVCRnnnn.DNG unprocessed, LVCKnnnn.DNG the dark frame.");
}
static MENU_SELECT_FUNC(bench_select)
{
    (void)priv;(void)delta;
    if (running || attempted) return;
    bench_requested=1;status="Benchmark: close the menu (~1 min)";
}
static MENU_UPDATE_FUNC(update_bench)
{
    MENU_SET_VALUE("%s",bench_requested ? "Requested":running ? "Running":"Run");
}
static void bench_task(void *unused)
{
    (void)unused;
    worker_alive=1;
    NotifyBoxHide();msleep(100);
    livecomp_bench_run(get_ml_card(),&status);
    NotifyBox(8000,"%s",status);
    stop_requested=requested=0;
    attempted=0;running=0;
}
static MENU_UPDATE_FUNC(update_status)
{
    if (!running && !attempted && !is_movie_mode()) {
        MENU_SET_VALUE("Needs Movie mode");
        MENU_SET_HELP("%s","Switch the camera to Movie mode (video); the module is inactive in photo mode.");
        MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Switch the camera to Movie mode.");
        return;
    }
    MENU_SET_VALUE("%s",running ? "Running":attempted ? "Reboot":requested ? "Starting":enabled ? "Armed":"Off");
    MENU_SET_HELP("%s",status ? status:"Movie M, fixed ISO/WB, x1 view, ML FPS override OFF.");
}
static struct menu_entry lv_children[]={
    {.name="Mode",.priv=&live_mode,.max=1,.update=update_mode,
     .choices=CHOICES("Live Composite","Live Bulb"),
     .help="Live Composite: brightest pixels. Live Bulb: frames add up (one long exposure).",
     .help2="Both show the result on the LCD after every frame and save a RAW DNG."},
    {.name="Shutter",.priv=&shutter,.max=LC_FRR_FPS_CHOICES-1,.update=update_shutter,
     .choices=CHOICES("2.5 s","3 s","4 s","5 s","6 s","8 s","10 s","13 s","15 s",
                      "20 s","25 s","30 s","40 s","50 s","60 s"),
     .help2="ISO and aperture: set them on the camera (Movie M); they stay fixed."},
    {.name="Preview only",.priv=&preview_only,.max=1,.update=update_preview,
     .help="ON: SET shows one frame with the current shutter/ISO/aperture.",
     .help2="Nothing is saved. Adjust, then turn it OFF for the composite."},
    {.name="Frame limit",.priv=&frame_limit,.max=COUNT(frame_limits)-1,.update=update_frames,
     .choices=CHOICES("No limit","30 frames","60 frames","120 frames","240 frames"),
     .help="Stop and save automatically after this many frames. SET stops earlier.",
     .help2="SET during the first frame: a single-frame bulb (SET stops after it)."},
    {.name="Dark frame",.priv=&dark_frame,.max=1,.update=update_dark,
     .help="After the last frame, cover the lens. Bulb: as many dark frames;",
     .help2="Composite: one. Subtracted like long exposure NR. SET skips/stops."},
    {.name="Save unprocessed",.priv=&save_orig,.max=1,.update=update_orig,
     .help="Also save the DNG without in-camera noise reduction (LVBR/LVCR)",
     .help2="and the dark frames (LVDK/LVCK, Dark frame ON). Slower: extra card writes."},
    {.name="Debug",.priv=&debug,.max=1,
     .help="ON: write diagnostic LOG/CSV files next to the DNG (DCIM/LIVE).",
     .help2="OFF: no log files; only the composite DNG is saved."},
    {.name="Merge code",.priv=&merge_code,.max=2,
     .choices=CHOICES("C","ARM","ARM fast load"),
     .help="SPEED TEST. Live Composite merge: C (LCM63), ARM (hand-written,",
     .help2="branch-free), ARM fast load (LDM; CSV changed_pct then 0). Same image."},
    {.name="Preview copy",.priv=&preview_dma,.max=1,
     .choices=CHOICES("CPU","EDMAC"),
     .help="SPEED TEST. Copy of the LCD preview into Canon's 3 LV buffers:",
     .help2="CPU memcpy (LCM63, ~110 ms) or EDMAC hardware copy (~5 ms)."},
    {.name="2 s shutter (test)",.priv=&shutter_2s,.max=1,
     .help="SPEED TEST. Live Composite with 2 s frames (ignores Shutter).",
     .help2="Live Bulb is not affected. Debug ON to see the merge times in the LOG."},
    {.name="Speed benchmark",.select=bench_select,.update=update_bench,
     .help="Measures memory and merge speed with Live View running (~1 min).",
     .help2="Movie mode, x1. Nothing is saved except DCIM/LIVE/LVBENCH.LOG."},
    {.name="Status",.update=update_status,
     .help="SET: start (or preview). SET again: stop and save."},
    MENU_EOL
};
static struct menu_entry lv_menu[]={
    {.name="Live Composite",.priv=&enabled,.max=1,.select=toggle_enabled,.update=update_lv,
     .help="Live Composite (brightest pixels) or Live Bulb (added light), into a RAW DNG.",
     .submenu_width=700,
     .children=lv_children}
};
void livecomp_lv_poll(struct card_info *card)
{
    if (!menu_added) { menu_add("Shoot",lv_menu,COUNT(lv_menu));menu_added=1; }
    if (!enabled || ml_shutdown_requested) { requested=0;return; }
    if (bench_requested && !running && !attempted && card && !gui_menu_shown() &&
        gui_state==livecomp_camera.gui_idle && lv && !LV_PAUSED && !lens_info.job_state) {
        bench_requested=0;attempted=running=1;worker_alive=0;create_ms=get_ms_clock();
        task_create("lcbench",0x1e,0x4000,bench_task,0);
        return;
    }
    if (requested && !attempted && card && !gui_menu_shown() &&
        gui_state==livecomp_camera.gui_idle && lv && !LV_PAUSED && !lens_info.job_state) {
        attempted=running=1;requested=0;worker_alive=0;create_ms=get_ms_clock();
        task_create("lvcomposite",0x1e,0x4000,run,0);
    }
    else if (running && !worker_alive && get_ms_clock()-create_ms>3000) {
        status="Worker failed; retry";attempted=running=0;
    }
}
MODULE_CONFIGS_START()
    MODULE_CONFIG(enabled)
    MODULE_CONFIG(live_mode)
    MODULE_CONFIG(shutter)
    MODULE_CONFIG(frame_limit)
    MODULE_CONFIG(preview_only)
    MODULE_CONFIG(debug)
    MODULE_CONFIG(dark_frame)
    MODULE_CONFIG(save_orig)
    MODULE_CONFIG(merge_code)
    MODULE_CONFIG(preview_dma)
    MODULE_CONFIG(shutter_2s)
MODULE_CONFIGS_END()
