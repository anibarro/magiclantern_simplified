/* GPL-2.0-or-later. Live Composite controller: menu, start/stop keys and the
 * worker task. The capture itself (full-sensor x5 readout, CPU merge, LCD
 * preview, DNG) is fullres-lv.c. AI-assisted implementation
 * (OpenAI Codex LCM1-LCM14, Anthropic Claude LCM15 onwards).
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
static unsigned menu_added;
static CONFIG_INT("livecomp.enabled",enabled,0);
/* Shutter: index into lc_frr_timing (fullres-regs.h). */
static CONFIG_INT("livecomp.subexp",shutter,LC_FRR_FPS_DEFAULT);
static CONFIG_INT("livecomp.frames",frame_limit,0);
/* Preview only: one frame with the current settings on the LCD, not saved. */
static CONFIG_INT("livecomp.preview",preview_only,0);
static const unsigned frame_limits[]={0,30,60,120,240};   /* 0 = no limit */
static unsigned trigger_ms;
static const char *status;   /* last result; NULL = show the setup hint */

int livecomp_lv_busy(void) { return requested || attempted; }
int livecomp_lv_running(void) { return running; }
static void stop(unsigned kind) { (void)kind;stop_requested=1; }
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
            status=preview_only ? "Starting the preview":"Starting the composite";
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
    const unsigned limit=frame_limits[frame_limit>=0 && frame_limit<(int)COUNT(frame_limits) ? frame_limit:0];
    struct lc_frlv_options o={(unsigned)shutter,limit,(unsigned)preview_only};
    /* Global Draw would draw over the LCD composite: off for the session,
     * then back to the user's setting. */
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
    MENU_SET_HELP("%s",running ? "SET stops (and saves the composite).":
        attempted ? "Capture ownership uncertain. Reboot required.":
        enabled ? "Armed. Close the menu, then press SET in Movie Live View.":
        "Enable, close the menu, then press SET. Normal photos never start a session.");
}
static MENU_UPDATE_FUNC(update_shutter)
{
    const unsigned iso=lens_info.raw_iso ? (unsigned)raw2iso(lens_info.raw_iso):0;
    if (iso) MENU_SET_RINFO("ISO %d f/%d.%d",iso,lens_info.aperture/10,lens_info.aperture%10);
    else MENU_SET_RINFO("Auto ISO!");
    MENU_SET_HELP("%s",shutter==0 ? "Each frame 2 s. The LCD shows a quarter of the composite per frame.":
        "Length of each frame, no gaps. The LCD shows the composite after every frame.");
    if (!iso) MENU_SET_WARNING(MENU_WARN_ADVICE,"Auto ISO: set a fixed ISO on the camera (Movie M).");
    else if (shutter>=(int)LC_FRR_TESTED) MENU_SET_WARNING(MENU_WARN_ADVICE,"This shutter is not yet tested on the camera.");
}
static MENU_UPDATE_FUNC(update_preview)
{
    if (preview_only) MENU_SET_WARNING(MENU_WARN_ADVICE,"Preview only: SET shows one frame, no composite is recorded.");
}
static MENU_UPDATE_FUNC(update_status)
{
    MENU_SET_VALUE("%s",running ? "Running":attempted ? "Reboot":requested ? "Starting":enabled ? "Armed":"Off");
    MENU_SET_HELP("%s",status ? status:"Movie M, fixed ISO/WB, x1 view, ML FPS override OFF, Global Draw OFF.");
}
static struct menu_entry lv_children[]={
    {.name="Shutter",.priv=&shutter,.max=LC_FRR_FPS_CHOICES-1,.update=update_shutter,
     .choices=CHOICES("2 s","2.5 s","3 s","4 s","5 s","6 s","8 s","10 s","13 s","15 s",
                      "20 s","25 s","30 s","40 s","50 s","60 s"),
     .help2="ISO and aperture: set them on the camera (Movie M); they stay fixed."},
    {.name="Preview only",.priv=&preview_only,.max=1,.update=update_preview,
     .help="ON: SET shows one frame with the current shutter/ISO/aperture.",
     .help2="Nothing is saved. Adjust, then turn it OFF for the composite."},
    {.name="Frame limit",.priv=&frame_limit,.max=COUNT(frame_limits)-1,
     .choices=CHOICES("No limit","30 frames","60 frames","120 frames","240 frames"),
     .help="Stop and save automatically after this many frames. SET stops earlier."},
    {.name="Status",.update=update_status,
     .help="SET: start (or preview). SET again: stop and save."},
    MENU_EOL
};
static struct menu_entry lv_menu[]={
    {.name="Live Composite",.priv=&enabled,.max=1,.select=toggle_enabled,.update=update_lv,
     .help="Olympus-style live composite: brightest pixels accumulate into a RAW DNG.",
     .submenu_width=700,
     .children=lv_children}
};
void livecomp_lv_poll(struct card_info *card)
{
    if (!menu_added) { menu_add("Shoot",lv_menu,COUNT(lv_menu));menu_added=1; }
    if (!enabled || ml_shutdown_requested) { requested=0;return; }
    if (requested && !attempted && card && !gui_menu_shown() &&
        gui_state==livecomp_camera.gui_idle && lv && !LV_PAUSED && !lens_info.job_state) {
        attempted=running=1;requested=0;
        if ((int)task_create("lvcomposite",0x1e,0x4000,run,0)<=0) {
            status="Worker failed; retry";attempted=running=0;
        }
    }
}
MODULE_CONFIGS_START()
    MODULE_CONFIG(enabled)
    MODULE_CONFIG(shutter)
    MODULE_CONFIG(frame_limit)
    MODULE_CONFIG(preview_only)
MODULE_CONFIGS_END()
