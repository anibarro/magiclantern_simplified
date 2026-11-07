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
 * Forcing another category made Canon's GUI oscillate (v41), so XPan stays in
 * category 0 and decodes the dial and LEFT from the raw MPU messages instead,
 * before Canon acts on them (mpu_input). RIGHT may arrive as 0x27 only.
 * Therefore:
 *   UP / DOWN (short press): selected value up / down. DOWN also feeds ML's
 *          long-press detector (long = ML menu), which swallows a short press
 *          outside its menu; so a DOWN released within 300 ms counts here.
 *   LEFT / RIGHT: previous / next field. Canon never sees LEFT's fake
 *          half-press; real half-presses are untouched.
 *   Mode : XPan allows Tv, Av and M only; any other mode is switched to M.
 *   INFO : 3:2 = XPan layout on/off; 65:24 = layout top/bottom.
 *   dial : selected value down / up, like DOWN / UP (clockwise = up).
 *          Canon's own exposure change is never applied.
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
/* Raw MPU input (mpu_recv_hook): set from Canon's MPU receive path. */
static volatile int hud_owns_lv;              /* HUD controls active (shoot_task) */
static volatile int mpu_btn38_ms,mpu_sw1_ms;  /* "control used" / half-press switch */
static volatile int mpu_dial_armed;           /* one dial message per "control used" */
static volatile int mpu_dial_drop_ms;         /* last dropped dial message */
static volatile int left_swallow,left_swallow_ms; /* dropping a LEFT press/release */
static volatile int dial_resync_id,dial_resync_value; /* MPU copy to put back */
static volatile int right_press_ms;           /* last PRESS_RIGHT, to pair its release */
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

static unsigned controls_block_mask(void)
{
    unsigned m=0;
    if (!lv) m|=1;
    if (gui_menu_shown()) m|=2;
    if (PLAY_OR_QR_MODE) m|=4;
    if (RECORDING) m|=8;
    if (!xpan_photo_ui_active()) m|=16;
    if (!xpan_photo_hud_is_enabled()) m|=32;
    if (gui_state!=GUISTATE_IDLE) m|=64;
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
static int target_value(unsigned target);
static int key_inner(struct event *event)
{
    if (event->param==GMT_OLC_INFO_CHANGED) return 1;   /* Canon status refresh */
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
    if (event->param==BGMT_PRESS_DOWN) { down_ms=get_ms_clock(); return 1; }
    if (event->param==BGMT_UNPRESS_DOWN) {
        if (down_ms>0 && get_ms_clock()-down_ms<300) { value_request--; dbg_ring[slot]|=0x20000; }
        down_ms=0;
        return 1;
    }
    switch (event->param) {
        case BGMT_WHEEL_LEFT:
        case BGMT_WHEEL_RIGHT: {
            /* Known direction (dial forwarded outside plain Live View). */
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
        case BGMT_PRESS_UP:                       /* value up (also blocks */
            value_request++; break;               /* Canon's drive dialog) */
        case BGMT_UNPRESS_UP:
            break;
        case BGMT_PRESS_RIGHT:                    /* next field */
            right_press_ms=get_ms_clock(); select_request++; break;
        case BGMT_UNPRESS_RIGHT:
            /* Some RIGHT presses arrive as the release only. */
            if (get_ms_clock()-right_press_ms>500) select_request++;
            right_press_ms=0;
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
static void step_field(int field, int sign)
{
    apply(field,sign);
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
#define KEY_LOG_SIZE 1024
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
    /* Periodic Live View status (several per second) would bury the keys. */
    int msg=((regs[0]&0xff)<<8)|(regs[1]&0xff);
    if (msg==0x0917 || msg==0x033b || msg==0x091a) return;
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
/* Messages from the MPU (buttons, dial, properties), Debug info only.
 * InitializeIntercom (RAM 0x380C, ROM FFA6CD9C) stores Canon's receive
 * handler mpu_recv (RAM 0x36A0, ROM FFA6CC30) at 0x4C5BC+0x14; every MPU
 * message goes through that pointer as buf = [len, class, id, data...].
 * The pointer is swapped for a logger that calls the original, only while it
 * still holds Canon's handler, so no Canon code is patched. */
#define MPU_RECV_CBR 0x4C5D0u
#define MPU_RECV_ORIGINAL 0x36A0u
static int mpu_recv_state;             /* 0 not tried, 1 installed, -1 not found */
static int (*mpu_recv_original)(uint8_t *buf, int a1, int a2, int a3);
/* Decode the HUD controls from raw MPU input while the HUD owns Live View
 * (key logs v42-v43). Returns 1 to drop the message before Canon sees it.
 *   dial : 06 26 01 ("control used"), then within ~1 ms 01 05 xx (shutter)
 *          or 01 06 xx (aperture) with the value the MPU moved its dial
 *          target to (+-3 per click). The message is dropped, so Canon's
 *          exposure never changes; the sign is the direction (clockwise =
 *          up); shoot_task puts the camera's value back into the MPU.
 *   LEFT : 06 26 01, 09 0C 01, 05 00 ... 05 0B, 09 0C 00: Canon would turn
 *          this into a half-press/release. A real half-press always sends
 *          the half-press switch 09 06 (with 09 18 E8) just before 09 0C 01,
 *          so 09 0C 01 without it is LEFT, and that press/release is dropped.
 *   RIGHT: 06 1A 00 (release only), left to Canon (GUI 0x27, see key_inner).
 *   status: about 10-1000 ms after a dial click the MPU also reports its
 *          moved value in 0A 08 [mask lo] [mask hi] fields... [1 byte], with
 *          mask bit 0 = 2 bytes (01 80), bit 1 = 6 bytes (01 xx 98 10 00 Tv),
 *          bit 2 = 5 bytes (01 03 50 18 Av). Canon and ML's lens code take
 *          the value from there too, so for 1.5 s after a dropped click the
 *          Tv/Av bytes are rewritten to the camera's value. Messages with
 *          other mask bits are left alone.
 * Canon's mpu_recv only dispatches to its handlers, so a dropped message
 * leaves no state behind. */
static int mpu_status_pin(uint8_t *buf, int len)
{
    if (len<5 || buf[4]!=0 || (buf[3]&~7)) return 0;
    int mask=buf[3],off=5,changed=0;
    if (mask&1) off+=2;
    if (mask&2) {
        if (off+6>len) return 0;
        int tv=lens_info.raw_shutter;
        if (tv && buf[off+5]!=tv) { buf[off+5]=tv; changed=1; }
        off+=6;
    }
    int av=lens_info.raw_aperture;
    if ((mask&4) && off+5<=len && av && buf[off+4]!=av) { buf[off+4]=av; changed=1; }
    return changed;
}
static int mpu_input(uint8_t *buf, int len, int now)
{
    if (len<3) return 0;
    int cls=buf[1],id=buf[2],v=len>=4 ? buf[3]:-1;
    if (cls==0x06 && id==0x26) { mpu_btn38_ms=now; mpu_dial_armed=1; }
    if (cls==0x09 && id==0x06) mpu_sw1_ms=now;
    if (left_swallow && now-left_swallow_ms>3000) left_swallow=0;   /* safety */
    if (left_swallow) {
        if ((cls==0x05 && (id==0x00 || id==0x0b)) || (cls==0x09 && id==0x0c && v==1)) return 1;
        if (cls==0x09 && id==0x0c && v==0) { left_swallow=0; return 1; }
    }
    if (!hud_owns_lv) return 0;
    /* 2 = passed on, rewritten (logged as PINNED). */
    if (cls==0x0a && id==0x08 && mpu_dial_drop_ms && now-mpu_dial_drop_ms<1500 && mpu_status_pin(buf,len)) return 2;
    int recent=now-mpu_btn38_ms<=20;
    /* Only the first dial-target message after "control used": the MPU's
     * echoes of later camera-side changes (XPan's own step) are not clicks. */
    if (cls==0x01 && (id==0x05 || id==0x06) && v>=0 && recent && mpu_dial_armed) {
        mpu_dial_armed=0;
        int cur=id==0x05 ? lens_info.raw_shutter:lens_info.raw_aperture;
        if (!cur || v==cur) return 0;
        wheel_clicks=COERCE(wheel_clicks+(v>cur ? 1:-1),-8,8);
        dial_resync_id=id; dial_resync_value=cur; mpu_dial_drop_ms=now;
        return 1;
    }
    if (cls==0x09 && id==0x0c && v==1 && recent && now-mpu_sw1_ms>30) {
        left_swallow=1; left_swallow_ms=now; select_request--;
        return 1;
    }
    return 0;
}
static int mpu_recv_hook(uint8_t *buf, int a1, int a2, int a3)
{
    if (!buf || (((uint32_t)buf)&~0x40000000u)>=0x20000000u) return mpu_recv_original(buf,a1,a2,a3);
    int len=buf[0],now=get_ms_clock();
    int drop=mpu_input(buf,len,now);
    if (xpan_photo_debug_enabled()) {
        struct key_log_entry entry;
        memset(&entry,0,sizeof(entry));
        uint8_t bytes[12]={0};
        for (int i=0;i<len-1 && i<12;i++) bytes[i]=buf[i+1];
        entry.ms=now; entry.type=10; entry.arg=len; entry.param=drop;
        entry.obj0=(bytes[0]<<24)|(bytes[1]<<16)|(bytes[2]<<8)|bytes[3];
        entry.obj1=(bytes[4]<<24)|(bytes[5]<<16)|(bytes[6]<<8)|bytes[7];
        entry.obj2=(bytes[8]<<24)|(bytes[9]<<16)|(bytes[10]<<8)|bytes[11];
        uint32_t old=cli();
        key_log[key_log_head%KEY_LOG_SIZE]=entry; key_log_head++;
        sei(old);
    }
    if (drop==1) return 0;
    return mpu_recv_original(buf,a1,a2,a3);
}
/* Put the camera's dial target value back into the MPU after a dropped dial
 * message, with the message Canon itself sends for it (class 01, 2 bytes). */
static void dial_resync(void)
{
    uint32_t old=cli();
    int id=dial_resync_id,value=dial_resync_value; dial_resync_id=0;
    sei(old);
    if (!id) return;
    static uint8_t data[2];
    int (*send)(int,int,int,const void *)=(int (*)(int,int,int,const void *))MPU_SEND;
    data[0]=value; data[1]=0;
    send(0x01,id,2,data);
}
/* Installed while the XPan photo UI is active: the HUD controls need it. */
static void mpu_recv_install(void)
{
    if (mpu_recv_state || !xpan_photo_ui_active()) return;
    volatile uint32_t *cbr=(volatile uint32_t *)MPU_RECV_CBR;
    mpu_recv_state=-1;
    if (*cbr!=MPU_RECV_ORIGINAL) return;
    mpu_recv_original=(int (*)(uint8_t *,int,int,int))MPU_RECV_ORIGINAL;
    *cbr=(uint32_t)mpu_recv_hook;
    mpu_recv_state=1;
}

/* Diagnostics (Debug info): who erases the HUD, and is the AF frame known?
 * Sample three HUD-band pixels on screen; log each change of them and of
 * the AF-frame inputs. A HUD pixel turning empty also forces a HUD redraw. */
int xpan_photo_get_hud_y(void);              /* cropmarks.c */
int get_afframe_rect(int *x, int *y, int *w, int *h, int *W, int *H);
void xpan_native_capture_diag(char *buf, int size);   /* xpan-native-look.c */
static void hud_watch(unsigned mask)
{
    if (!xpan_photo_debug_enabled() || mask || !lv) return;
    uint8_t *real=bmp_vram_real();
    int y=xpan_photo_get_hud_y();
    if (!real || y<0 || y+XPAN_PHOTO_HUD_HEIGHT>480) return;
    int p0=real[(xpan_photo_get_hud_top_y()+2)*BMPPITCH+360],p1=real[(y+100)*BMPPITCH+700],p2=real[(y+200)*BMPPITCH+20];
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
static void key_log_flush(void)
{
    button_hook_install();
    mpu_hook_install();
    mpu_recv_install();
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
    /* Capture path state (xpan-native-look.c), logged when it changes. */
    static char last_diag[240];
    char diag[240];
    xpan_native_capture_diag(diag,sizeof(diag));
    int diag_changed=strcmp(diag,last_diag)!=0;
    if ((head==key_log_written && !diag_changed) || get_ms_clock()-last_ms<1000) return;
    last_ms=get_ms_clock();
    FILE *f=FIO_CreateFileOrAppend("ML/LOGS/XPANKEYS.LOG");
    if (!f) return;
    char line[300];
    if (diag_changed) {
        int n=snprintf(line,sizeof(line),"%8d CAPTURE %s\n",(int)get_ms_clock(),diag);
        if (n>0) FIO_WriteFile(f,line,MIN(n,(int)sizeof(line)-1));
        memcpy(last_diag,diag,sizeof(last_diag));
    }
    if (head-key_log_written>KEY_LOG_SIZE) {
        int n=snprintf(line,sizeof(line),"--- %d entries lost\n",(int)(head-key_log_written-KEY_LOG_SIZE));
        FIO_WriteFile(f,line,n);
        key_log_written=head-KEY_LOG_SIZE;
    }
    for (;key_log_written!=head;key_log_written++) {
        const struct key_log_entry *e=&key_log[key_log_written%KEY_LOG_SIZE];
        int n=e->type==10
            ? snprintf(line,sizeof(line),"%8d MPU RECV len %d: %08x %08x %08x%s\n",
                (int)e->ms,e->arg,e->obj0,e->obj1,e->obj2,e->param==1 ? " DROPPED":e->param==2 ? " PINNED":"")
            : e->type==2
            ? snprintf(line,sizeof(line),"%8d HUDWATCH px %06x af %d (1 rect,2 MF,4 lens) rect %d,%d %dx%d guimode %d\n",
                (int)e->ms,e->param,e->arg,e->obj,e->obj0,e->obj1,e->obj2,e->field)
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

/* One value step on the selected field: UP/DOWN and the dial (+1 = UP or
 * clockwise). On WB in Kelvin it adjusts the temperature; otherwise WB steps
 * the presets. Taps always step the presets, which is how Kelvin is left. */
static void value_step(int sign)
{
    if (hud_field==XPAN_HUD_WB && lens_info.wb_mode==WB_KELVIN) { kelvin_toggle(0,sign); msleep(80); }
    else step_field(hud_field,sign);
}
/* Dial clicks decoded from raw MPU input (mpu_input), or forwarded with a
 * known direction as GUI events 2/3: one value step each on the selected
 * field (clockwise = up). The MPU's copy of its dial target is put back
 * first, so a step on that same setting starts from the camera's value. */
static void handle_dial(int active)
{
    dial_resync();
    uint32_t old=cli();
    int clicks=wheel_clicks; wheel_clicks=0;
    sei(old);
    if (!active || lens_info.job_state || !clicks) return;
    dbg_used++;
    for (int i=0;i<ABS(clicks) && i<8;i++) value_step(clicks>0 ? 1:-1);
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

/* XPan needs a predictable exposure and Canon's look settings unlocked:
 * Tv, Av or M. Anything else (P, Creative Auto, scene modes) is switched to M
 * while XPan is ON. Three quick tries, then one every 10 s for as long as the
 * mode is wrong; the A+ dial position never gets here (XPan is off there). */
static void keep_manual_mode(void)
{
    static int tries,last_ms;
    int m=shooting_mode;
    if (m==SHOOTMODE_M || m==SHOOTMODE_AV || m==SHOOTMODE_TV) { tries=0; return; }
    if (lens_info.job_state || get_ms_clock()-last_ms<(tries<3 ? 1500:10000)) return;
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
    keep_clean_display(mask&~32u);
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
    if (!mask) for (int i=0;i<taps;i++) step_field(XPAN_HUD_WB,1);
    hud_watch(mask);
    old=cli(); int values=value_request; value_request=0; sei(old);
    if (!mask && values)
        for (int i=0;i<ABS(values) && i<8;i++) value_step(values>0 ? 1:-1);
    hud_owns_lv=!mask;                    /* mpu_input decodes the controls */
    mpu_recv_install();
    handle_dial(!mask);
    key_log_flush();
}
