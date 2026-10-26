/* XPan Live View HUD controls (see xpan-hud-controls.h).
 * The GUI handler only records events; xpan_hud_controls_step(), called from
 * ML's shoot_task (~50 ms), does the work, because Canon property changes can
 * wait for acknowledgement and must not stall the GUI task.
 *
 * EOS M 2.0.2 dial (key logs v36-v41): Canon tells the MPU which GUI category
 * is active (FF0D76FC). In category 0 (plain Live View) the MPU applies the
 * dial to the exposure setting itself and only reports "a control was used"
 * (button 38 -> GUI 0x54), so the direction never reaches the camera's processor.
 * In any other category (menus, Q) it forwards the dial as button 13 with a
 * signed value, which Canon turns into GUI events 2 (left) / 3 (right).
 * While the XPan HUD owns Live View it keeps the MPU out of category 0, so
 * every click arrives as 2/3. RIGHT arrives as 0x27. Therefore:
 *   UP / DOWN (short press): selected value up / down. DOWN also feeds ML's
 *          long-press detector (long = ML menu), which swallows a short press
 *          outside its menu; so a DOWN released within 300 ms counts here.
 *   Mode : XPan allows Tv, Av and M only; any other mode is switched to M.
 *   INFO : 3:2 = XPan layout on/off; 65:24 = layout top/bottom.
 *   dial : selects the next setting. In plain Live View the MPU applies the
 *          dial itself and reports only "a control was used" (0x54 with no
 *          follow-up key), so its direction is unknown; Canon's own change
 *          to its dial target is undone. If a 2/3 event does arrive (known
 *          direction), the selection moves next/previous accordingly.
 * With XPan > Debug info on, every key event (and the dial target value) is
 * logged to ML/LOGS/XPANKEYS.LOG.
 *   touch: tapping a HUD field only selects it; values change with UP/DOWN. Exception: tapping the
 *          already selected WB field steps the WB preset (touch point from Canon's touch
 *          state). A tap or drag on the picture moves the AF frame (XPan
 *          touch AF, mapped through the 65:24 row shift).
 *   Q/SET: short press (< 400 ms) cycles Canon's focus magnification
 *          1x -> 5x -> 10x -> 1x at the AF frame; held 500 ms opens Canon's
 *          Q screen as before (sent as a fake Q_SET).
 *   REC  : unused.
 * UP (Canon's drive dialog) is blocked. DOWN stays with ML (long press = menu).
 * Canon's GUI layer is kept off the screen while the HUD owns Live View. */
#include "dryos.h"
#include "lens.h"
#include "property.h"
#include "propvalues.h"
#include "menu.h"
#include "zebra.h"
#include "bmp.h"
#include "focus.h"
#include "shoot.h"
#include "gui-common.h"
#include "xpan-mode.h"
#include "cropmarks.h"
#include "xpan-hud-controls.h"
#include "patch.h"
#include "crop-mode-hack.h"
int xpan_movie_enabled(void);   /* cropmarks.c: XPan ON/OFF in movie mode */

/* Provided by XPan (cropmarks.c / xpan-workflow.inc). */
void xpan_photo_capture_format_step(int sign);
void xpan_native_look_step_cube(int delta);

/* Left-to-right order of the HUD (the mode box sits top-left of the values). */
/* FOCUS is display-only: the EOS M ignores ML's PROP_AF_MODE requests (v20). */
static const int field_order[]={XPAN_HUD_MODE,XPAN_HUD_ISO,XPAN_HUD_APERTURE,XPAN_HUD_SHUTTER,
    XPAN_HUD_WB,XPAN_HUD_DRIVE,XPAN_HUD_METER,XPAN_HUD_FORMAT,XPAN_HUD_EV,XPAN_HUD_LUT};

static volatile int hud_field=XPAN_HUD_SHUTTER;
static volatile int select_request;           /* pending UP/DOWN moves */
static volatile int info_request;             /* pending INFO layout toggles */
void xpan_photo_info_toggle(void);            /* cropmarks.c */
int xpan_photo_hud_hit_center(int field);     /* xpan-photo-hud.inc */
static volatile int zoom_request;             /* short SET: next magnification */
static volatile int dbg_set_press,dbg_set_release,dbg_zoom_req,dbg_zoom_from,dbg_zoom_to;
static volatile int movie_set_ms;             /* movie mode Q/SET held since (ms) */
static volatile int crop_toggle_request;      /* short SET in movie: 1:1 crop on/off */
static volatile int set_down_ms;              /* Q/SET held since (ms); -1 = long press sent */
static volatile int wb_taps;                  /* taps on the selected WB field: next preset */
static volatile int wheel_clicks;             /* dial clicks for the HUD field, signed */
static volatile int value_request;           /* UP/DOWN value steps, signed */
static volatile int dial_prefix_pending,dial_prefix_ms; /* bare 0x54 = dial in Live View */
static volatile int dial_mode_replay;         /* key to replay after leaving dial mode */
static volatile int open_menu_request;        /* long DOWN in the dial mode */
static void dial_mode_schedule_clears(void);
void xpan_photo_hud_force_redraw(void);       /* xpan-photo-hud.inc */
static volatile int dbg_wheel_raw,dbg_wheel_seen;
static volatile int dbg_touch_raw,dbg_touch_obj,dbg_touch_hit=-9;
int xpan_photo_hud_hit_test(int x, int y);    /* xpan-photo-hud.inc */
int xpan_photo_touch_af(int x, int y);        /* xpan-photo-hud.inc */

/* Touch point of a TOUCH event, LCD pixels. Method from the EOS M "slim GUI"
 * build (eosm_touch_get_xy): event->obj word 1 is a low-RAM pointer
 * (0x900000..0x9FFFFF) whose word 1 packs Y<<16 | X. Only aligned, valid-
 * looking addresses are dereferenced. Fallback: TOUCH_XY_RAW1 (consts.h). */
static int touch_valid(uint32_t v, int *x, int *y)
{
    int cx=v&0xffff,cy=(v>>16)&0xffff;
    if (!v || cx>=720 || cy>=480) return 0;
    *x=cx; *y=cy; return 1;
}
static int touch_point(struct event *event, int *x, int *y)
{
    uint32_t obj=(uint32_t)event->obj,w1=0,packed=0;
    if (obj && !(obj&0xF0000000) && !(obj&3)) w1=((volatile uint32_t *)obj)[1];
    if (w1>=0x00900000 && w1<0x00A00000 && !(w1&3)) packed=((volatile uint32_t *)w1)[1];
    dbg_touch_obj=(int)packed;
    dbg_touch_raw=(int)MEM(0x4D868);
    if (touch_valid(packed,x,y)) return 1;
    return touch_valid((uint32_t)dbg_touch_raw,x,y);
}

/* Diagnostics for XPan > Debug info (HUD test line; no file logging). */
static volatile int dbg_events,dbg_last,dbg_mask=-1,dbg_used,dbg_task_mask=-1,dbg_task_loops;
static volatile int dbg_ring[9],dbg_ring_pos;

int xpan_hud_controls_field(void) { return hud_field; }

/* Dial mode (experiment, XPan > Debug info on): while the HUD owns Live
 * View, Canon is put in ML's own menu GUI mode (93 in Live View, as when the
 * ML menu is open). Canon's Live View keeps running, and its GUI state makes
 * the MPU forward the dial with its direction (button 13 -> 2/3). */
#define XPAN_DIAL_GUI_MODE 93
static volatile int dial_mode_held;           /* we put Canon in that mode */
int xpan_hud_dial_mode_active(void) { return dial_mode_held && CURRENT_GUI_MODE==XPAN_DIAL_GUI_MODE; }
static unsigned controls_block_mask(void)
{
    unsigned m=0;
    if (!lv) m|=1;
    if (gui_menu_shown()) m|=2;
    if (PLAY_OR_QR_MODE) m|=4;
    if (RECORDING) m|=8;
    if (!xpan_photo_ui_active()) m|=16;
    if (!xpan_photo_hud_is_enabled()) m|=32;
    if (gui_state!=GUISTATE_IDLE &&
        !(dial_mode_held && CURRENT_GUI_MODE==XPAN_DIAL_GUI_MODE)) m|=64;
    if (!LV_NON_PAUSED) m|=128;
    return m;
}
static int controls_active(void) { return !controls_block_mask(); }

/* A field is disabled when changing it has no effect in the current setup. */
int xpan_hud_controls_field_enabled(int field)
{
    int mode=shooting_mode;
    switch (field) {
        case XPAN_HUD_FOCUS: return 0;                      /* AF/MF not settable */
        case XPAN_HUD_APERTURE:                             /* no electronic aperture, or auto */
            return lens_info.lens_exists && (mode==SHOOTMODE_M || mode==SHOOTMODE_AV);
        case XPAN_HUD_SHUTTER: return mode==SHOOTMODE_M || mode==SHOOTMODE_TV;
        case XPAN_HUD_EV: return mode!=SHOOTMODE_M || !lens_info.raw_iso; /* M: Auto ISO only */
    }
    return 1;
}
static void select_step(int sign)
{
    int n=COUNT(field_order),i=0;
    while (i<n && field_order[i]!=hud_field) i++;
    if (i>=n) i=0;
    for (int k=0;k<n;k++) {
        i=(i+n+(sign ? sign:1))%n;
        if (xpan_hud_controls_field_enabled(field_order[i])) { hud_field=field_order[i]; return; }
        if (!sign) break;
    }
}

/* Words of a generic 0x54 event's obj; read only for an aligned pointer into
 * the EOS M's 512 MB RAM (cached or uncached alias). -1 if not readable. */
static int event_obj_word(struct event *event, int index)
{
    uint32_t obj=(uint32_t)event->obj,ram=obj&~0x40000000u;
    if ((obj&3) || ram<0x4000 || ram>=0x20000000u-64) return -1;
    return ((volatile int *)obj)[index];
}
static int target_field(unsigned target);
static int target_value(unsigned target);
static int key_inner(struct event *event)
{
    if (event->param==GMT_OLC_INFO_CHANGED) return 1;   /* Canon status refresh */
    /* A key right after 0x54 means that 0x54 was its prefix, not the dial. */
    if (dial_prefix_pending && event->param!=GMT_GUICMD_PRESS_BUTTON_SOMETHING) dial_prefix_pending=0;
    /* Key log for XPan > Debug info only; slot 8 is a scratch slot otherwise. */
    int slot=8;
    if (xpan_photo_debug_enabled()) {
        dbg_events++; dbg_last=event->param;
        slot=dbg_ring_pos++&7;
        dbg_ring[slot]=(event->param&0xfff)|(IS_FAKE(event) ? 0x10000:0);
    }
    unsigned mask=controls_block_mask();
    dbg_mask=mask;
    /* INFO toggles the layout, so it must also work while the HUD is hidden. */
    if (event->param==BGMT_INFO && !IS_FAKE(event) && !(mask&~32u)) {
        info_request++; dbg_ring[slot]|=0x20000; return 0;
    }
    if (event->param==BGMT_WHEEL_LEFT || event->param==BGMT_WHEEL_RIGHT) dbg_wheel_raw++;
    /* Movie mode, XPan ON, not recording: short Q/SET toggles Canon's 1:1
     * movie crop mode (1920x1080 from the sensor centre, ~3x). Held 500 ms
     * = Canon's Q screen. Same press/release handling as the photo zoom. */
    if (is_movie_mode() && xpan_movie_enabled() && lv && !RECORDING && !gui_menu_shown() &&
        !PLAY_OR_QR_MODE && gui_state==GUISTATE_IDLE && !IS_FAKE(event)) {
        if (event->param==BGMT_Q_SET || event->param==BGMT_PRESS_SET) {
            if (!movie_set_ms) movie_set_ms=get_ms_clock();
            return 0;
        }
        if (event->param==BGMT_UNPRESS_SET && movie_set_ms) {
            if (movie_set_ms>0 && get_ms_clock()-movie_set_ms<400) crop_toggle_request=1;
            movie_set_ms=0;
            return 1;
        }
    }
    /* EOS M Q/SET: Canon sends BGMT_Q_SET on press and BGMT_UNPRESS_SET on
     * release. ML's own long-press code (menu.c) starts on a real Q_SET and
     * misses a release that is swallowed, which opened the Q screen on every
     * press. XPan therefore takes the real press itself (livecomp pattern)
     * and always lets the release through. Works with the HUD hidden too. */
    if ((event->param==BGMT_Q_SET || event->param==BGMT_PRESS_SET) &&
        !IS_FAKE(event) && !(mask&~32u)) {
        if (!set_down_ms) set_down_ms=get_ms_clock();
        dbg_set_press++;
        dbg_ring[slot]|=0x20000;
        return 0;
    }
    if (event->param==BGMT_UNPRESS_SET && !IS_FAKE(event) && set_down_ms) {
        if (set_down_ms>0 && get_ms_clock()-set_down_ms<400) zoom_request=1;
        set_down_ms=0; dbg_set_release++;
        return 1;
    }
    if (mask) return 1;
    if (IS_FAKE(event)) return 1;
    /* Short DOWN: timed here, both events still go on to ML's long-press code. */
    static int down_ms;
    if (dial_mode_held && (event->param==BGMT_PRESS_DOWN || event->param==BGMT_UNPRESS_DOWN)) {
        /* In the dial mode Canon auto-repeats PRESS_DOWN while held, which
         * defeats ML's long-press detector: time the press here. Short =
         * previous setting; held 600 ms = open the ML menu (shoot_task). */
        if (event->param==BGMT_PRESS_DOWN) {
            if (!down_ms) down_ms=get_ms_clock();
            else if (get_ms_clock()-down_ms>=600 && down_ms>0) { open_menu_request=1; down_ms=-1; }
        } else {
            if (down_ms>0 && get_ms_clock()-down_ms<300) select_request--;
            down_ms=0;
        }
        dbg_ring[slot]|=0x20000;
        return 0;
    }
    if (event->param==BGMT_PRESS_DOWN) { down_ms=get_ms_clock(); return 1; }
    if (event->param==BGMT_UNPRESS_DOWN) {
        if (down_ms>0 && get_ms_clock()-down_ms<300) { value_request--; dbg_ring[slot]|=0x20000; }
        down_ms=0;
        return 1;
    }
    switch (event->param) {
        case GMT_GUICMD_PRESS_BUTTON_SOMETHING:
            /* Prefix of every control; a dial click if no key follows. */
            dial_prefix_pending=1; dial_prefix_ms=get_ms_clock();
            return 1;
        case BGMT_WHEEL_LEFT:
        case BGMT_WHEEL_RIGHT: {
            /* Known direction: move the selection next / previous. */
            int sign=event->param==BGMT_WHEEL_RIGHT ? 1:-1;
            uint32_t old=cli(); wheel_clicks=COERCE(wheel_clicks+sign,-8,8); sei(old);
            dbg_wheel_seen++;
            break;
        }
        case BGMT_TOUCH_1_FINGER:
        case BGMT_TOUCH_MOVE:
        case BGMT_UNTOUCH_1_FINGER: {
            /* XPan owns every Live View touch: HUD taps only select fields;
             * picture taps (and drags) move the AF frame through XPan's own
             * mapping, so Canon never draws its 3:2 AF box over the screen. */
            static int touch_kind;                /* 0 none, 1 HUD, 2 picture */
            static int move_ms;
            int x,y;
            if (event->param==BGMT_UNTOUCH_1_FINGER) { touch_kind=0; break; }
            if (event->param==BGMT_TOUCH_MOVE) {
                if (touch_kind==2 && get_ms_clock()-move_ms>=100 && touch_point(event,&x,&y)) {
                    move_ms=get_ms_clock(); xpan_photo_touch_af(x,y);
                }
                break;
            }
            if (!touch_point(event,&x,&y)) { dbg_touch_hit=-9; touch_kind=0; return 1; }
            int hit=xpan_photo_hud_hit_test(x,y);
            dbg_touch_hit=hit;
            if (hit==-1) { touch_kind=2; move_ms=get_ms_clock(); xpan_photo_touch_af(x,y); break; }
            touch_kind=1;
            if (hit<0 || !xpan_hud_controls_field_enabled(hit)) break;
            /* Touch selects only; values change with UP/DOWN. Exception: a tap
             * on the already selected WB field steps the WB preset list
             * (UP/DOWN and the dial adjust Kelvin there). */
            if (hit==XPAN_HUD_WB && hit==hud_field) {
                uint32_t old=cli(); wb_taps=COERCE(wb_taps+1,0,8); sei(old);
                break;
            }
            hud_field=hit;
            break;
        }
        case BGMT_Q_SET:
        case BGMT_MENU:
        case BGMT_PLAY:
            /* Canon's ML-menu mode would swallow these: return Canon to
             * Live View first, then replay the key (shoot_task). */
            if (!dial_mode_held) return 1;
            dial_mode_replay=event->param;
            break;
        case BGMT_PRESS_UP:                       /* value up (also blocks */
            value_request++; break;               /* Canon's drive dialog) */
        case BGMT_UNPRESS_UP:
            break;
        default:
            return 1;
    }
    dbg_ring[slot]|=0x20000;
    return 0;
}

/* Debug info: the last Canon UI states seen (gui_state/GUI mode/overlay
 * mode), recorded on every change by the step function. */
static volatile int dbg_state_ring[6],dbg_state_pos,dbg_state_last=-1;
static void record_ui_state(void)
{
    if (!xpan_photo_debug_enabled()) return;
    int v=((gui_state&0xff)<<16)|((CURRENT_GUI_MODE&0xff)<<8)|(LV_OVERLAYS_MODE&0xff);
    if (v==dbg_state_last) return;
    dbg_state_last=v; dbg_state_ring[dbg_state_pos++%6]=v|0x1000000;
}
void xpan_hud_controls_events(char *buf, int size)
{
    int n=0; buf[0]=0;
    for (int i=6;i>0 && n<size-12;i--) {
        int v=dbg_state_ring[(dbg_state_pos-i+60)%6];
        if (!v) continue;
        n+=snprintf(buf+n,size-n,"%d/%d/%d ",(v>>16)&0xff,(v>>8)&0xff,v&0xff);
    }
}
void xpan_hud_controls_debug(char *buf, int size)
{
    snprintf(buf,size,"K%d M%x S%d/%d Z%d %x>%x U%d W%d/%d h%d",dbg_events,dbg_mask,
        dbg_set_press,dbg_set_release,dbg_zoom_req,dbg_zoom_from,dbg_zoom_to,dbg_used,
        dbg_wheel_raw,dbg_wheel_seen,dbg_touch_hit);
}

/* Exposure compensation in Canon's 1/3-stop raw steps, +-3 EV on EOS M. */
static void step_ev(int sign)
{
    int ae=lens_info.ae;
    for (int k=1;k<=8;k++) {
        int next=round_expo_comp(COERCE(ae+sign*k,-24,24));
        if (next!=ae) { lens_set_ae(next); return; }
    }
}

static void set_wb_mode(int mode)
{
    prop_request_change(PROP_WB_MODE_LV,&mode,4);
    prop_request_change(PROP_WB_MODE_PH,&mode,4);
}
/* WB list for taps: presets, then Kelvin (last Kelvin value), wrapping. */
static void step_wb(int sign)
{
    static const int presets[]={WB_AUTO,WB_SUNNY,WB_SHADE,WB_CLOUDY,WB_TUNGSTEN,WB_FLUORESCENT,WB_FLASH,WB_KELVIN};
    int n=COUNT(presets),i=0,mode=lens_info.wb_mode;
    while (i<n && presets[i]!=mode) i++;
    i=(i>=n) ? 0 : (i+n+sign)%n;
    if (presets[i]==WB_KELVIN) {
        static int last_kelvin=5200;
        if ((int)lens_info.kelvin>=KELVIN_MIN) last_kelvin=lens_info.kelvin;
        lens_set_kelvin(last_kelvin);
    } else set_wb_mode(presets[i]);
}
/* Wheel on WB: Kelvin adjustment only. */
static void wheel_wb(int sign)
{
    if (lens_info.wb_mode==WB_KELVIN) kelvin_toggle(0,sign);
}

/* len 0: property.c uses Canon's own length (AF mode is 2 bytes on EOS M). */
static void step_cycle(unsigned property,int current,const int *values,int count,int sign)
{
    int i=0;
    while (i<count && values[i]!=current) i++;
    i=(i>=count) ? 0 : (i+sign+count)%count;
    int v=values[i];
    prop_request_change(property,&v,0);
}

static void apply(int field,int sign)
{
    switch (field) {
        case XPAN_HUD_ISO: iso_toggle(0,sign); break;
        case XPAN_HUD_APERTURE: aperture_toggle(0,sign); break;
        case XPAN_HUD_SHUTTER: shutter_toggle(0,sign); break;
        case XPAN_HUD_WB: step_wb(sign); break;
        case XPAN_HUD_FOCUS: {
            static const int modes[]={AF_MODE_ONE_SHOT,AF_MODE_MANUAL_FOCUS};
            step_cycle(PROP_AF_MODE,af_mode,modes,2,sign); break;
        }
        case XPAN_HUD_DRIVE: {
            static const int modes[]={DRIVE_SINGLE,DRIVE_CONTINUOUS,DRIVE_SELFTIMER_2SEC,
                DRIVE_SELFTIMER_REMOTE,DRIVE_SELFTIMER_CONTINUOUS};
            step_cycle(PROP_DRIVE,drive_mode,modes,COUNT(modes),sign); break;
        }
        case XPAN_HUD_METER: {
            static const int modes[]={3,4,1,5};  /* evaluative, partial, spot, center */
            step_cycle(PROP_METERING_MODE,metering_mode,modes,COUNT(modes),sign); break;
        }
        case XPAN_HUD_FORMAT: xpan_photo_capture_format_step(sign); break;
        case XPAN_HUD_EV: step_ev(sign); break;
        case XPAN_HUD_LUT: xpan_native_look_step_cube(sign); break;
        case XPAN_HUD_MODE: {
            static const int modes[]={SHOOTMODE_TV,SHOOTMODE_AV,SHOOTMODE_M};
            int i=0,n=COUNT(modes);
            while (i<n && modes[i]!=shooting_mode) i++;
            set_shooting_mode(modes[i>=n ? n-1:(i+n+sign)%n]);
            break;
        }
    }
}

/* While the XPan HUD owns Live View, stop Canon's GUI from reaching the
 * screen (same mechanism ML uses for its menu and crop_rec uses in x5), so
 * Canon's focus/exposure bars and shortcut dialogs cannot paint over the HUD.
 * Restored as soon as the HUD stops owning the screen (menu, Q, playback). */
static int front_disabled_by_hud;
static void keep_canon_gui_hidden(unsigned mask)
{
    dbg_task_mask=mask; dbg_task_loops++;
    int want=!mask;
    if (want && !canon_gui_front_buffer_disabled()) {
        get_yuv422_vram();
        canon_gui_disable_front_buffer();
        clrscr();
        front_disabled_by_hud=1;
    } else if (!want && front_disabled_by_hud) {
        front_disabled_by_hud=0;
        /* ML's own menu manages the buffer while it is open. */
        if (!gui_menu_shown() && canon_gui_front_buffer_disabled()) canon_gui_enable_front_buffer(0);
    }
}

/* Canon's dial target (shutter, aperture, ISO or compensation), read from
 * Canon's state at 0x517A0. */
static int target_value(unsigned target)
{
    switch (target) {
        case XPAN_WHEEL_SHUTTER: return lens_info.raw_shutter;
        case XPAN_WHEEL_APERTURE: return lens_info.raw_aperture;
        case XPAN_WHEEL_ISO: return lens_info.raw_iso;
        case XPAN_WHEEL_COMPENSATION: return lens_info.ae;
    }
    return 0;
}
static int target_field(unsigned target)
{
    switch (target) {
        case XPAN_WHEEL_SHUTTER: return XPAN_HUD_SHUTTER;
        case XPAN_WHEEL_APERTURE: return XPAN_HUD_APERTURE;
        case XPAN_WHEEL_ISO: return XPAN_HUD_ISO;
        case XPAN_WHEEL_COMPENSATION: return XPAN_HUD_EV;
    }
    return -1;
}
static void step_field(int field, int sign, int wheel)
{
    if (field==XPAN_HUD_WB && wheel) wheel_wb(sign);
    else apply(field,sign);
    msleep(field==XPAN_HUD_FORMAT || field==XPAN_HUD_MODE ? 300:80);
}

/* Key log (XPan > Debug info): the GUI task only fills this RAM ring;
 * shoot_task appends it to ML/LOGS/XPANKEYS.LOG about once a second. */
struct key_log_entry {
    uint32_t ms;
    int type,param,arg,result; /* result: 1 = to Canon/ML, 0 = consumed; -1 = target change */
    int obj,obj0,obj1,obj2;    /* raw obj pointer and its first words */
    uint16_t mask;
    int8_t field,fake;
    uint8_t target;
    int16_t value;
};
#define KEY_LOG_SIZE 512
static struct key_log_entry key_log[KEY_LOG_SIZE];
static volatile unsigned key_log_head;
static unsigned key_log_written;
static struct key_log_entry *key_log_add(void)
{
    struct key_log_entry *e=&key_log[key_log_head%KEY_LOG_SIZE];
    unsigned target=xpan_photo_wheel_targets();
    e->ms=get_ms_clock(); e->mask=controls_block_mask(); e->field=hud_field;
    e->target=target; e->value=target_value(target);
    return e;
}
int xpan_hud_controls_key(struct event *event)
{
    /* Canon's frequent status refresh (0x69) would flood the log. */
    if (!xpan_photo_debug_enabled() || event->param==GMT_OLC_INFO_CHANGED) return key_inner(event);
    /* The entry records the state before the handler and its decision. */
    struct key_log_entry entry,*e=&entry;
    entry=*key_log_add();
    e->type=event->type; e->param=event->param; e->arg=event->arg; e->fake=IS_FAKE(event) ? 1:0;
    e->obj=(int)event->obj; e->obj0=event_obj_word(event,0);
    e->obj1=event_obj_word(event,1); e->obj2=event_obj_word(event,2);
    e->result=key_inner(event);
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
    return entry.result;
}
/* Non-button GUI events (type != 0), logged only; Canon always gets them.
 * Canon's frequent LV refresh (type 2, 0x36) is left out. */
void xpan_hud_controls_log_other(struct event *event)
{
    if (!xpan_photo_debug_enabled() || (event->type==2 && event->param==0x36)) return;
    struct key_log_entry entry=*key_log_add();
    entry.type=event->type; entry.param=event->param; entry.arg=event->arg; entry.fake=0;
    entry.obj=(int)event->obj; entry.obj0=event_obj_word(event,0);
    entry.obj1=event_obj_word(event,1); entry.obj2=event_obj_word(event,2);
    entry.result=1;
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
}
/* Canon's physical-button dispatcher (EOS M 2.0.2 ROM, FF0CC564): r0 = MPU
 * button id, r1 = value (signed byte for dials). It turns buttons into the
 * GUI events ML sees (dial id 13: 2/3, id 38: 0x54...). Debug only: log
 * every call as "BTN id value" to see what the Live View dial sends. */
#define BUTTON_DISPATCH 0xFF0CC564u
#define BUTTON_DISPATCH_INSTR 0xE92D47FCu
static int button_hook_state;          /* 0 not tried, 1 installed, -1 failed */
static void button_dispatch_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    if (pc!=BUTTON_DISPATCH || !xpan_photo_debug_enabled()) return;
    struct key_log_entry entry=*key_log_add();
    entry.type=9; entry.param=(int)regs[0]; entry.arg=(int)regs[1]; entry.fake=0;
    entry.obj=0; entry.obj0=entry.obj1=entry.obj2=-1; entry.result=1;
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
}
static void button_hook_install(void)
{
    if (button_hook_state || !xpan_photo_debug_enabled()) return;
    button_hook_state=-1;
    if (*(volatile uint32_t *)BUTTON_DISPATCH!=BUTTON_DISPATCH_INSTR) return;
    int error=patch_hook_function(BUTTON_DISPATCH,BUTTON_DISPATCH_INSTR,button_dispatch_hook,
            "XPan button dispatch log");
    if (!error) button_hook_state=1;
    struct key_log_entry entry=*key_log_add();
    entry.type=8; entry.param=button_hook_state; entry.arg=error; entry.fake=0;
    entry.obj=0; entry.obj0=entry.obj1=entry.obj2=-1; entry.result=1;
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
}
/* GUI category notification to the MPU (FF0D76FC, r0 = category, r1 = 0).
 * It calls the handler stored in gui_main_struct+0x10 (0x3E164). */
#define GUI_CATEGORY_NOTIFY 0xFF0D76FCu
#define GUI_CATEGORY_NOTIFY_INSTR 0xE92D4070u
#define GUI_CATEGORY_HANDLER 0x3E164u
#define DIAL_FORWARD_CATEGORY 1          /* Canon's category for menus/dialogs */
static int category_hook_state;           /* 0 not tried, 1 installed, -1 failed */
static volatile int canon_category=-1;    /* last category Canon asked for */
static volatile int sent_category=-1;     /* last category the MPU was given */
static volatile int dial_forward_wanted;
static void log_category(int canon, int sent)
{
    if (!xpan_photo_debug_enabled()) return;
    struct key_log_entry entry=*key_log_add();
    entry.type=7; entry.param=canon; entry.arg=sent; entry.fake=0;
    entry.obj=0; entry.obj0=entry.obj1=entry.obj2=-1; entry.result=1;
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
}
static void category_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    if (pc!=GUI_CATEGORY_NOTIFY) return;
    canon_category=(int)regs[0];
    /* Plain Live View while the HUD owns it: keep the dial forwarded. */
    if (regs[0]==0 && dial_forward_wanted) regs[0]=DIAL_FORWARD_CATEGORY;
    sent_category=(int)regs[0];
    log_category(canon_category,sent_category);
}
static void category_send(int category)
{
    void (*handler)(int,int)=*(void (**)(int,int))GUI_CATEGORY_HANDLER;
    if (!handler) return;
    handler(category,0);
    sent_category=category;
    log_category(canon_category,category);
}
/* Called from shoot_task: forward the dial only while the HUD owns Live View. */
static void dial_forward_step(unsigned mask)
{
    if (!category_hook_state) {
        category_hook_state=-1;
        if (*(volatile uint32_t *)GUI_CATEGORY_NOTIFY==GUI_CATEGORY_NOTIFY_INSTR &&
            !patch_hook_function(GUI_CATEGORY_NOTIFY,GUI_CATEGORY_NOTIFY_INSTR,category_hook,
                "XPan dial forwarding")) category_hook_state=1;
    }
    if (category_hook_state!=1) return;
    int want=!mask && canon_category<=0;
    dial_forward_wanted=want;
    if (want && sent_category!=DIAL_FORWARD_CATEGORY) category_send(DIAL_FORWARD_CATEGORY);
    else if (!want && sent_category==DIAL_FORWARD_CATEGORY && canon_category<=0) category_send(0);
}
/* Canon's "send to MPU" routine (RAM 0x38C4; the ROM debug command send_mpu
 * calls it as (class, id, data_len, data)). Debug only, log-only: record
 * every message so the one that makes the MPU forward the dial can be found.
 * Its first instruction is read at run time (code copied to RAM at boot). */
#define MPU_SEND 0x38C4u
static int mpu_hook_state;             /* 0 not tried, 1 installed, -1 failed */
static uint32_t mpu_hook_instr;
static void mpu_send_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    if (pc!=MPU_SEND || !xpan_photo_debug_enabled()) return;
    struct key_log_entry entry;
    memset(&entry,0,sizeof(entry));
    entry.ms=get_ms_clock(); entry.type=6;
    entry.param=(int)(((regs[0]&0xff)<<8)|(regs[1]&0xff)); entry.arg=(int)regs[2];
    entry.obj=(int)regs[3]; entry.obj0=entry.obj1=entry.obj2=0;
    uint32_t data=regs[3]; int len=(int)regs[2];
    if (data && (data&~0x40000000u)<0x20000000u && len>0) {
        uint8_t bytes[12]={0};
        for (int i=0;i<len && i<12;i++) bytes[i]=((volatile uint8_t *)data)[i];
        entry.obj0=(bytes[0]<<24)|(bytes[1]<<16)|(bytes[2]<<8)|bytes[3];
        entry.obj1=(bytes[4]<<24)|(bytes[5]<<16)|(bytes[6]<<8)|bytes[7];
        entry.obj2=(bytes[8]<<24)|(bytes[9]<<16)|(bytes[10]<<8)|bytes[11];
    }
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
}
static void mpu_hook_install(void)
{
    if (mpu_hook_state || !xpan_photo_debug_enabled()) return;
    mpu_hook_state=-1;
    mpu_hook_instr=*(volatile uint32_t *)MPU_SEND;
    int error=-1;
    /* Only a plain register push (STMFD SP!) is safe to relocate. */
    if ((mpu_hook_instr&0xFFFF0000u)==0xE92D0000u) {
        error=patch_hook_function(MPU_SEND,mpu_hook_instr,mpu_send_hook,"XPan MPU send log");
        if (!error) mpu_hook_state=1;
    }
    struct key_log_entry entry;
    memset(&entry,0,sizeof(entry));
    entry.ms=get_ms_clock(); entry.type=5; entry.param=mpu_hook_state;
    entry.arg=error; entry.obj=(int)mpu_hook_instr;
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
}
/* Experiment (Debug info only): the MPU learns Canon's GUI state from MPU
 * message class 04 id 00 (PROP_GUI_STATE: 0 Live View, 6 menu, 9 Q). In a
 * non-zero state it forwards the dial with its direction (button 13).
 * Send that one MPU message directly, without changing Canon's property or
 * GUI, once per HUD activation; give up for good if Canon's GUI reacts. */
#define MPU_GUI_STATE_FORWARD 9
static int mpu_forward_sent,mpu_forward_ms,mpu_forward_disabled;
static void mpu_gui_state_send(int state)
{
    static uint8_t data[2];
    int (*send)(int,int,int,const void *)=(int (*)(int,int,int,const void *))MPU_SEND;
    data[0]=state; data[1]=0;
    send(0x04,0x00,2,data);
}
static void mpu_forward_step(unsigned mask)
{
    if (!xpan_photo_debug_enabled() || mpu_hook_state!=1 || mpu_forward_disabled) return;
    int now=get_ms_clock();
    if (!mpu_forward_sent) {
        static int active_since;
        if (mask) { active_since=0; return; }
        if (!active_since) active_since=now;
        if (now-active_since<3000) return;
        mpu_gui_state_send(MPU_GUI_STATE_FORWARD);
        mpu_forward_sent=1; mpu_forward_ms=now;
        return;
    }
    if (mask) {
        /* Canon's GUI left Live View right after our message: never again. */
        if (now-mpu_forward_ms<1500) mpu_forward_disabled=1;
        mpu_forward_sent=0;
        /* HUD switched off in Live View: tell the MPU we are back in state 0.
         * Any other exit is a Canon screen, which sends its own state. */
        if (mask==32u) mpu_gui_state_send(0);
    }
}
static void log_gui_mode(int mode, int note)
{
    if (!xpan_photo_debug_enabled()) return;
    struct key_log_entry entry;
    memset(&entry,0,sizeof(entry));
    entry.ms=get_ms_clock(); entry.type=3; entry.param=mode; entry.arg=note;
    entry.obj=gui_state; entry.field=hud_field;
    uint32_t old=cli();
    key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
    sei(old);
}
static void dial_mode_leave(int note)
{
    if (!dial_mode_held) return;
    /* The ML menu shares this Canon mode: hand it over unchanged. */
    if (CURRENT_GUI_MODE==XPAN_DIAL_GUI_MODE && !gui_menu_shown()) SetGUIRequestMode(0);
    dial_mode_held=0;
    log_gui_mode(CURRENT_GUI_MODE,note);
}
/* Canon draws its mode-93 screen straight into the shared bitmap layer when
 * the mode changes (and after setting changes). Clear ML's layer and redraw
 * the HUD a few times shortly afterwards so nothing of it stays visible. */
static int clear_at[4];
static void dial_mode_schedule_clears(void)
{
    int now=get_ms_clock();
    clear_at[0]=now+150; clear_at[1]=now+600; clear_at[2]=0; clear_at[3]=0;
}
static void dial_mode_clears(unsigned mask)
{
    int now=get_ms_clock(),due=0;
    for (int i=0;i<4;i++) if (clear_at[i] && now>=clear_at[i]) { clear_at[i]=0; due=1; }
    if (due && !mask && lv) { clrscr(); xpan_photo_hud_force_redraw(); log_gui_mode(CURRENT_GUI_MODE,5); }
}

/* Diagnostics (Debug info): who erases the HUD, and is the AF frame known?
 * Sample three HUD-band pixels on screen; log each change of them and of
 * the AF-frame inputs. A HUD pixel turning empty also forces a HUD redraw. */
int xpan_photo_get_hud_y(void);              /* cropmarks.c */
int get_afframe_rect(int *x, int *y, int *w, int *h, int *W, int *H);
static void hud_watch(unsigned mask)
{
    if (!xpan_photo_debug_enabled() || mask || !lv) return;
    uint8_t *real=bmp_vram_real();
    int y=xpan_photo_get_hud_y();
    if (!real || y<0 || y+XPAN_PHOTO_HUD_HEIGHT>480) return;
    int p0=real[(y+2)*BMPPITCH+360],p1=real[(y+100)*BMPPITCH+700],p2=real[(y+200)*BMPPITCH+20];
    int px=(p0<<16)|(p1<<8)|p2;
    int ax=0,ay=0,aw=0,ah=0,W=0,H=0;
    int af=get_afframe_rect(&ax,&ay,&aw,&ah,&W,&H)|(is_manual_focus()?2:0)|(lens_info.lens_exists?4:0);
    static int last_px=-1,last_af=-1,last_ax=-1;
    if (px!=last_px || af!=last_af || ax!=last_ax) {
        struct key_log_entry entry;
        memset(&entry,0,sizeof(entry));
        entry.ms=get_ms_clock(); entry.type=2; entry.param=px; entry.arg=af;
        entry.obj=ax; entry.obj0=ay; entry.obj1=aw; entry.obj2=ah; entry.field=CURRENT_GUI_MODE;
        uint32_t old=cli();
        key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
        sei(old);
        if (last_px>=0 && (!p0 || !p1 || !p2)) xpan_photo_hud_force_redraw();
        last_px=px; last_af=af; last_ax=ax;
    }
}
static void dial_mode_step(unsigned mask)
{
    static int last_mode=-1,left_ms,enter_ms;
    int mode=CURRENT_GUI_MODE,now=get_ms_clock();
    if (mode!=last_mode) {
        log_gui_mode(mode,0); last_mode=mode;
        if (mode==XPAN_DIAL_GUI_MODE || mode==0) dial_mode_schedule_clears();
    }
    dial_mode_clears(mask);
    if (open_menu_request) {
        open_menu_request=0;
        /* ML's menu uses this same Canon mode: open it in place. */
        if (dial_mode_held) gui_open_menu();
        return;
    }
    int key=dial_mode_replay;
    if (key) {
        dial_mode_replay=0;
        dial_mode_leave(1);
        msleep(150);
        fake_simple_button(key);
        left_ms=now;
        return;
    }
    /* Canon switches modes asynchronously: allow 1 s to reach ours. */
    int settling=dial_mode_held && now-enter_ms<1000;
    if (dial_mode_held && mode!=XPAN_DIAL_GUI_MODE && !settling) {
        /* Canon left our mode by itself (half-press, shot, mode dial...). */
        dial_mode_held=0; left_ms=now; log_gui_mode(mode,2);
    }
    /* While settling, a not-yet-idle GUI state is our own transition. */
    int want=xpan_photo_debug_enabled() && !(settling ? (mask&~64u) : mask);
    if (!want) { dial_mode_leave(3); return; }
    if (dial_mode_held) return;
    /* Enter from plain Live View only, never during focus/capture. */
    if (mode!=0 || get_halfshutter_pressed() || lens_info.job_state || now-left_ms<500) return;
    dial_mode_held=1; enter_ms=now;
    SetGUIRequestMode(XPAN_DIAL_GUI_MODE);
    log_gui_mode(XPAN_DIAL_GUI_MODE,4);
}
static void key_log_flush(void)
{
    button_hook_install();
    mpu_hook_install();
    static int last_ms,last_target=-1,last_value;
    if (!xpan_photo_debug_enabled()) { key_log_written=key_log_head; return; }
    /* Canon's dial target value, logged whenever it changes. */
    unsigned target=xpan_photo_wheel_targets();
    int value=target_value(target);
    if ((int)target!=last_target || value!=last_value) {
        uint32_t old=cli();
        struct key_log_entry *e=key_log_add();
        e->type=0; e->param=-1; e->arg=0; e->fake=0; e->result=-1; e->obj=0; e->obj0=e->obj1=e->obj2=-1;
        key_log_head++;
        sei(old);
        last_target=target; last_value=value;
    }
    static int last_dispsize=-1;
    if (lv_dispsize!=last_dispsize) {
        uint32_t old=cli();
        struct key_log_entry *e=key_log_add();
        e->type=8; e->param=lv_dispsize; e->arg=0; e->fake=0; e->result=0; e->obj=0; e->obj0=e->obj1=e->obj2=-1;
        key_log_head++;
        sei(old);
        last_dispsize=lv_dispsize;
    }
    unsigned head=key_log_head;
    if (head==key_log_written || get_ms_clock()-last_ms<1000) return;
    last_ms=get_ms_clock();
    FILE *f=FIO_CreateFileOrAppend("ML/LOGS/XPANKEYS.LOG");
    if (!f) return;
    char line[160];
    if (head-key_log_written>KEY_LOG_SIZE) {
        int n=snprintf(line,sizeof(line),"--- %d entries lost\n",(int)(head-key_log_written-KEY_LOG_SIZE));
        FIO_WriteFile(f,line,n);
        key_log_written=head-KEY_LOG_SIZE;
    }
    for (;key_log_written!=head;key_log_written++) {
        const struct key_log_entry *e=&key_log[key_log_written%KEY_LOG_SIZE];
        int n=e->type==2
            ? snprintf(line,sizeof(line),"%8d HUDWATCH px %06x af %d (1 rect,2 MF,4 lens) rect %d,%d %dx%d guimode %d\n",
                (int)e->ms,e->param,e->arg,e->obj,e->obj0,e->obj1,e->obj2,e->field)
            : e->type==3
            ? snprintf(line,sizeof(line),"%8d GUI MODE %d gui_state %d note %d (0 seen,1 key,2 canon left,3 hud off,4 enter)\n",
                (int)e->ms,e->param,e->obj,e->arg)
            : e->type==7
            ? snprintf(line,sizeof(line),"%8d ZOOM stage %d (0 start,1 direct,2 pre,3 retry) req %x now %x gui_state %d guimode %d rec %d\n",
                (int)e->ms,e->param,e->arg,e->obj,e->obj0,e->obj1,e->obj2)
            : e->type==8
            ? snprintf(line,sizeof(line),"%8d DISPSIZE now %x\n",(int)e->ms,e->param)
            : e->type==5
            ? snprintf(line,sizeof(line),"%8d MPU HOOK state %d error %x instr %08x\n",
                (int)e->ms,e->param,e->arg,e->obj)
            : e->type==6
            ? snprintf(line,sizeof(line),"%8d MPU SEND class %02x id %02x len %d data %08x %08x %08x\n",
                (int)e->ms,(e->param>>8)&0xff,e->param&0xff,e->arg,e->obj0,e->obj1,e->obj2)
            : e->type==7
            ? snprintf(line,sizeof(line),"%8d GUI CATEGORY canon %d sent %d\n",(int)e->ms,e->param,e->arg)
            : e->type==8
            ? snprintf(line,sizeof(line),"%8d BUTTON HOOK state %d error %x\n",(int)e->ms,e->param,e->arg)
            : e->type==9
            ? snprintf(line,sizeof(line),"%8d BTN id %d value %d (0x%x) field %d target %x value %d\n",
                (int)e->ms,e->param,(int)(signed char)e->arg,e->arg,e->field,e->target,e->value)
            : e->param==-1
            ? snprintf(line,sizeof(line),"%8d TARGET %x value %d field %d mask %x\n",
                (int)e->ms,e->target,e->value,e->field,e->mask)
            : snprintf(line,sizeof(line),"%8d t%d ev %3x arg %x obj %x [%x %x %x] fake %d mask %x field %d target %x value %d -> %s\n",
                (int)e->ms,e->type,e->param,e->arg,e->obj,e->obj0,e->obj1,e->obj2,e->fake,e->mask,e->field,e->target,e->value,
                e->result ? "pass":"CONSUMED");
        if (n>0) FIO_WriteFile(f,line,MIN(n,(int)sizeof(line)-1));
    }
    FIO_CloseFile(f);
}

/* Dial clicks counted by the key handler step the selected HUD field. */
static void target_restore(unsigned target, int v)
{
    switch (target) {
        case XPAN_WHEEL_SHUTTER: lens_set_rawshutter(v); break;
        case XPAN_WHEEL_APERTURE: lens_set_rawaperture(v); break;
        case XPAN_WHEEL_ISO: lens_set_rawiso(v); break;
        case XPAN_WHEEL_COMPENSATION: lens_set_ae(v); break;
    }
}
/* Dial: next setting per click. Canon's own change to its dial target is
 * put back (the MPU applies the dial before any event reaches the camera). */
static void handle_dial(int active)
{
    static unsigned saved_target; static int saved_value,restore_until,restores;
    unsigned target=xpan_photo_wheel_targets();
    int now=get_ms_clock(),v=target_value(target);
    uint32_t old=cli();
    int clicks=wheel_clicks; wheel_clicks=0;
    int bare=dial_prefix_pending && now-dial_prefix_ms>=40;
    if (bare) dial_prefix_pending=0;
    sei(old);
    if (!active || lens_info.job_state || target!=saved_target) {
        saved_target=target; saved_value=v; restore_until=0; return;
    }
    if (bare || clicks) { restore_until=now+700; restores=0; }
    if (restore_until && now<restore_until) {
        if (target && v!=saved_value && restores<2) { target_restore(target,saved_value); restores++; }
    } else {
        /* Keep the pre-click value while a 0x54 is still unresolved. */
        restore_until=0;
        if (!dial_prefix_pending) saved_value=v;
    }
    if (bare) { dbg_used++; select_step(1); }
    if (clicks) { dbg_used++; for (int i=0;i<ABS(clicks);i++) select_step(clicks>0 ? 1:-1); }
}

/* Canon's INFO display modes (changed while XPan was off) must not survive
 * into XPan: return Canon to its clean overlay mode (EOS M state 3) with ML
 * INFO presses. At most 6 presses per episode, so an unexpected state can
 * never make XPan cycle the display forever. */
void xpan_photo_hud_force_redraw(void);   /* xpan-photo-hud.inc */
static void keep_clean_display(unsigned mask)
{
    /* Canon's own erase of its previous display mode cannot reach the screen
     * while its GUI layer is hidden, so its icons would stay frozen there:
     * on every overlay-mode change, clear and redraw ML's layer. */
    static int last_mode=-1;
    int mode=LV_OVERLAYS_MODE;
    if (mode!=last_mode) {
        if (last_mode>=0 && lv && !mask && canon_gui_front_buffer_disabled()) {
            clrscr(); redraw(); xpan_photo_hud_force_redraw();
        }
        last_mode=mode;
    }
    static int tries,last_ms;
    if (!lv || mask || LV_OVERLAYS_MODE==3) { if (LV_OVERLAYS_MODE==3 || !lv) tries=0; return; }
    if (tries>=6 || get_halfshutter_pressed() || lens_info.job_state || get_ms_clock()-last_ms<500) return;
    tries++; last_ms=get_ms_clock();
    fake_simple_button(BGMT_INFO);
}

/* XPan needs a predictable exposure: Tv, Av or M. Anything else (P, Auto,
 * scene/creative modes) is switched to M; at most 3 tries per episode, so a
 * physical Auto switch position can never cause a request loop. */
static void keep_manual_mode(void)
{
    static int tries,last_ms;
    int m=shooting_mode;
    if (m==SHOOTMODE_M || m==SHOOTMODE_AV || m==SHOOTMODE_TV) { tries=0; return; }
    if (lens_info.job_state || tries>=3 || get_ms_clock()-last_ms<1500) return;
    tries++; last_ms=get_ms_clock();
    if (m==SHOOTMODE_P) { set_shooting_mode(SHOOTMODE_M); return; }
    int target=SHOOTMODE_M;
    prop_request_change_wait(PROP_SHOOTING_MODE,&target,4,1000);
}

/* Leaving the XPan photo UI (movie mode, XPan off, green Auto): the HUD
 * bitmap is not redrawn any more, so clear it once. */
static void clear_hud_on_exit(void)
{
    static int was_active;
    int active=xpan_photo_ui_active();
    if (was_active && !active && lv && !gui_menu_shown()) { clrscr(); redraw(); }
    was_active=active;
}

/* Next Canon magnification at the AF frame: 1x -> 5x -> 10x -> 1x.
 * PROP_LV_DISPSIZE must be on the EOS M write allow-list (property.c). */
static void zoom_log(int stage, int requested)
{
    if (!xpan_photo_debug_enabled()) return;
    uint32_t old=cli();
    struct key_log_entry *e=key_log_add();
    e->type=7; e->param=stage; e->arg=requested; e->obj=lv_dispsize; e->fake=0; e->result=0;
    e->obj0=gui_state; e->obj1=CURRENT_GUI_MODE; e->obj2=RECORDING;
    key_log_head++;
    sei(old);
}
static void xpan_zoom_step(void)
{
    int from=lv_dispsize;
    zoom_log(0,from);
    int to=from==1 ? 5 : from==5 ? 10 : 1;
    dbg_zoom_req++; dbg_zoom_from=from;
    if (!lv || RECORDING) { dbg_zoom_to=-1; return; }
    prop_request_change_wait(PROP_LV_DISPSIZE,&to,4,1000);
    zoom_log(1,to);
    dbg_zoom_to=lv_dispsize;
}

/* Called from ML's shoot_task loop (about every 50 ms). */
void xpan_hud_controls_step(void)
{
    record_ui_state();
    clear_hud_on_exit();
    unsigned mask=controls_block_mask();
    /* Canon's GUI stays hidden in XPan Live View even with the layout off. */
    keep_canon_gui_hidden(mask&~32u);
    uint32_t old=cli(); int moves=select_request,infos=info_request; select_request=0; info_request=0; sei(old);
    if (!(mask&~32u) && (infos&1)) xpan_photo_info_toggle();
    if (!(mask&~32u)) keep_manual_mode();
    /* In the dial mode Canon's Live View overlays are not active: no INFO presses. */
    if (!dial_mode_held) keep_clean_display(mask&~32u);
    if (!xpan_hud_controls_field_enabled(hud_field)) select_step(1);
    if (!mask) while (moves) { select_step(moves>0 ? 1:-1); moves+=moves>0 ? -1:1; }
    /* Movie: 1:1 crop toggle and long Q/SET. */
    old=cli(); int crop_toggle=crop_toggle_request; crop_toggle_request=0;
    int movie_long=movie_set_ms>0 && get_ms_clock()-movie_set_ms>=500;
    if (movie_long) movie_set_ms=-1;
    sei(old);
    if (movie_long) fake_simple_button(BGMT_Q_SET);
    if (crop_toggle && is_movie_mode() && !RECORDING) {
        if (!is_crop_hack_supported()) NotifyBox(2000,"1:1 crop: 1080p only, not while recording");
        else if (video_mode_crop) { movie_crop_hack_disable(); NotifyBox(1500,"1:1 crop OFF"); }
        else { movie_crop_hack_enable(); NotifyBox(1500,"1:1 crop ON (1920x1080)"); }
    }
    old=cli(); int zoom=zoom_request; zoom_request=0;
    int long_set=set_down_ms>0 && get_ms_clock()-set_down_ms>=500;
    if (long_set) set_down_ms=-1;
    sei(old);
    if (zoom && !(mask&~32u)) xpan_zoom_step();
    /* Held Q/SET: Canon's Q screen. menu.c ignores fake events, so this
     * reaches Canon directly. */
    if (long_set) fake_simple_button(BGMT_Q_SET);
    old=cli(); int taps=wb_taps; wb_taps=0; sei(old);
    if (!mask) for (int i=0;i<taps;i++) step_field(XPAN_HUD_WB,1,0);
    /* v41: forcing the GUI category from here made Canon's GUI oscillate and
     * turned the screen off (the handler switches Canon's GUI, not just the
     * MPU). Disabled until the MPU-only message is known. */
    (void)dial_forward_step;
    (void)mpu_forward_step;
    (void)dial_mode_step;                 /* GUI mode 93 experiment: off */
    hud_watch(mask);
    old=cli(); int values=value_request; value_request=0; sei(old);
    if (!mask && values)
        for (int i=0;i<ABS(values) && i<8;i++) {
            /* UP/DOWN on WB: in Kelvin they adjust the temperature (like the
             * dial); otherwise they step the presets. Taps always step the
             * presets, which is how Kelvin is left again. */
            if (hud_field==XPAN_HUD_WB && lens_info.wb_mode==WB_KELVIN)
            { kelvin_toggle(0,values>0 ? 1:-1); msleep(80); }
            else step_field(hud_field,values>0 ? 1:-1,0);
        }
    handle_dial(!mask);
    key_log_flush();
}
