/* XPan on the Canon EOS M50 (firmware 1.1.0).
 *
 * Shared with the EOS M (other files): the 65:24 / 3:2 frame, Top / Bottom /
 * Split alignment, the photo HUD, themes, grid and the equivalent lens. This
 * file holds the M50's own stages (HANDOFF-XPAN-M50.md lists them; the
 * research log is README-XPAN-M50.md):
 *
 *  - Canon settings from the HUD: ISO, shutter, aperture (checked writers)
 *    and EV, WB, drive, metering, format (only values learned from Canon).
 *  - Custom LUT on the saved JPEG (Canon's colour table for the shot, through
 *    the colour-table hook) and the Live LUT preview (Canon's style library).
 *  - Film grain in Canon's own creative-filter grain stage.
 *  - 65:24 JPEG written by Canon (size and encoder tables) and the 65:24 live
 *    picture placed by Canon's display hardware (LCD and EVF), with touch AF
 *    and Canon's AF box following it.
 *
 * DIGIC 8 has no runtime ROM hooks: Canon's ROM code is hooked through ML's
 * MMU remap, with a table fixed at build time (platform/M50.110/include/
 * platform/mmu_patches.h); every hook replays the bytes it replaces and the
 * build checks them against the ROM dump. Canon's own structures are never
 * written: Canon gets modified copies, or values through ML's patch API. */
#include "dryos.h"
#include "menu.h"
#include "lens.h"
#include "propvalues.h"
#include "shoot.h"
#include "property.h"
#include "fio-ml.h"
#include "config.h"
#include "tasks.h"
#include "xpan-mode.h"
#include "patch.h"
#include "xpan-hud-controls.h"
#include "mem.h"
#include "timer.h"
#include "xpan-cube.h"
#include "xpan-lut.h"
#include "xpan-native-table.h"
extern void dcache_clean(uint32_t addr, uint32_t size);   /* stubs.S */
int xpan_m50_card_ok(void);     /* power-off guard, below */
static volatile int live_applied;  /* Live LUT: style library graded, photos come out graded already */

#if !defined(CONFIG_M50) || !defined(CONFIG_XPAN) || defined(CONFIG_QEMU)
#error xpan-m50.c is the physical M50 1.1.0 XPan build only.
#endif


/* ------------------------------------------------------------------ */
/* Native look (EOS M: colour table in Canon's develop job).           */

const char *xpan_native_look_status(void) { return "M50: LUT, grain and 65:24 crop on the JPEG."; }
int xpan_native_look_adjusting(void) { return 0; }
void xpan_native_look_mode_changed(void) {}
void xpan_native_look_start_worker(void) {}
void xpan_native_look_note_jpeg(uint32_t jpeg_job) { (void)jpeg_job; }
int xpan_native_look_photo_lut(char *name, unsigned size) { if (size) name[0]=0; return 0; }
int xpan_native_look_loading(void) { return 0; }
/* The HUD's message box (xpan-photo-hud.inc): XPan's frame and 65:24 JPEG
 * need Canon's aspect ratio at 3:2; the user sets it (camera test
 * 2026-11-11: XPan's aspect write had no effect). */
const char *xpan_native_look_blocked(void)
{
    if (xpan_photo_enabled() && !is_movie_mode() && xpan_image_aspect_ratio_valid==1 && xpan_image_aspect_ratio!=0)
        return "Set Canon's aspect ratio to 3:2";
    return 0;
}
void xpan_native_look_menu_sanitize(void) {}
extern char *strrchr(const char *s, int c);
void xpan_photo_hud_force_redraw(void);       /* xpan-photo-hud.inc */
static void m50_lut_step(int delta);          /* LUT selection, below */
static const char *m50_lut_name(void);
void xpan_native_look_step_cube(int delta) { m50_lut_step(delta); }
void xpan_native_capture_diag(char *buf, int size) { if (size>0) snprintf(buf,size,"M50: see Custom LUT, Film grain, 65:24 JPEG crop"); }
/* The HUD's LUT field: the selected file's name (ML/XPAN/LUTS/), and its
 * state. The LUT grades the saved JPEG (colour-table hook), not Live View. */
const char *m50_look_hud_state(void);         /* Custom LUT, below */
void xpan_native_look_hud_label(char *name, unsigned name_size, char *state, unsigned state_size)
{
    const char *n=m50_lut_name();
    if (name_size) snprintf(name,name_size,"%s",n ? n : "CANON (no LUTs in ML/XPAN/LUTS)");
    if (name_size && n) {
        /* KODAK~1.CUB -> KODAK: Canon's file system lists the 8.3 alias of
         * a long name, ".cube" already makes a name long. */
        char *dot=strrchr(name,'.'); if (dot) *dot=0;
        unsigned l=strlen(name), d=l;
        while (d>0 && name[d-1]>='0' && name[d-1]<='9') d--;
        if (d<l && d>=2 && name[d-1]=='~') name[d-1]=0;
    }
    if (state_size) snprintf(state,state_size,"%s",n ? m50_look_hud_state() : "");
}

/* Saved-image grain (EOS M: YUV stage between FOD and TTJ). */
/* xpan_film_grain_enabled: Film grain, at the JPEG encoder setup hook (below). */

/* ------------------------------------------------------------------ */
/* Canon settings report.                                              */

/* Codes as the EOS M uses them. PROP_IMAGE_ASPECT_RATIO (0 = 3:2) and the
 * PICQ codes are not confirmed on the M50 yet: the menu shows the raw
 * values so a first camera test can confirm or correct them. */
static const char *aspect_name(uint32_t code)
{
    switch (code) {
        case 0: return "3:2";
        case 1: return "16:9";
        case 2: return "4:3";
        case 3: return "1:1";
    }
    return "unknown";
}

static const char *mode_name(void)
{
    switch (shooting_mode) {
        case SHOOTMODE_M: return "M";
        case SHOOTMODE_AV: return "Av";
        case SHOOTMODE_TV: return "Tv";
        case SHOOTMODE_P: return "P";
    }
    return "other";
}

static MENU_UPDATE_FUNC(settings_display)
{
    if (xpan_image_aspect_ratio_valid!=1) {
        MENU_SET_VALUE("Aspect not reported");
        MENU_SET_WARNING(MENU_WARN_INFO,"Canon has not reported the aspect ratio. Set it to 3:2 in Canon's menu.");
    } else if (xpan_image_aspect_ratio!=0) {
        MENU_SET_VALUE("Aspect %s (%d)",aspect_name(xpan_image_aspect_ratio),(int)xpan_image_aspect_ratio);
        MENU_SET_WARNING(MENU_WARN_ADVICE,"Set Canon's aspect ratio to 3:2: the 65:24 frame assumes a full 3:2 Live View.");
    } else {
        MENU_SET_VALUE("Aspect 3:2");
    }
    MENU_SET_RINFO("%s",mode_name());
    MENU_SET_HELP("While XPan is on it keeps Picture Style Neutral and an L format with a JPEG; set 3:2 yourself. PICQ %x%s.",
        (unsigned)pic_quality,xpan_pic_quality_valid==1 ? "":" (not reported)");
}

/* ------------------------------------------------------------------ */
/* Exposure probe (read only).                                         */

/* First camera test (2026-11-08): the HUD showed shutter "1/45" (a half-stop
 * value; Canon steps in thirds), aperture "--" and ISO "AUTO". ISO is
 * expected: PROP_ISO is on the M50's handler deny list
 * (platform/M50.110/property_whitelist.h), so ML never reads it. For shutter
 * and aperture this records what Canon actually sends: every PROP_SHUTTER,
 * PROP_APERTURE and their _AUTO variants, and the aperture fields of
 * PROP_LENS_DYNAMIC_DATA (AVEF current, AVO open, AVMAX closed; units not
 * documented yet). Extra handlers next to lens.c's; nothing is written to
 * Canon. XPan > Exposure probe: SET saves ML/LOGS/XM50EXPO.LOG. */
struct expo_rec { uint32_t ms, prop, len, a, b, c, d; int mode; };
/* 32 since 2026-11-09: the exposure writers are done, MMU remap needs the memory. */
#define EXPO_RECS 32
static struct expo_rec expo_ring[EXPO_RECS];
static volatile unsigned expo_head, expo_saved;
static volatile uint32_t last_tv, last_av, last_tv_auto, last_av_auto;
static volatile uint32_t last_avef, last_avo, last_avmax, last_fl;
static volatile unsigned dyn_count;

/* Development records and logs run only with XPan > Debug info ON (README
 * "XPan menu"): nothing is recorded or written otherwise. */
extern int ml_started;                      /* init.c */
static int m50_dbg(void) { return xpan_photo_debug_enabled(); }
static void expo_add(uint32_t prop, uint32_t len, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    if (!m50_dbg()) return;
    struct expo_rec *r=&expo_ring[expo_head%EXPO_RECS];
    r->ms=get_ms_clock(); r->prop=prop; r->len=len;
    r->a=a; r->b=b; r->c=c; r->d=d; r->mode=shooting_mode;
    expo_head++;
}

static void probe_expo(unsigned property, void *token, void *data, unsigned len)
{
    (void)token;
    const uint32_t *buf=data;
    uint32_t v=len ? buf[0] : 0;
    if (property==PROP_SHUTTER) last_tv=v;
    else if (property==PROP_APERTURE) last_av=v;
    else if (property==PROP_SHUTTER_AUTO) last_tv_auto=v;
    else if (property==PROP_APERTURE_AUTO) last_av_auto=v;
    expo_add(property,len,v,lens_info.raw_shutter,lens_info.raw_aperture,lens_info.aperture);
}
REGISTER_PROP_HANDLER_EX(PROP_SHUTTER, probe_expo, 0);
REGISTER_PROP_HANDLER_EX(PROP_APERTURE, probe_expo, 0);
REGISTER_PROP_HANDLER_EX(PROP_SHUTTER_AUTO, probe_expo, 0);
REGISTER_PROP_HANDLER_EX(PROP_APERTURE_AUTO, probe_expo, 0);

/* ISO_AUTO is not on the deny list: log what Canon sends. */
static volatile uint32_t last_iso, last_iso_auto;
static void probe_iso_auto(unsigned property, void *token, void *data, unsigned len)
{
    (void)token;
    uint32_t v=len ? *(uint32_t *)data : 0;
    last_iso_auto=v;
    expo_add(property,len,v,lens_info.raw_iso_auto,lens_info.iso_auto,0);
}
REGISTER_PROP_HANDLER_EX(PROP_ISO_AUTO, probe_iso_auto, 0);

/* ISO reader. PROP_ISO is on the D678 handler deny list
 * (platform/M50.110/property_whitelist.h, added for all D678 bodies in 2021
 * without a confirmed crash on the M50). Instead of lifting the deny for the
 * boot-time registration, XPan registers its own slave for PROP_ISO alone
 * once the camera has finished starting, so a problem cannot stop the boot.
 * It always runs, like the other settings XPan reads (2026-11-11: the guard
 * file and the XPan > ISO reader / ISO write test rows are gone; the
 * power-off hangs they were for were ML's MMU patch stalls, README "Menu
 * hang: Canon's own log"). */
extern void *_prop_cleanup(void *token, int property);
void m50_iso_from_prop(uint32_t value);    /* lens.c */
extern int m50_iso_known;                  /* lens.c */
static unsigned iso_prop_list[]={PROP_ISO};
static void *volatile iso_token;
static volatile unsigned iso_calls;
static volatile int iso_registered;

static void iso_token_handler(void *token) { iso_token=token; }

static void *iso_prop_handler(unsigned property, void *priv, void *addr, unsigned len)
{
    (void)priv;
    if (property==PROP_ISO && addr && len>=4) {
        uint32_t v=*(uint32_t *)addr;
        last_iso=v;
        iso_calls++;
        m50_iso_from_prop(v);
        expo_add(PROP_ISO,len,v,lens_info.raw_iso,lens_info.iso,0);
    }
    return _prop_cleanup(iso_token,property);
}

static void iso_reader_register(void)
{
    if (iso_registered) return;
    iso_registered=1;
    prop_register_slave(iso_prop_list,1,iso_prop_handler,0,iso_token_handler);
}

/* Canon's PROP_ISO values (ISO reader log, 2026-11-08): 0 = Auto, then
 * 0xF00 = ISO 100 and 0x100 per third of a stop up to 0x2700 = ISO 25600. */
#define M50_ISO_MIN 0xF00
#define M50_ISO_MAX 0x2700
static const unsigned short m50_iso_thirds[]={100,125,160,200,250,320,400,500,640,800,1000,
    1250,1600,2000,2500,3200,4000,5000,6400,8000,10000,12800,16000,20000,25600};
static int m50_iso_valid(uint32_t v)
{
    return v==0 || (v>=M50_ISO_MIN && v<=M50_ISO_MAX && (v&0xFF)==0);
}
/* ISO number for the HUD: exact third-stop labels (ML's table stops at 6400). */
int m50_iso_value(void)
{
    uint32_t v=last_iso;
    if (!m50_iso_known || !v || !m50_iso_valid(v)) return 0;
    return m50_iso_thirds[(v-M50_ISO_MIN)>>8];
}

/* ISO write. ML's prop_request_change() cannot be used: it checks the length
 * against ML's own PROP_ISO handler, which is never registered on the M50
 * (deny list), and on DIGIC 8 it allows only prop_write_allow[] (empty). So
 * XPan writes ISO through this one function and nothing else: only a value
 * Canon itself sends (m50_iso_valid), length 4 as Canon reports it, from
 * XPan's own task, never from a property handler or the GUI task. The ISO
 * reader shows Canon's answer. */
static volatile int iso_write_request;      /* 0 none, else value+1 */
static volatile unsigned iso_writes, iso_write_refused;
static uint32_t iso_target, iso_target_ms;  /* last request, for quick dial turns */
void m50_iso_write_request(uint32_t value)
{
    if (!m50_iso_valid(value)) { iso_write_refused++; return; }
    iso_target=value; iso_target_ms=get_ms_clock();
    iso_write_request=(int)value+1;
}
static void m50_iso_write_now(void)
{
    int r=iso_write_request;
    if (!r) return;
    iso_write_request=0;
    uint32_t v=(uint32_t)(r-1);
    if (!m50_iso_valid(v) || !iso_registered || !m50_iso_known) { iso_write_refused++; return; }
    extern void _prop_request_change(unsigned property, const void *addr, size_t len);
    _prop_request_change(PROP_ISO,&v,4);
    iso_writes++;
}
/* One third up or down from what Canon reports; below ISO 100 is Auto. */
/* Quick dial turns: step from the value just requested until Canon has
 * answered (400 ms), not from the older value Canon still reports. */
uint32_t m50_iso_step(int delta)
{
    uint32_t v=get_ms_clock()-iso_target_ms<400 ? iso_target : last_iso;
    if (!m50_iso_valid(v)) return v;
    if (delta>0) return v==0 ? M50_ISO_MIN : MIN(v+0x100,(uint32_t)M50_ISO_MAX);
    if (delta<0) return v<=M50_ISO_MIN ? 0 : v-0x100;
    return v;
}

/* Shutter and aperture, same rules as ISO (2026-11-08, from the Exposure
 * probe log). PROP_SHUTTER, length 4: Tv x 768, 0x100 per third, 0 = 1",
 * 0x2400 = 1/4000. Exposures longer than 1" are negative and their 32-bit
 * form has not been logged yet, so XPan writes 1" .. 1/4000 only.
 * PROP_APERTURE, length 4: low half Av x 768 (0xAE0 = f/3.5 wide open, then
 * 0xB00, 0xC00 ... in thirds up to the lens's smallest aperture, 0x1B00 here);
 * the high half repeats the current shutter value (0x14001200 with shutter
 * 0x1400). XPan keeps the high half exactly as Canon last sent it and changes
 * only the low half, within the lens's AVO..AVMAX (PROP_LENS_DYNAMIC_DATA). */
#define M50_TV_MAX 0x2400
static int m50_tv_valid(uint32_t v) { return v<=M50_TV_MAX && (v&0xFF)==0; }
static int m50_av_valid(uint32_t v)
{
    uint32_t lo=v&0xFFFF;
    if ((v>>16)!=(last_av>>16) || !last_avo || !last_avmax || last_avo>last_avmax) return 0;
    if (lo<last_avo || lo>last_avmax) return 0;
    return lo==last_avo || (lo&0xFF)==0;
}
static volatile int tv_write_request, av_write_request;   /* 0 none, else 1 */
static volatile uint32_t tv_write_value, av_write_value;
static volatile unsigned expo_writes, expo_write_refused;
static uint32_t tv_target, tv_target_ms, av_target, av_target_ms;
static void m50_tv_write_request(uint32_t v)
{
    if (!m50_tv_valid(v)) { expo_write_refused++; return; }
    tv_target=v; tv_target_ms=get_ms_clock();
    tv_write_value=v; tv_write_request=1;
}
static void m50_av_write_request(uint32_t v)
{
    if (!m50_av_valid(v)) { expo_write_refused++; return; }
    av_target=v; av_target_ms=get_ms_clock();
    av_write_value=v; av_write_request=1;
}
static void m50_expo_write_now(void)
{
    extern void _prop_request_change(unsigned property, const void *addr, size_t len);
    if (tv_write_request) {
        tv_write_request=0;
        uint32_t v=tv_write_value;
        if (m50_tv_valid(v) && shooting_mode==SHOOTMODE_M) { _prop_request_change(PROP_SHUTTER,&v,4); expo_writes++; }
        else if (m50_tv_valid(v) && shooting_mode==SHOOTMODE_TV) { _prop_request_change(PROP_SHUTTER,&v,4); expo_writes++; }
        else expo_write_refused++;
    }
    if (av_write_request) {
        av_write_request=0;
        uint32_t v=av_write_value;
        if (m50_av_valid(v) && (shooting_mode==SHOOTMODE_M || shooting_mode==SHOOTMODE_AV)) {
            _prop_request_change(PROP_APERTURE,&v,4); expo_writes++;
        } else expo_write_refused++;
    }
}
static uint32_t m50_tv_step(int delta)
{
    uint32_t v=get_ms_clock()-tv_target_ms<400 ? tv_target : last_tv;
    if (!m50_tv_valid(v)) return 0xFFFFFFFF;        /* longer than 1": not yet */
    if (delta>0) return MIN(v+0x100,(uint32_t)M50_TV_MAX);
    if (delta<0) return v>=0x100 ? v-0x100 : v;
    return v;
}
static uint32_t m50_av_step(int delta)
{
    uint32_t v=get_ms_clock()-av_target_ms<400 ? av_target : last_av;
    uint32_t hi=last_av&0xFFFF0000, lo=v&0xFFFF;
    if (!last_avo || !last_avmax) return 0xFFFFFFFF;
    if (delta>0) lo=MIN((lo&~0xFFu)+0x100,last_avmax);
    else if (delta<0) lo=lo&0xFF ? lo&~0xFFu : lo-0x100;  /* 0xAE0 is already open */
    if (lo<last_avo) lo=last_avo;
    return hi|lo;
}

/* ------------------------------------------------------------------ */
/* Learned settings: EV, WB, drive, metering, file format.            */

/* README-XPAN-M50.md, "Controls, step 6". Their M50 encodings are not
 * logged yet, and a wrong value written to a Canon property can corrupt
 * the settings Canon saves. So XPan writes only what Canon itself wrote:
 * whenever Canon reports a change of one of these settings (from its own
 * screen, Q menu or buttons), XPan waits until the setting's properties have
 * been quiet for 400 ms and records their exact bytes and lengths together
 * as one option. Selecting the field on the HUD then steps through the
 * recorded options only, writing those bytes back with Canon's
 * _prop_request_change, from XPan's task, as for ISO. EV is kept in numeric
 * order and stops at both ends; the others cycle in the order Canon gave them.
 * The options are saved in ML/SETTINGS/XM50OPTS.BIN, so each setting has to
 * be cycled through Canon's screen once.
 *
 * Second camera test (2026-11-08, XM50OPTS.BIN from the card):
 * - EV: PROP_AE is 4 bytes with the compensation in byte 1, in thirds of a
 *   stop (byte 0 always 0): -9..+9 = -3..+3 EV. XPan sorted by byte 0, so
 *   the order was the learning order, and 16 slots could not hold all 19
 *   values. ML's lens_info.ae reads byte 0, so the HUD always showed 0.0 EV.
 *   Now: sorted by bytes 0-1 as a signed 16-bit value, 24 slots, and the HUD
 *   shows m50_ev_thirds().
 * - FORMAT: nothing was learned. Not all three PICQ properties report on the
 *   M50 and XPan waited for all three. Now an option holds the ones that did
 *   report (the others are stored empty and never written). The M50 sets RAW
 *   and JPEG separately (RAW "-" / RAW / cRAW under INFO), so the field steps
 *   JPG -> RAW+JPG -> cRAW+JPG through the learned options that keep the
 *   current JPEG. Every report is logged to ML/LOGS/XM50OPTS.LOG; the third
 *   test's log gave the M50 PICQ layout (M50_PICQ_KIND below), which is not
 *   the EOS M one. */
#define M50_OPT_PROPS 3
#define M50_OPT_MAX 24
#define M50_OPT_MAX_V1 16
#define M50_OPT_BYTES 16
struct m50_prop_val { uint8_t len; uint8_t b[M50_OPT_BYTES]; };
struct m50_ctl {
    int field; const char *name; int nprops; unsigned props[M50_OPT_PROPS]; int numeric; int format;
    struct m50_prop_val cur[M50_OPT_PROPS];     /* last reported by Canon */
    volatile uint32_t changed_ms; volatile int dirty;
    volatile uint32_t seq;                       /* seqlock for cur[]: odd while PropMgr writes */
    int nopts;
    struct m50_prop_val opts[M50_OPT_MAX][M50_OPT_PROPS];
    int target; uint32_t target_ms;              /* last option requested */
    volatile int write_req;                      /* option index + 1 */
    unsigned writes, refused, too_long;
};
static struct m50_ctl m50_ctls[]={
    { .field=XPAN_HUD_EV,     .name="EV",     .nprops=1, .props={PROP_AE}, .numeric=1 },
    { .field=XPAN_HUD_WB,     .name="WB",     .nprops=2, .props={PROP_WB_MODE_LV,PROP_WB_MODE_PH} },
    { .field=XPAN_HUD_DRIVE,  .name="DRIVE",  .nprops=1, .props={PROP_DRIVE} },
    { .field=XPAN_HUD_METER,  .name="METER",  .nprops=1, .props={PROP_METERING_MODE} },
    { .field=XPAN_HUD_FORMAT, .name="FORMAT", .nprops=3, .props={PROP_PIC_QUALITY,PROP_PIC_QUALITY2,PROP_PIC_QUALITY3}, .format=1 },
};
static volatile int m50_opts_dirty;     /* options changed: save the file */
static int m50_opts_loaded;
#define M50_OPTS_FILE "ML/SETTINGS/XM50OPTS.BIN"

static struct m50_ctl *m50_ctl_for_field(int field)
{
    for (unsigned i=0;i<COUNT(m50_ctls);i++) if (m50_ctls[i].field==field) return &m50_ctls[i];
    return 0;
}
/* Report log: every report of these properties and every learned option,
 * appended to ML/LOGS/XM50OPTS.LOG by XPan's task. */
struct m50_rep { uint32_t ms, prop; uint8_t len, b[8]; };
#define M50_REPS 64
static struct m50_rep m50_reps[M50_REPS];
static volatile unsigned m50_rep_head, m50_rep_saved;
static void m50_rep_add(uint32_t prop, unsigned len, const void *data)
{
    uint32_t old=cli();
    struct m50_rep *r=&m50_reps[m50_rep_head%M50_REPS];
    r->ms=get_ms_clock(); r->prop=prop; r->len=len;
    memset(r->b,0,sizeof(r->b));
    if (data) memcpy(r->b,data,MIN(len,sizeof(r->b)));
    m50_rep_head++;
    sei(old);
}
/* Property handler (PropMgr task): only copies Canon's bytes. */
static void m50_ctl_prop(unsigned property, void *token, void *data, unsigned len)
{
    (void)token;
    m50_rep_add(property,len,len ? data:0);
    for (unsigned i=0;i<COUNT(m50_ctls);i++) {
        struct m50_ctl *c=&m50_ctls[i];
        for (int p=0;p<c->nprops;p++) {
            if (c->props[p]!=property) continue;
            if (!data || !len || len>M50_OPT_BYTES) { c->too_long++; continue; }
            /* Seqlock (review 2026-11-09): cli() masks one core only and
             * the XPan task may run on the other. */
            c->seq++;
            asm volatile("dmb" ::: "memory");
            c->cur[p].len=len; memcpy(c->cur[p].b,data,len);
            c->changed_ms=get_ms_clock();
            asm volatile("dmb" ::: "memory");
            c->seq++;
            c->dirty=1;
        }
    }
}
REGISTER_PROP_HANDLER_EX(PROP_AE, m50_ctl_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_WB_MODE_LV, m50_ctl_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_WB_MODE_PH, m50_ctl_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_DRIVE, m50_ctl_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_METERING_MODE, m50_ctl_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_PIC_QUALITY, m50_ctl_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_PIC_QUALITY2, m50_ctl_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_PIC_QUALITY3, m50_ctl_prop, 0);

/* Creative filter properties (hardware grain study, read-only): Canon's
 * property table E0F1E510 has PROP_LV_CFILTER = 0x8004004F (40 bytes: filter
 * kind, ?, 8 levels) and E0F1E54C PROP_MOVIE_CFILTER = 0x8004006E (32 bytes).
 * One writer (PropMgr). */
#define PROP_M50_LV_CFILTER    0x8004004F
#define PROP_M50_MOVIE_CFILTER 0x8004006E
/* Canon's current PROP_LV_CFILTER (40 bytes). One writer (PropMgr), read by
 * XPan's task: seqlock, odd while the handler copies. */
static volatile uint32_t cf_lv_seq;
static uint32_t cf_lv_cur[10];
static volatile int cf_lv_known;
static void m50_cf_prop(unsigned property, void *token, void *data, unsigned len)
{
    (void)token;
    if (property==PROP_M50_LV_CFILTER && data && len==sizeof(cf_lv_cur)) {
        cf_lv_seq++;
        asm volatile("dmb" ::: "memory");
        memcpy(cf_lv_cur,data,sizeof(cf_lv_cur));
        asm volatile("dmb" ::: "memory");
        cf_lv_seq++;
        cf_lv_known=1;
    }
}
REGISTER_PROP_HANDLER_EX(PROP_M50_LV_CFILTER, m50_cf_prop, 0);

static int cf_lv_read(uint32_t out[10])
{
    for (int t=0;t<8;t++) {
        uint32_t s1=cf_lv_seq;
        asm volatile("dmb" ::: "memory");
        memcpy(out,cf_lv_cur,sizeof(cf_lv_cur));
        asm volatile("dmb" ::: "memory");
        if (!(s1&1) && s1==cf_lv_seq) return cf_lv_known;
    }
    return 0;
}
/* Only the values Canon itself sent in the read-only test (README "Hardware
 * grain"): all zero (M), or Grainy B/W = kind 1 with its strength 0..2 in
 * word 3, every other word zero. */
static int cf_lv_canon_value(const uint32_t w[10])
{
    if (w[1] || w[2]) return 0;
    for (int i=4;i<10;i++) if (w[i]) return 0;
    if (w[0]==0) return w[3]==0;
    return w[0]==1 && w[3]<=2;
}

/* Film grain stage switch (README "XPan's own grain in Canon's filter
 * stage"): with Film grain ON in M, XPan sets PROP_LV_CFILTER to Canon's
 * Grainy B/W value (kind 1, strength word 0) through Canon's property API;
 * Canon's DCS handler (E01E22A6) copies it to the shot settings and Canon's
 * decision E01E340A schedules its art filter stages, where XPan's grain then
 * runs instead of Canon's B/W engine (xpan_m50_cf_grain_run). With Film grain
 * OFF: Canon's M value (zeros). Written from XPan's task only, only in M,
 * only from and to values Canon itself sends, not while half-pressed, busy
 * or recording. Canon writes zeros itself on a mode change; XPan writes again
 * only after a new report from Canon (one write per report, so never a loop
 * if Canon keeps another value). */
static int film_stage_wanted(void);          /* Film grain, below */
/* P, Tv, Av, M: Canon's decision E01E340A allows the filter there (+0xAC == 3
 * in every mode), and Canon keeps PROP_LV_CFILTER when switching from M to Av
 * (camera test 2026-11-10: Live View B/W in Av). */
static int m50_pasm(void)
{
    /* In video shooting_mode is the movie exposure mode (0 auto, 3 manual):
     * not a photo mode (propvalues.c is_movie_mode). */
    #define M50_PASM(m) ((m)==SHOOTMODE_M || (m)==SHOOTMODE_AV || (m)==SHOOTMODE_TV || (m)==SHOOTMODE_P)
    return !is_movie_mode() && M50_PASM(shooting_mode) && M50_PASM(shooting_mode_custom);   /* as cropmarks.c xpan_auto_mode */
    #undef M50_PASM
}

/* Live View without Canon's filter (README "Live View without Canon's
 * filter"): Canon's Live View maps the filter kind with E0776CC6
 * (LvCFilterCommon.c), called only by E048CB24, which Live View's
 * PROP_LV_CFILTER handler uses to rebuild Live View with or without its
 * filter path. For XPan's grain (kind 1, Film grain on, M) the hook returns
 * Canon's own value for kind 0 ("no filter"): Live View is built exactly as
 * without a filter, nothing is copied or overwritten, no per-frame work.
 * Photos take the filter from DcsParam (E01E22A6) and keep XPan's grain
 * stage. Any other case: Canon's value. Runs in Canon's Live View task:
 * only reads, no blocking. lvmap_kind1_shown: the last kind-1 map gave
 * Canon's filter (Live View B/W), so XPan's writer refreshes the setting. */
static volatile unsigned lvmap_calls, lvmap_none;
static volatile int lvmap_kind1_shown;
int xpan_m50_lv_cfilter_tramp(int kind);
asm(
    ".text\n.syntax unified\n.thumb\n.align 2\n"
    ".global xpan_m50_lv_cfilter_tramp\n.thumb_func\n"
    "xpan_m50_lv_cfilter_tramp:\n"
    "    push {r4, lr}\n"              /* E0776CC6 */
    "    cmp r0, #8\n"                 /* E0776CC8 */
    "    mov r4, r0\n"                 /* E0776CCA (no flags) */
    "    bcs 2f\n"                     /* E0776CCC: bcc E0776CE4 */
    "    ldr.w pc, 1f\n"
    "2:  ldr.w pc, 3f\n"
    ".align 2\n"
    "1:  .word 0xE0776CE5\n"           /* kind < 8: table lookup, Thumb */
    "3:  .word 0xE0776CCF\n"           /* kind >= 8: Canon's error path, Thumb */
);
int xpan_m50_lv_cfilter_map(int kind)
{
    lvmap_calls++;
    if (kind==1 && film_stage_wanted() && m50_pasm()) {
        lvmap_none++; lvmap_kind1_shown=0;
        return xpan_m50_lv_cfilter_tramp(0);
    }
    lvmap_kind1_shown=(kind==1);
    return xpan_m50_lv_cfilter_tramp(kind);
}

static volatile unsigned cgrain_writes, cgrain_refused;
static const char *cgrain_why="";
static uint32_t cgrain_written_seq=0xffffffffu;   /* cf_lv_seq when XPan last wrote */
static void m50_cgrain_write_now(void)
{
    if (get_halfshutter_pressed() || lens_info.job_state || RECORDING) return;
    if (!m50_pasm()) return;
    uint32_t cur[10];
    uint32_t seq=cf_lv_seq;
    if (!cf_lv_read(cur)) { cgrain_why="Canon has not reported the filter yet. "; return; }
    uint32_t v[10]={0};
    if (film_stage_wanted()) v[0]=1;
    if (v[0] && !memcmp(v,cur,sizeof(v)) && lvmap_kind1_shown && seq!=cgrain_written_seq) {
        /* XPan's value is set but Live View got Canon's filter (it was mapped
         * before Film grain was known, e.g. at start): zeros now, XPan's value
         * again after Canon's report (one write per report). */
        uint32_t z[10]={0};
        cgrain_written_seq=seq;
        extern void _prop_request_change(unsigned property, const void *addr, size_t len);
        _prop_request_change(PROP_M50_LV_CFILTER,z,sizeof(z));
        cgrain_writes++;
        return;
    }
    if (!memcmp(v,cur,sizeof(v))) { cgrain_why=""; return; }
    if (seq==cgrain_written_seq) return;          /* already written for this report */
    cgrain_written_seq=seq;
    if (!cf_lv_canon_value(cur)) { cgrain_refused++; cgrain_why="Canon's filter value is not one XPan knows. "; return; }
    extern void _prop_request_change(unsigned property, const void *addr, size_t len);
    _prop_request_change(PROP_M50_LV_CFILTER,v,sizeof(v));
    cgrain_writes++; cgrain_why="";
}

static int m50_val_eq(const struct m50_prop_val *a, const struct m50_prop_val *b)
{
    return a->len==b->len && !memcmp(a->b,b->b,a->len);
}
static int m50_opt_eq(const struct m50_ctl *c, const struct m50_prop_val *a, const struct m50_prop_val *b)
{
    for (int p=0;p<c->nprops;p++) if (!m50_val_eq(&a[p],&b[p])) return 0;
    return 1;
}
static int m50_opt_find(const struct m50_ctl *c, const struct m50_prop_val *v)
{
    for (int i=0;i<c->nopts;i++) if (m50_opt_eq(c,c->opts[i],v)) return i;
    return -1;
}
/* EV sort key: bytes 0-1 as a signed 16-bit value (the M50 keeps the
 * compensation in byte 1; byte 0 has always been 0). */
static int m50_ev_of(const struct m50_prop_val *v)
{
    return v[0].len>=2 ? (int16_t)(v[0].b[0] | (v[0].b[1]<<8)) : v[0].len ? (int8_t)v[0].b[0] : 0;
}

/* XPan task: record a settled, complete Canon state as an option. */
static void m50_ctl_learn(void)
{
    for (unsigned i=0;i<COUNT(m50_ctls);i++) {
        struct m50_ctl *c=&m50_ctls[i];
        if (!c->dirty || get_ms_clock()-c->changed_ms<400) continue;
        struct m50_prop_val snap[M50_OPT_PROPS];
        c->dirty=0;                             /* a later report sets it again */
        asm volatile("dmb" ::: "memory");
        int consistent=0;
        for (int tries=0;tries<8 && !consistent;tries++) {
            uint32_t s1=c->seq;
            asm volatile("dmb" ::: "memory");
            memcpy(snap,c->cur,sizeof(snap));
            asm volatile("dmb" ::: "memory");
            consistent=!(s1&1) && s1==c->seq;
        }
        if (!consistent) { c->dirty=1; continue; } /* PropMgr busy: next pass */
        /* Only the properties Canon reported (not all three PICQ aliases
         * report on the M50). */
        int reported=0;
        for (int p=0;p<c->nprops;p++) if (snap[p].len) reported++;
        if (!reported || m50_opt_find(c,snap)>=0 || c->nopts>=M50_OPT_MAX) continue;
        int at=c->nopts;
        if (c->numeric) while (at>0 && m50_ev_of(c->opts[at-1])>m50_ev_of(snap)) at--;
        for (int k=c->nopts;k>at;k--) memcpy(c->opts[k],c->opts[k-1],sizeof(c->opts[0]));
        memcpy(c->opts[at],snap,sizeof(c->opts[0]));
        c->nopts++;
        m50_opts_dirty=1;
        uint8_t info[2]={(uint8_t)i,(uint8_t)at};
        m50_rep_add(0,2,info);                  /* "learned" line in the log */
    }
}

/* FORMAT: the PICQ value of an option (first property that reported). */
static int m50_fmt_picq(const struct m50_prop_val *v, uint32_t *q)
{
    for (int p=0;p<M50_OPT_PROPS;p++) if (v[p].len>=4) { memcpy(q,v[p].b,4); return 1; }
    return 0;
}
/* M50 PICQ layout (XM50OPTS.LOG, 2026-11-09; PROP_PIC_QUALITY and _2
 * report the same 4 bytes, _3 never reports):
 *   byte 0: 0 (JPEG size L; never changed in the test)
 *   byte 1: kind, 0x01 JPEG only, 0x07 RAW+JPEG, 0x06 RAW only
 *   byte 2: JPEG quality, 3 fine, 2 normal
 *   byte 3: RAW type, toggled 4 <-> 3 by RAW <-> cRAW in Canon's INFO screen;
 *           4 = RAW and 3 = cRAW is assumed (first step from "-" gave 4, and
 *           Canon's maker notes use 4 for RAW): check the HUD label. */
#define M50_PICQ_KIND(q)   (((q)>>8)&0xFF)
#define M50_PICQ_JPEG(q)   ((q)&0x00FF00FF)      /* size and quality */
static int m50_fmt_has_jpeg(uint32_t q) { return M50_PICQ_KIND(q)&1; }
static int m50_fmt_has_raw(uint32_t q) { return (M50_PICQ_KIND(q)&6)!=0; }
/* Order on the HUD: JPG, RAW+JPG, cRAW+JPG. */
static int m50_fmt_rank(uint32_t q) { return !m50_fmt_has_raw(q) ? 0 : (q>>24)==4 ? 1 : 2; }
/* FORMAT step: among the learned options that keep a JPEG of the current
 * size and quality (RAW only is never chosen: XPan needs the JPEG), the
 * next one in HUD order. -1: fewer than two such options learned yet. */
static int m50_fmt_step(const struct m50_ctl *c, const struct m50_prop_val *from, int delta)
{
    uint32_t q;
    if (!m50_fmt_picq(from,&q)) return -1;
    int list[M50_OPT_MAX], n=0, at=-1;
    for (int rank=0;rank<3;rank++)
        for (int i=0;i<c->nopts;i++) {
            uint32_t o;
            if (!m50_fmt_picq(c->opts[i],&o) || !m50_fmt_has_jpeg(o)) continue;
            if (M50_PICQ_JPEG(o)!=M50_PICQ_JPEG(q) || m50_fmt_rank(o)!=rank) continue;
            if (m50_opt_eq(c,c->opts[i],from)) at=n;
            list[n++]=i;
        }
    if (n<2) return -1;
    if (at<0) return list[0];
    return list[(at+n+(delta>0 ? 1:-1))%n];
}
/* HUD FMT label from Canon's current PICQ (CONFIG_M50 in xpan-photo-hud.inc). */
const char *m50_format_label(void)
{
    if (xpan_pic_quality_valid!=1) return "WAIT";
    uint32_t q=pic_quality;
    int raw=m50_fmt_has_raw(q), jpeg=m50_fmt_has_jpeg(q), fine=((q>>16)&0xFF)==3;
    const char *r=(q>>24)==4 ? "RAW":"CRAW";
    static char label[16];
    if (raw && jpeg) snprintf(label,sizeof(label),"%s+JPG %s",r,fine ? "F":"N");
    else if (raw) snprintf(label,sizeof(label),"%s",r);
    else if (jpeg) snprintf(label,sizeof(label),"JPG %s",fine ? "F":"N");
    else snprintf(label,sizeof(label),"? %x",(unsigned)q);
    return label;
}

int m50_ev_thirds(void)
{
    const struct m50_prop_val *v=&m50_ctls[0].cur[0];
    return v->len>=2 ? (int8_t)v->b[1] : 0;
}

/* Capture format and JPEG quality (README "Capture format on the M50"):
 * built from the M50 PICQ layout Canon reports (byte 0 size, 0 = L; byte 1
 * kind, 0x01 JPEG, 0x07 RAW+JPEG, 0x06 RAW; byte 2 quality, 3 fine, 2
 * normal; byte 3 RAW type, 4 RAW, 3 cRAW), always L with a JPEG (XPan needs
 * it), written to the PICQ aliases Canon reports, from XPan's task only.
 * HUD order: JPG, RAW+JPG, cRAW+JPG. */
static volatile uint32_t lutp_ps_raw;    /* Canon's Picture Style (handler below) */
static volatile unsigned lutp_ps_len;
static volatile int picq_write_req;          /* 0 none, else value+1 (value < 2^31) */
static uint32_t picq_target, picq_target_ms;
static int m50_picq_known(void) { return xpan_pic_quality_valid==1; }
/* RAW chosen (or being set): Film grain uses Canon's creative-filter stage,
 * and Canon allows only JPEG while a creative filter is on (camera test
 * 2026-11-11: RAW could not be set, not even in Canon's menu). So RAW
 * wins: the grain pauses (filter off) while a RAW format is chosen. */
int m50_picq_raw_wanted(void)
{
    if (!m50_picq_known()) return 0;
    uint32_t q=picq_write_req ? picq_target : (uint32_t)pic_quality;
    return m50_fmt_has_raw(q);
}
static uint32_t m50_picq_now(void)
{
    return get_ms_clock()-picq_target_ms<1200 && picq_target ? picq_target : (uint32_t)pic_quality;
}
static int m50_picq_format(uint32_t q) { return !m50_fmt_has_raw(q) ? 0 : (q>>24)==3 ? 2 : 1; }
static uint32_t m50_picq_make(int format, int fine, uint32_t from)
{
    uint32_t raw=format==2 ? 3 : format==1 ? 4 : ((from>>24)==3 ? 3 : 4);   /* JPEG only keeps the RAW type */
    return (raw<<24) | ((uint32_t)(fine ? 3 : 2)<<16) | ((uint32_t)(format ? 0x07 : 0x01)<<8) | 0x00;
}
static void m50_picq_request(uint32_t q)
{
    picq_target=q; picq_target_ms=get_ms_clock();
    picq_write_req=(int)(q+1);
}
void m50_picq_step_format(int delta)
{
    if (!m50_picq_known()) return;
    uint32_t q=m50_picq_now();
    int f=(m50_picq_format(q)+(delta>0 ? 1:2))%3;
    m50_picq_request(m50_picq_make(f,((q>>16)&0xFF)!=2,q));
}
void m50_picq_step_quality(int delta)
{
    (void)delta;
    if (!m50_picq_known()) return;
    uint32_t q=m50_picq_now();
    m50_picq_request(m50_picq_make(m50_picq_format(q),((q>>16)&0xFF)==2,q));
}
const char *m50_capture_format_name(void)
{
    if (!m50_picq_known()) return "Waiting for Canon";
    static const char *n[3]={"JPEG","RAW+JPEG","cRAW+JPEG"};
    return n[m50_picq_format(m50_picq_now())];
}
const char *m50_jpeg_quality_name(void)
{
    if (!m50_picq_known()) return "Waiting for Canon";
    return ((m50_picq_now()>>16)&0xFF)==2 ? "Large Normal" : "Large Fine";
}
/* XPan task. */
static void m50_picq_write_now(void)
{
    extern void _prop_request_change(unsigned property, const void *addr, size_t len);
    int r=picq_write_req;
    if (!r) return;
    picq_write_req=0;
    uint32_t q=(uint32_t)(r-1);
    if (!m50_picq_known() || get_halfshutter_pressed() || lens_info.job_state || RECORDING) return;
    /* RAW: first Canon's filter must be off (the grain pauses, above); kept
     * queued up to 3 s for that. */
    uint32_t cur[10];
    if (m50_fmt_has_raw(q) && cf_lv_read(cur) && cur[0]) {
        if (get_ms_clock()-picq_target_ms<3000) picq_write_req=r;
        return;
    }
    uint32_t kind=M50_PICQ_KIND(q), qual=(q>>16)&0xFF, raw=q>>24;
    if ((q&0xFF) || (kind!=0x01 && kind!=0x07) || (qual!=2 && qual!=3) || (raw!=3 && raw!=4)) return;
    if (q==(uint32_t)pic_quality) return;
    if (xpan_pic_quality1_valid==1) _prop_request_change(PROP_PIC_QUALITY,&q,4);
    if (xpan_pic_quality2_valid==1) _prop_request_change(PROP_PIC_QUALITY2,&q,4);
}

/* Canon settings XPan needs while it is on (README "Forced Canon settings"),
 * as the EOS M does: Picture Style Neutral (0x84) and an L format with a
 * JPEG. The style is saved when XPan changes it and put back when XPan goes
 * off or to movie mode. The aspect ratio is not written: the HUD asks for
 * 3:2 (xpan_native_look_blocked). In P, Tv, Av and M only (Canon locks these
 * elsewhere), from XPan's task, not while Canon is busy, a dialog or a menu
 * is open, at most one try per second per setting and 5 tries per change. */
static struct { int saved; uint32_t v; int tries; uint32_t ms; } force_style;
static int m50_force_try(typeof(force_style) *f, uint32_t now)
{
    if (f->tries>=5 || get_ms_clock()-f->ms<1000) return 0;
    f->tries++; f->ms=get_ms_clock(); (void)now;
    return 1;
}
static void m50_force_step(void)
{
    extern void _prop_request_change(unsigned property, const void *addr, size_t len);
    if (get_halfshutter_pressed() || lens_info.job_state || RECORDING || gui_state!=GUISTATE_IDLE || gui_menu_shown()) return;
    /* Video (README-XPAN-M50.md "Video mode"): the movie LUT grades Canon's
     * style library like Live LUT preview, so Neutral stays while it is on;
     * the photo format (L with a JPEG) is for photos only. */
    extern int xpan_movie_enabled(void);
    int movie=is_movie_mode();
    int on=movie ? xpan_movie_enabled() : xpan_photo_enabled() && m50_pasm();
    uint32_t ps=lutp_ps_raw;
    int ps_known=ps && ps!=0xFFFFFFFF && lutp_ps_len>=1 && lutp_ps_len<=4;
    if (on) {
        if (ps_known && ps!=0x84) {
            if (!force_style.saved) { force_style.saved=1; force_style.v=ps; force_style.tries=0; }
            if (m50_force_try(&force_style,0)) { uint32_t v=0x84; _prop_request_change(PROP_PICTURE_STYLE,&v,lutp_ps_len); }
        } else if (ps==0x84) force_style.tries=0;
        if (!movie && m50_picq_known() && !picq_write_req) {
            uint32_t q=(uint32_t)pic_quality, kind=M50_PICQ_KIND(q);
            if ((q&0xFF) || (kind!=0x01 && kind!=0x07)) {
                static uint32_t last_ms;
                if (get_ms_clock()-last_ms>=1000) {
                    last_ms=get_ms_clock();
                    m50_picq_request(m50_picq_make(kind==0x06 ? 1+((q>>24)==3) : m50_picq_format(q),((q>>16)&0xFF)!=2,q));
                }
            }
        }
        return;
    }
    /* XPan off: put back what XPan changed, if it is still XPan's value. */
    if (force_style.saved) {
        if (ps_known && ps==0x84 && force_style.v!=0x84) {
            if (m50_force_try(&force_style,0)) { uint32_t v=force_style.v; _prop_request_change(PROP_PICTURE_STYLE,&v,lutp_ps_len); }
            if (force_style.tries<5) return;
        }
        memset(&force_style,0,sizeof(force_style));
    }
}

/* GUI task or the dial filter: only queue the step (no card or property
 * access here). */
static void m50_ctl_step(struct m50_ctl *c, int delta)
{
    if (c->format) { m50_picq_step_format(delta); return; }   /* built, not learned */
    if (c->nopts<2) { c->refused++; return; }
    int recent=get_ms_clock()-c->target_ms<600 && c->target<c->nopts;
    int i=recent ? c->target : m50_opt_find(c,c->cur);
    if (i<0) {                                   /* current state not recorded */
        if (!c->numeric) i=0;
        else { int ev=m50_ev_of(c->cur); i=0; while (i<c->nopts-1 && m50_ev_of(c->opts[i])<ev) i++; }
    } else if (c->numeric) i=MAX(0,MIN(c->nopts-1,i+(delta>0 ? 1:-1)));
    else i=(i+c->nopts+(delta>0 ? 1:-1))%c->nopts;
    c->target=i; c->target_ms=get_ms_clock();
    c->write_req=i+1;
}

/* XPan task: write a queued option, prop by prop, only where Canon's
 * current bytes differ. Not while half-pressed or while Canon is busy. */
static void m50_ctl_write_now(void)
{
    extern void _prop_request_change(unsigned property, const void *addr, size_t len);
    for (unsigned i=0;i<COUNT(m50_ctls);i++) {
        struct m50_ctl *c=&m50_ctls[i];
        int r=c->write_req;
        if (!r) continue;
        c->write_req=0;
        int idx=r-1;
        if (idx>=c->nopts || get_halfshutter_pressed() || lens_info.job_state || RECORDING) { c->refused++; continue; }
        for (int p=0;p<c->nprops;p++) {
            const struct m50_prop_val *v=&c->opts[idx][p];
            if (!v->len || v->len>M50_OPT_BYTES) continue;
            if (m50_val_eq(v,&c->cur[p])) continue;
            uint8_t buf[M50_OPT_BYTES];
            memcpy(buf,v->b,v->len);
            _prop_request_change(c->props[p],buf,v->len);
            c->writes++;
        }
    }
}

/* ML/SETTINGS/XM50OPTS.BIN: "XO1", then per control its property ids and
 * options. Loaded once, kept only if every id and length matches. */
static void m50_opts_save(void)
{
    FILE *f=FIO_CreateFile(M50_OPTS_FILE);
    if (!f) return;
    FIO_WriteFile(f,"XO2",4);
    for (unsigned i=0;i<COUNT(m50_ctls);i++) {
        struct m50_ctl *c=&m50_ctls[i];
        FIO_WriteFile(f,c->props,sizeof(c->props));
        FIO_WriteFile(f,&c->nopts,sizeof(c->nopts));
        FIO_WriteFile(f,c->opts,sizeof(c->opts));
    }
    FIO_CloseFile(f);
}
static void m50_opts_load(void)
{
    m50_opts_loaded=1;
    FILE *f=FIO_OpenFile(M50_OPTS_FILE,O_RDONLY|O_SYNC);
    if (!f) return;
    char magic[4];
    int ok=FIO_ReadFile(f,magic,4)==4 && (!memcmp(magic,"XO1",4) || !memcmp(magic,"XO2",4));
    /* XO1 (first build): 16 options per setting. */
    int max=ok && magic[2]=='1' ? M50_OPT_MAX_V1 : M50_OPT_MAX;
    for (unsigned i=0;ok && i<COUNT(m50_ctls);i++) {
        struct m50_ctl *c=&m50_ctls[i];
        unsigned props[M50_OPT_PROPS]; int n;
        static struct m50_prop_val opts[M50_OPT_MAX][M50_OPT_PROPS];
        int size=max*sizeof(opts[0]);
        if (FIO_ReadFile(f,props,sizeof(props))!=sizeof(props) || memcmp(props,c->props,sizeof(props)) ||
            FIO_ReadFile(f,&n,sizeof(n))!=sizeof(n) || n<0 || n>max ||
            FIO_ReadFile(f,opts,size)!=size) { ok=0; break; }
        for (int k=0;k<n;k++) {
            int reported=0;
            for (int p=0;p<M50_OPT_PROPS;p++) {
                if (opts[k][p].len>M50_OPT_BYTES || (p>=c->nprops && opts[k][p].len)) ok=0;
                if (opts[k][p].len) reported++;
            }
            if (!reported) ok=0;
        }
        if (!ok) break;
        /* Merge: what was learned this session (boot replay) stays. */
        for (int k=0;k<n && c->nopts<M50_OPT_MAX;k++) {
            if (m50_opt_find(c,opts[k])>=0) continue;
            int at=c->nopts;
            if (c->numeric) while (at>0 && m50_ev_of(c->opts[at-1])>m50_ev_of(opts[k])) at--;
            for (int j=c->nopts;j>at;j--) memcpy(c->opts[j],c->opts[j-1],sizeof(c->opts[0]));
            memcpy(c->opts[at],opts[k],sizeof(c->opts[0]));
            c->nopts++;
        }
    }
    FIO_CloseFile(f);
}

/* XPan task: append new reports to ML/LOGS/XM50OPTS.LOG. */
static void m50_rep_save(void)
{
    unsigned head=m50_rep_head, first=m50_rep_saved;
    if (head==first) return;
    if (head-first>M50_REPS) first=head-M50_REPS;
    FILE *f=FIO_OpenFile("ML/LOGS/XM50OPTS.LOG",O_RDWR|O_SYNC);
    if (f) FIO_SeekSkipFile(f,0,SEEK_END);
    else f=FIO_CreateFile("ML/LOGS/XM50OPTS.LOG");
    if (!f) return;
    char line[128];
    for (unsigned i=first;i<head;i++) {
        struct m50_rep r=m50_reps[i%M50_REPS];
        int n;
        if (!r.prop)
            n=snprintf(line,sizeof(line),"%8d learned %s option %d\n",(int)r.ms,
                r.b[0]<COUNT(m50_ctls) ? m50_ctls[r.b[0]].name:"?",(int)r.b[1]);
        else
            n=snprintf(line,sizeof(line),"%8d prop 0x%x len %d: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                (int)r.ms,(unsigned)r.prop,(int)r.len,r.b[0],r.b[1],r.b[2],r.b[3],r.b[4],r.b[5],r.b[6],r.b[7]);
        FIO_WriteFile(f,line,n);
    }
    FIO_CloseFile(f);
    m50_rep_saved=head;
}

static MENU_SELECT_FUNC(opts_forget_select)
{
    (void)priv; (void)delta;
    for (unsigned i=0;i<COUNT(m50_ctls);i++) { m50_ctls[i].nopts=0; m50_ctls[i].dirty=1; }
    m50_opts_dirty=1;
}
static MENU_UPDATE_FUNC(opts_display)
{
    MENU_SET_VALUE("EV %d WB %d DRV %d MTR %d FMT %d",m50_ctls[0].nopts,m50_ctls[1].nopts,
        m50_ctls[2].nopts,m50_ctls[3].nopts,m50_ctls[4].nopts);
    MENU_SET_HELP("Values XPan may write, learned from Canon. Change a setting in Canon's screen to teach it. SET: forget all. Writes %d/%d/%d/%d/%d.",
        m50_ctls[0].writes,m50_ctls[1].writes,m50_ctls[2].writes,m50_ctls[3].writes,m50_ctls[4].writes);
}

/* ------------------------------------------------------------------ */
/* LUT selection (the file names; the LUT itself: Custom LUT, below).   */

/* Same folder, file types, limit and remembered choice as the EOS M
 * (xpan-native-cube.c, not built for the M50): up to 16 .cube/.CUB files
 * in ML/XPAN/LUTS/, sorted by name, selection kept by name hash. */
#define M50_LUT_MAX 16
#define M50_LUT_NAME 48
static char m50_lut_names[M50_LUT_MAX][M50_LUT_NAME];
static volatile int m50_lut_count, m50_lut_index, m50_lut_scanned;
static CONFIG_INT("xpan.cube.name", m50_lut_saved, 0);
static unsigned m50_lut_hash(const char *s)
{
    unsigned h=2166136261u;
    while (*s) h=(h^(unsigned char)*s++)*16777619u;
    return h;
}
static void m50_lut_scan(void)
{
    m50_lut_scanned=1;
    struct card_info *card=get_ml_card();
    if (!card) return;
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/ML/XPAN/LUTS/",card->drive_letter);
    struct fio_file file;
    struct fio_dirent *dir=FIO_FindFirstEx(path,&file);
    if (!dir || IS_ERROR(dir)) return;
    int n=0; unsigned visited=0;
    do {
        if (++visited>256 || n>=M50_LUT_MAX) break;
        unsigned length=strlen(file.name);
        int cube=length>=6 && !strcasecmp(file.name+length-5,".cube");
        int alias=length>=5 && !strcasecmp(file.name+length-4,".cub");
        if ((file.mode&ATTR_DIRECTORY) || length>=M50_LUT_NAME || !(cube || alias) || file.name[0]=='.') continue;
        snprintf(m50_lut_names[n++],M50_LUT_NAME,"%s",file.name);
    } while (!FIO_FindNextEx(dir,&file));
    FIO_FindClose(dir);
    for (int i=0;i<n;i++) for (int j=i+1;j<n;j++)
        if (strcasecmp(m50_lut_names[i],m50_lut_names[j])>0) {
            char t[M50_LUT_NAME];
            memcpy(t,m50_lut_names[i],sizeof(t));
            memcpy(m50_lut_names[i],m50_lut_names[j],sizeof(t));
            memcpy(m50_lut_names[j],t,sizeof(t));
        }
    int index=0;
    for (int i=0;i<n;i++) if (m50_lut_saved && m50_lut_hash(m50_lut_names[i])==(unsigned)m50_lut_saved) index=i;
    m50_lut_index=index;
    m50_lut_count=n;
}
static const char *m50_lut_name(void)
{
    return m50_lut_count>0 ? m50_lut_names[m50_lut_index] : 0;
}
static volatile uint32_t m50_lut_step_ms;
static void m50_lut_step(int delta)
{
    int n=m50_lut_count;
    if (n<1) return;
    m50_lut_index=(m50_lut_index+n+(delta>0 ? 1:-1))%n;
    m50_lut_saved=(int)m50_lut_hash(m50_lut_names[m50_lut_index]);
    m50_lut_step_ms=get_ms_clock();
}

/* ------------------------------------------------------------------ */
/* Custom LUT on the JPEG, step 1: Canon's 10-point colour table       */
/* (README-XPAN-M50.md, "Custom LUT, step 1").                         */

/* XPan's task loads the selected .cube (EOS M parser, xpan-cube.c). At each
 * still develop, the colour-table hook (below) converts it onto the table
 * Canon chose for that shot (xpan-native-table.c, the EOS M conversion; the
 * M50 table has the same layout and format) and gives the develop XPan's
 * table instead. If the LUT is loading, the look is off, or Canon's table
 * fails the format check, the shot keeps Canon's own table. Canon passes
 * tables by their uncached address (0x44032480 in the probe), so XPan's is
 * written through the cache, cleaned to RAM, and passed by its uncached
 * alias. The M50's 17-point grid: m50_grid17, below. */
static CONFIG_INT("xpan.m50.look", m50_look_on, 1);
static CONFIG_INT("xpan.m50.strength", m50_look_strength, 100);   /* percent */
static uint16_t *m50_cube_rgb;
static unsigned m50_cube_cap;
static struct xpan_lut m50_cube;
static volatile int m50_cube_valid, m50_cube_identity;
/* Look lock (review 2026-11-09): DryOS semaphore, count 1, created when the
 * XPan task starts; until then the colour hook passes Canon's table through.
 * Replaces a cli()-guarded flag (cli masks one core only). */
static struct semaphore *m50_look_lock;
static int m50_cube_loaded_index=-1, m50_cube_failed_index=-1;
static char m50_cube_loaded[M50_LUT_NAME];
static char m50_look_status[64]="No LUT loaded yet";
static uint32_t m50_look_table[1024] __attribute__((aligned(64)));
static volatile unsigned m50_look_graded, m50_look_kept;
static volatile uint32_t m50_look_flags, m50_look_us;
/* 17-point grid (README "The 17-point grid"): the M50's colour-table engine
 * takes 17^3 nodes when Canon's loader is given size 0x4CC4 (Canon's own
 * choice in E011E194; Canon's styles always pass 0x1000 = 10 points). The
 * table is 4913 contiguous words (Canon's Opera lists copy it as 4 KB pages);
 * the node order inside is in no Canon table, so the setting names a
 * candidate layout until the camera test finds the right one: 0 = 10-point,
 * 1..4 = XPAN_GRID17_*.
 *
 * Nothing is computed in Canon's develop task (4913 nodes are five times the
 * 10-point work). The hook only compares: it keeps a copy of the shot's
 * Canon table when that changed (m50_src10) and hands Canon a finished
 * 17-point table if one was built for exactly this Canon table, LUT and
 * settings; otherwise the shot gets the 10-point table as before. XPan's
 * task builds the 17-point table ahead (m50_grid17_poll), from the copy,
 * resampled at the 17-point nodes, with the same conversion. So the first
 * photo after start-up or a Picture Style change is 10-point; the log and
 * the menu say which grid each photo got.
 *
 * Two buffers: the task always builds into the one that was not handed to
 * Canon last, so a table Canon's loader may still be reading is never
 * rewritten (a later hand-out means Canon's earlier load has finished).
 * cur / handed / key are read and written under m50_look_lock only. */
static CONFIG_INT("xpan.m50.grid17", m50_grid17, 0);
/* OFF since the camera test of 2026-11-10 (build 59238b91b421): the three
 * 17-point photos (layout A) came out at half brightness, Live View stopped
 * updating and the camera then stopped responding (README "17-point grid:
 * camera test"). With 0 the setting is ignored, the menu entry is gone and
 * the hook always passes 10 points, as in the tested build 8f344d055b96. */
#define M50_GRID17_TEST 0
static uint32_t *volatile m50_look17[2];        /* heap, 64-byte aligned, XPAN_GRID17_WORDS each */
static int m50_look17_cur=-1, m50_look17_handed=1;
static uint32_t m50_look17_key[6];              /* what m50_look17[cur] was built from */
static uint32_t m50_src10[1024];                /* Canon's table of the last shot */
static uint32_t m50_src_sum;                    /* its sum, 0 = none yet (under the lock) */
static volatile unsigned m50_cube_gen;          /* +1 per LUT load */
static volatile unsigned m50_look17_graded, m50_look17_built;
static volatile int m50_look_grid;              /* last shot: 0 Canon's, 10 or 17 */
static volatile uint32_t m50_look17_ms, m50_look17_flags;
static char m50_grid17_status[48]="";
/* Canon's Picture Style as reported (lens_info.picstyle stays 0 on the M50);
 * 0x84 = Neutral, which the LUTs expect. */
static volatile uint32_t lutp_ps_raw;
static volatile unsigned lutp_ps_len;     /* as Canon reports it (forced-settings writer) */
static void lutp_ps_prop(unsigned property, void *token, void *data, unsigned len)
{
    (void)property; (void)token;
    lutp_ps_raw=data && len>=4 ? *(uint32_t *)data : data && len ? *(uint8_t *)data : 0xFFFFFFFF;
    lutp_ps_len=data ? len : 0;
}
REGISTER_PROP_HANDLER_EX(PROP_PICTURE_STYLE, lutp_ps_prop, 0);
#define M50_CUBE_FILE_MAX (4u*1024u*1024u)

/* XPan task. 1 = loaded. */
static int m50_cube_load(void)
{
    struct card_info *card=get_ml_card();
    int index=m50_lut_index;
    if (!card || m50_lut_count<1 || index>=m50_lut_count) return 0;
    char name[M50_LUT_NAME];
    snprintf(name,sizeof(name),"%s",m50_lut_names[index]);
    /* The hook must stop using the old LUT before its buffer changes. */
    if (!m50_look_lock || take_semaphore(m50_look_lock,1000)) { snprintf(m50_look_status,sizeof(m50_look_status),"Busy, retrying"); return 0; }
    m50_cube_valid=0;                           /* under the lock: no user is inside */
    give_semaphore(m50_look_lock);
    if (!m50_cube_rgb) {
        m50_cube_cap=XPAN_CUBE_MAX_COMPONENTS;                 /* 33-point */
        m50_cube_rgb=malloc(m50_cube_cap*sizeof(uint16_t));
        if (!m50_cube_rgb) {
            m50_cube_cap=17u*17u*17u*3u;                        /* 17-point */
            m50_cube_rgb=malloc(m50_cube_cap*sizeof(uint16_t));
        }
        if (!m50_cube_rgb) { snprintf(m50_look_status,sizeof(m50_look_status),"No memory for the LUT"); return 0; }
    }
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/ML/XPAN/LUTS/%s",card->drive_letter,name);
    uint32_t bytes=0;
    if (FIO_GetFileSize(path,&bytes) || !bytes || bytes>M50_CUBE_FILE_MAX) {
        snprintf(m50_look_status,sizeof(m50_look_status),"%s: missing or over 4 MB",name); return 0;
    }
    FILE *f=FIO_OpenFile(path,O_RDONLY|O_SYNC);
    if (!f) { snprintf(m50_look_status,sizeof(m50_look_status),"%s: cannot open",name); return 0; }
    static struct xpan_cube_parser parser;
    static char buffer[2048];
    xpan_cube_init(&parser,m50_cube_rgb,m50_cube_cap);
    uint32_t done=0, started=get_ms_clock();
    int ok=1;
    while (ok && done<bytes) {
        if (!xpan_m50_card_ok() || get_ms_clock()-started>10000) { ok=0; break; }
        unsigned n=MIN((uint32_t)sizeof(buffer),bytes-done);
        if (FIO_ReadFile(f,buffer,n)!=(int)n) { ok=0; break; }
        done+=n;
        if (!xpan_cube_feed(&parser,buffer,n)) ok=0;
    }
    FIO_CloseFile(f);
    if (ok) ok=xpan_cube_finish(&parser);
    if (ok) ok=xpan_lut_init(&m50_cube,m50_cube_rgb,parser.size,parser.components);
    if (!ok) {
        if (parser.error) snprintf(m50_look_status,sizeof(m50_look_status),"%s line %d: %s",
            name,(int)parser.line_number,xpan_cube_error(&parser));
        else snprintf(m50_look_status,sizeof(m50_look_status),"%s: not loaded",name);
        return 0;
    }
    m50_cube_identity=parser.identity;
    snprintf(m50_cube_loaded,sizeof(m50_cube_loaded),"%s",name);
    m50_cube_loaded_index=index;
    snprintf(m50_look_status,sizeof(m50_look_status),"%s: %d-point, %d ms",
        name,(int)m50_cube.size,(int)(get_ms_clock()-started));
    m50_cube_gen++;
    asm volatile("dmb" ::: "memory");
    m50_cube_valid=1;
    return 1;
}
/* Tone base (camera test 2026-11-09: the LUTs on Canon Neutral came out far
 * harsher than on the EOS M). XPan's LUTs are built for Cineon log input, so
 * a base LUT maps Canon's render to Cineon first, composed before the LUT by
 * the same conversion. With it, IDENT shows the Cineon base.
 *   1 Cineon log: ML/XPAN/CINLV.cube, Canon's normal render (the EOS M's
 *     calibration, a first approximation for the M50).
 *   2 Cineon log +1 EV: ML/XPAN/CIN1EV.cube, the M50's own base for Canon's
 *     render with the white balance gains 1 EV lower (highlight headroom,
 *     xpan_m50_hilton below): fitted on the M50 (build_tools/
 *     xpan_m50_cin1ev.py, README "M50 tone base for the headroom").
 * A photo gets CIN1EV only if its gains really were lowered (hil_shot_lowered,
 * set by the pre-develop of the same photo); any other photo gets CINLV. */
static CONFIG_INT("xpan.m50.tone", m50_tone_cineon, 1);   /* 0 Canon Neutral, 1 Cineon, 2 Cineon +1 EV */
struct m50_basefile { const char *name, *alias; uint16_t *rgb; struct xpan_lut lut; volatile int valid; int tried; char status[48]; };
static struct m50_basefile m50_bases[2]={
    { "CINLV.cube",  "CINLV~1.CUB",  0, {0}, 0, 0, "CINLV.cube not loaded" },
    { "CIN1EV.cube", "CIN1EV~1.CUB", 0, {0}, 0, 0, "CIN1EV.cube not loaded" },
};
static volatile int hil_shot_lowered;           /* this photo's gains are 1 EV lower (Hilton hook) */
static volatile unsigned hr_without_base;       /* gains lowered but the photo got no CIN1EV (should stay 0) */
static void m50_base_load(struct m50_basefile *bf)
{
    bf->tried=1;
    struct card_info *card=get_ml_card();
    if (!card) return;
    /* FAT32: Canon sees only the 8.3 alias of a .cube name, e.g. CINLV~1.CUB. */
    const char *names[2]={bf->name,bf->alias};
    char path[FIO_MAX_PATH_LENGTH];
    uint32_t bytes=0;
    for (unsigned i=0;i<COUNT(names);i++) {
        snprintf(path,sizeof(path),"%s:/ML/XPAN/%s",card->drive_letter,names[i]);
        bytes=0;
        if (!FIO_GetFileSize(path,&bytes) && bytes) break;
    }
    if (!bytes || bytes>M50_CUBE_FILE_MAX) { snprintf(bf->status,sizeof(bf->status),"%s missing",bf->name); return; }
    if (!bf->rgb) bf->rgb=malloc(17u*17u*17u*3u*sizeof(uint16_t));
    if (!bf->rgb) { snprintf(bf->status,sizeof(bf->status),"No memory for %s",bf->name); return; }
    FILE *f=FIO_OpenFile(path,O_RDONLY|O_SYNC);
    if (!f) { snprintf(bf->status,sizeof(bf->status),"Cannot open %s",bf->name); return; }
    static struct xpan_cube_parser parser;
    static char buffer[2048];
    xpan_cube_init(&parser,bf->rgb,17u*17u*17u*3u);
    uint32_t done=0; int ok=1;
    while (ok && done<bytes && xpan_m50_card_ok()) {
        unsigned n=MIN((uint32_t)sizeof(buffer),bytes-done);
        if (FIO_ReadFile(f,buffer,n)!=(int)n) { ok=0; break; }
        done+=n;
        if (!xpan_cube_feed(&parser,buffer,n)) ok=0;
    }
    FIO_CloseFile(f);
    if (ok && done==bytes) ok=xpan_cube_finish(&parser); else ok=0;
    if (ok) ok=xpan_lut_init(&bf->lut,bf->rgb,parser.size,parser.components);
    if (!ok) { snprintf(bf->status,sizeof(bf->status),"%s invalid (line %d)",bf->name,(int)parser.line_number); return; }
    snprintf(bf->status,sizeof(bf->status),"%s %d-point",bf->name,(int)bf->lut.size);
    asm volatile("dmb" ::: "memory");
    bf->valid=1;
}
/* XPan task, every 50 ms: load the selected LUT once the dial has stopped. */
static void m50_look_poll(void)
{
    if (m50_look_on && m50_tone_cineon && !m50_bases[0].tried && m50_lut_scanned) m50_base_load(&m50_bases[0]);
    if (m50_look_on && m50_tone_cineon==2 && !m50_bases[1].tried && m50_lut_scanned) m50_base_load(&m50_bases[1]);
    if (!m50_look_on || !m50_lut_scanned || m50_lut_count<1 || get_halfshutter_pressed()) return;
    int index=m50_lut_index;
    if ((index==m50_cube_loaded_index && m50_cube_valid) || index==m50_cube_failed_index) return;
    if (get_ms_clock()-m50_lut_step_ms<700) return;
    xpan_photo_hud_force_redraw();
    if (m50_cube_load()) m50_cube_failed_index=-1;
    else m50_cube_failed_index=index;
    xpan_photo_hud_force_redraw();
}
static uint32_t m50_table_sum(const volatile uint32_t *t)
{
    uint32_t s=0;
    for (unsigned i=0;i<1024;i++) s=(s<<5|s>>27)^t[i];
    return s ? s : 1;                           /* 0 means "no table yet" */
}
static void m50_grid17_key(uint32_t key[6], uint32_t sum, const struct xpan_lut *base, int layout)
{
    key[0]=sum; key[1]=m50_cube_gen; key[2]=(unsigned)COERCE(m50_look_strength,0,100)*65535u/100;
    key[3]=(uint32_t)base; key[4]=(uint32_t)m50_cube_identity; key[5]=(uint32_t)layout;
}
/* XPan task, every 50 ms: build the 17-point table for the last shot's Canon
 * table and the current LUT and settings, when they changed. */
static void m50_grid17_poll(void)
{
    int layout=M50_GRID17_TEST ? m50_grid17 : 0;
    if (layout<XPAN_GRID17_RGB || layout>XPAN_GRID17_PARITY) return;
    /* Tables from the heap at first use (the hook never allocates); room for
     * the alignment and whole cache lines. */
    static int no_memory;
    static uint32_t failed_key[6];              /* inputs that could not be converted: not retried */
    static int failed;
    if (no_memory) return;
    if (!m50_look17[1]) {
        for (int k=0;k<2;k++) {
            uint8_t *raw=malloc(XPAN_GRID17_WORDS*4u+128u);
            if (!raw) { no_memory=1; snprintf(m50_grid17_status,sizeof(m50_grid17_status),"No memory for the 17-point tables."); return; }
            memset(raw,0,XPAN_GRID17_WORDS*4u+128u);
            m50_look17[k]=(uint32_t *)(((uint32_t)raw+63u)&~63u);
        }
    }
    if (!m50_look_on || !m50_cube_valid || live_applied || get_halfshutter_pressed() || lens_info.job_state) return;
    const struct xpan_lut *base=m50_tone_cineon ? (m50_bases[0].valid ? &m50_bases[0].lut : 0) : 0;
    if (m50_tone_cineon && !base) return;
    static uint32_t src[1024];                  /* the task's own copy: built outside the lock */
    uint32_t key[6];
    if (!m50_look_lock || take_semaphore(m50_look_lock,200)) return;     /* 0 = taken */
    if (!m50_src_sum) { give_semaphore(m50_look_lock); snprintf(m50_grid17_status,sizeof(m50_grid17_status),"Waiting for a first photo."); return; }
    m50_grid17_key(key,m50_src_sum,base,layout);
    if ((m50_look17_cur>=0 && !memcmp(key,m50_look17_key,sizeof(key))) || (failed && !memcmp(key,failed_key,sizeof(key)))) {
        give_semaphore(m50_look_lock); return;
    }
    int target=1-m50_look17_handed;             /* never the table Canon had last */
    if (m50_look17_cur==target) m50_look17_cur=-1;
    memcpy(src,m50_src10,sizeof(src));
    give_semaphore(m50_look_lock);
    /* The LUT and base buffers change only in this task (m50_cube_load). */
    uint32_t start=get_ms_clock();
    struct xpan_native_cube_stats stats;
    uint32_t flags=0;
    uint32_t *t=m50_look17[target];
    int ok=xpan_native_table_prepare_cube17(t,src,&m50_cube,key[2],m50_cube_identity,base,(unsigned)layout,&stats,&flags);
    if (ok) dcache_clean((uint32_t)t,XPAN_GRID17_BYTES);
    m50_look17_ms=get_ms_clock()-start; m50_look17_flags=flags;
    if (!ok) {
        snprintf(m50_grid17_status,sizeof(m50_grid17_status),"Not built (flags %x): 10-point.",(unsigned)flags);
        memcpy(failed_key,key,sizeof(key)); failed=1;
        return;
    }
    if (take_semaphore(m50_look_lock,200)) return;                       /* next poll builds again */
    memcpy(m50_look17_key,key,sizeof(key));
    m50_look17_cur=target;
    give_semaphore(m50_look_lock);
    m50_look17_built++;
    snprintf(m50_grid17_status,sizeof(m50_grid17_status),"Built in %d ms.",(int)m50_look17_ms);
}
/* Colour-table hook: Canon's table for this shot in, the one to use out,
 * with the size Canon's loader gets for it (0x1000 = 10 points; 0x4CC4 = 17
 * points, only ever with XPan's own finished 17-point table). */
static void *m50_look_apply(void *table, int *size)
{
    *size=0x1000;
    uint32_t t=(uint32_t)table;
    int readable=(t>=0x1000 && t<0x80000000) || (t>=0xE0000000 && t<0xE2000000);
    /* Cineon tone base: the LUTs expect Cineon log input. Without its base
     * file the shot keeps Canon's table (a LUT on Neutral input is harsh).
     * A photo whose gains were lowered by its pre-develop gets the base made
     * for that render (the Hilton hook lowers them only when it is loaded). */
    int lowered=hil_shot_lowered;
    hil_shot_lowered=0;                         /* one photo */
    const struct xpan_lut *base=lowered ? &m50_bases[1].lut :
        m50_tone_cineon ? (m50_bases[0].valid ? &m50_bases[0].lut : 0) : 0;
    /* Live LUT preview grades Canon's style library: this table is graded already. */
    int go=0;
    if (m50_look_lock && !take_semaphore_now(m50_look_lock)) {
        go=m50_look_on && m50_cube_valid && readable && (!m50_tone_cineon || base) && !live_applied
           && xpan_photo_enabled() && !is_movie_mode();   /* XPan off: Canon's colour */
        if (!go) give_semaphore(m50_look_lock);
    }
    if (!go) { m50_look_kept++; m50_look_grid=0; if (lowered) hr_without_base++; return table; }
    uint32_t start=get_us_clock();
    struct xpan_native_cube_stats stats;
    uint32_t flags=0;
    void *use=table;
    int layout=M50_GRID17_TEST ? m50_grid17 : 0;
    if (layout>=XPAN_GRID17_RGB && layout<=XPAN_GRID17_PARITY) {
        /* Compare only: the 17-point table is built ahead by XPan's task. */
        uint32_t key[6], sum=m50_table_sum((const volatile uint32_t *)table);
        m50_grid17_key(key,sum,base,layout);
        if (m50_look17_cur>=0 && !memcmp(key,m50_look17_key,sizeof(key))) {
            m50_look17_handed=m50_look17_cur;
            use=UNCACHEABLE(m50_look17[m50_look17_cur]); *size=XPAN_GRID17_BYTES;
            m50_look17_graded++; m50_look_graded++; m50_look_grid=17;
        } else if (sum!=m50_src_sum) {
            memcpy(m50_src10,table,sizeof(m50_src10));     /* for the task: next photo */
            m50_src_sum=sum;
        }
    }
    if (use!=table) ;                           /* XPan's 17-point table goes to Canon */
    else if (xpan_native_table_prepare_cube(m50_look_table,(const uint32_t *)table,&m50_cube,(unsigned)COERCE(m50_look_strength,0,100)*65535u/100,
            m50_cube_identity,base,&stats,&flags)) {
        dcache_clean((uint32_t)m50_look_table,sizeof(m50_look_table));
        use=UNCACHEABLE(m50_look_table);
        m50_look_graded++; m50_look_grid=10;
    } else { m50_look_kept++; m50_look_grid=0; if (lowered) hr_without_base++; }
    m50_look_flags=flags;
    m50_look_us=get_us_clock()-start;
    give_semaphore(m50_look_lock);
    return use;
}
/* HUD state under the LUT name. */
/* The small line under the HUD's "LUT" label (40 px): the strength, "..."
 * while the LUT loads, "!" when it cannot grade (Custom LUT's help says
 * why), OFF. */
const char *m50_look_hud_state(void)
{
    static char text[8];
    if (!m50_look_on) return "OFF";
    if (m50_lut_index!=m50_cube_loaded_index || !m50_cube_valid)
        return m50_lut_index==m50_cube_failed_index ? "!" : "...";
    if (m50_tone_cineon && !m50_bases[0].valid) return "!";
    snprintf(text,sizeof(text),"%d%%",(int)COERCE(m50_look_strength,0,100));
    return text;
}
/* Custom LUT submenu (PLAY opens it, as on the EOS M; SET is Canon's Quick
 * menu on the M50). */
static MENU_SELECT_FUNC(cube_file_select) { (void)priv; m50_lut_step(delta); xpan_photo_hud_force_redraw(); }
static MENU_UPDATE_FUNC(cube_file_display)
{
    const char *n=m50_lut_name();
    MENU_SET_VALUE("%s",n ? n : "No .cube files");
    MENU_SET_HELP("LEFT/RIGHT: LUT from ML/XPAN/LUTS (same as the HUD's LUT field). %s.",m50_look_status);
    if (!n) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"No .cube files in ML/XPAN/LUTS (up to 16).");
}
static MENU_SELECT_FUNC(cube_refresh_select)
{
    (void)priv; (void)delta;
    m50_cube_loaded_index=m50_cube_failed_index=-1;     /* reload the selected file too */
    m50_lut_scanned=0;                                  /* XPan task rescans */
    NotifyBox(1500,"Refreshing the LUT list");
}
static MENU_UPDATE_FUNC(cube_refresh_display)
{
    MENU_SET_VALUE("%d files",(int)m50_lut_count);
    MENU_SET_HELP("LEFT/RIGHT: rescan ML/XPAN/LUTS and reload the selected file.");
}
static MENU_UPDATE_FUNC(strength_display)
{
    MENU_SET_VALUE("%d%%",(int)m50_look_strength);
    MENU_SET_HELP("Blend of the LUT over the tone base, 0..100%%.");
}
static char live_status[64];
static int m50_live_on;         /* Live LUT preview (CONFIG_INT below) */
static MENU_SELECT_FUNC(live_lut_select) { (void)priv; (void)delta; m50_live_on=!m50_live_on; }
static MENU_UPDATE_FUNC(live_lut_display)
{
    MENU_SET_VALUE("%s",m50_live_on ? "ON":"OFF");
    MENU_SET_ENABLED(m50_live_on);
    MENU_SET_HELP("Experimental: the LUT also in Live View. %s.",live_status);
}
static volatile unsigned hil_lowered, hil_refused;   /* Hilton hook, below */
/* Tone base: ML's choice list (.choices), so the picker shows the names. */
static MENU_UPDATE_FUNC(tone_display)
{
    int t=COERCE(m50_tone_cineon,0,2);
    MENU_SET_ENABLED(1);
    if (t==2) {
        MENU_SET_HELP("+1 EV highlight headroom: Canon's gains 1 EV lower on the RAW data, then %s. %s.%s",
            m50_bases[1].name,m50_bases[1].status,hr_without_base ? " Some photos had no base: see the log." : "");
    } else MENU_SET_HELP("Cineon log: Canon's render mapped to Cineon (ML/XPAN/CINLV.cube) before the LUT, as XPan's LUTs expect. %s.",
        m50_bases[0].status);
    if (t && !m50_bases[0].valid) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"%s: photos keep Canon's colours.",m50_bases[0].status);
    else if (t==2 && !m50_bases[1].valid && m50_bases[1].tried) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"%s: photos without headroom.",m50_bases[1].status);
    else if (t==2 && m50_live_on) MENU_SET_WARNING(MENU_WARN_ADVICE,"Live LUT preview is ON: photos come from the library, without headroom.");
}
static MENU_SELECT_FUNC(grid17_select)
{
    (void)priv;
    m50_grid17=(COERCE(m50_grid17,0,4)+5+(delta>0 ? 1:-1))%5;
}
static MENU_UPDATE_FUNC(grid17_display)
{
    static const char *names[5]={"10-point (Canon)","17-point test A","17-point test B","17-point test C","17-point test D"};
    int g=COERCE(m50_grid17,0,4);
    MENU_SET_VALUE("%s",names[g]);
    MENU_SET_RINFO("%d photos",(int)m50_look17_graded);
    MENU_SET_HELP("Test: 17-point layouts A-D; the right one looks like 10-point. Last photo: %s. %s First photo after a change may be 10-point.",
        m50_look_grid==17 ? "17-point" : m50_look_grid==10 ? "10-point" : "Canon's table",m50_grid17_status);
    if (g && m50_live_on) MENU_SET_WARNING(MENU_WARN_ADVICE,"Live LUT preview is ON: photos use the 10-point library tables. Turn it OFF.");
}
static MENU_SELECT_FUNC(look_select) { (void)priv; (void)delta; m50_look_on=!m50_look_on; }
static MENU_UPDATE_FUNC(look_display)
{
    MENU_SET_VALUE("%s",m50_look_on ? "ON":"OFF");
    MENU_SET_ENABLED(m50_look_on);              /* no priv: ML would draw it OFF */
    MENU_SET_HELP("The selected LUT on the saved JPEG. %s.",m50_look_status);
    if (m50_look_on && lutp_ps_raw && lutp_ps_raw!=0x84)
        MENU_SET_WARNING(MENU_WARN_ADVICE,"Set Canon's Picture Style to Neutral: the LUTs expect it.");
}


/* Dynamic lens data fires on every focus step: log only aperture/focal changes. */
static void probe_lens_dynamic(unsigned property, void *token, void *data, unsigned len)
{
    (void)token;
    dyn_count++;
    if (len<sizeof(struct prop_lens_dynamic_data)) { expo_add(property,len,0,0,0,0); return; }
    const struct prop_lens_dynamic_data *d=data;
    if (d->AVEF==last_avef && d->AVO==last_avo && d->AVMAX==last_avmax && d->FL==last_fl) return;
    last_avef=d->AVEF; last_avo=d->AVO; last_avmax=d->AVMAX; last_fl=d->FL;
    expo_add(property,len,d->AVEF,d->AVO,d->AVMAX,d->FL);
}
REGISTER_PROP_HANDLER_EX(PROP_LENS_DYNAMIC_DATA, probe_lens_dynamic, 0);

static volatile int expo_save_request;
static const char *expo_status="";

static int expo_save(void)
{
    FILE *f=FIO_OpenFile("ML/LOGS/XM50EXPO.LOG",O_RDWR|O_SYNC);
    if (f) FIO_SeekSkipFile(f,0,SEEK_END);
    else f=FIO_CreateFile("ML/LOGS/XM50EXPO.LOG");
    if (!f) return 0;
    char line[160];
    unsigned head=expo_head,first=expo_saved;
    if (head-first>EXPO_RECS) first=head-EXPO_RECS;
    int n=snprintf(line,sizeof(line),"--- save at %d ms: lens '%s' exists %d focal %d; HUD raw Tv %d Av %d aperture %d; mode %d\n",
        (int)get_ms_clock(),lens_info.name,lens_info.lens_exists,lens_info.focal_len,
        lens_info.raw_shutter,lens_info.raw_aperture,lens_info.aperture,shooting_mode);
    FIO_WriteFile(f,line,n);
    for (unsigned i=first;i<head;i++) {
        const struct expo_rec *r=&expo_ring[i%EXPO_RECS];
        const char *name=r->prop==PROP_SHUTTER ? "TV     " : r->prop==PROP_APERTURE ? "AV     " :
            r->prop==PROP_SHUTTER_AUTO ? "TV_AUTO" : r->prop==PROP_APERTURE_AUTO ? "AV_AUTO" :
            r->prop==PROP_ISO ? "ISO    " : r->prop==PROP_ISO_AUTO ? "ISOAUTO" : "LENSDYN";
        if (r->prop==PROP_ISO || r->prop==PROP_ISO_AUTO)
            n=snprintf(line,sizeof(line),"%8d %s len %d raw 0x%x  [ML raw ISO %d = %d] mode %d\n",
                (int)r->ms,name,(int)r->len,(unsigned)r->a,(int)r->b,(int)r->c,r->mode);
        else if (r->prop==PROP_LENS_DYNAMIC_DATA)
            /* ML's snprintf has no %u: %d and %x only. */
            n=snprintf(line,sizeof(line),"%8d %s len %d AVEF 0x%x AVO 0x%x AVMAX 0x%x FL %d mode %d\n",
                (int)r->ms,name,(int)r->len,(unsigned)r->a,(unsigned)r->b,(unsigned)r->c,(int)r->d,r->mode);
        else
            n=snprintf(line,sizeof(line),"%8d %s len %d raw 0x%x  [ML raw Tv %d Av %d aperture %d] mode %d\n",
                (int)r->ms,name,(int)r->len,(unsigned)r->a,(int)r->b,(int)r->c,(int)r->d,r->mode);
        FIO_WriteFile(f,line,n);
    }
    FIO_CloseFile(f);
    expo_saved=head;
    return 1;
}

static MENU_SELECT_FUNC(expo_select) { (void)priv; (void)delta; expo_save_request=1; }
static MENU_UPDATE_FUNC(expo_display)
{
    MENU_SET_VALUE("Tv %d Av %d AVEF %d",(int)last_tv,(int)last_av,(int)last_avef);
    MENU_SET_HELP("%s%d new records. Tv/Av auto %d/%d, AVO %d AVMAX %d, lens data %d calls. SET: save log.",
        expo_status,expo_head-expo_saved,(int)last_tv_auto,(int)last_av_auto,(int)last_avo,(int)last_avmax,dyn_count);
}

/* ------------------------------------------------------------------ */
/* Touch position probe (for touch-to-select on the HUD).              */

/* Each M50 touch event (0x7F down, 0x80 up) carries event->obj = a small
 * record [0, pointer, 0x55555555] (key log). Where the x/y position is, is
 * not known yet: keep the last 32 events with the record and the 8 words the
 * pointer points to. XPan > Touch probe: SET saves ML/LOGS/XTOUCH.LOG. */
struct touch_rec { uint32_t ms, param, arg, obj, rec[3], data[8]; int ok; };
#define TOUCH_RECS 32
static struct touch_rec touch_ring[TOUCH_RECS];
static volatile unsigned touch_head, touch_saved;
static volatile int touch_save_request;
static const char *touch_status="";

static int ram_ptr_ok(uint32_t p) { return p>=0x1000 && p<0x40000000 && !(p&3); }

static void touch_record(struct event *event)
{
    struct touch_rec *r=&touch_ring[touch_head%TOUCH_RECS];
    memset(r,0,sizeof(*r));
    r->ms=get_ms_clock(); r->param=event->param; r->arg=event->arg; r->obj=(uint32_t)event->obj;
    if (ram_ptr_ok(r->obj)) {
        const uint32_t *o=(const uint32_t *)r->obj;
        for (int i=0;i<3;i++) r->rec[i]=o[i];
        if (ram_ptr_ok(r->rec[1])) {
            const uint32_t *d=(const uint32_t *)r->rec[1];
            for (int i=0;i<8;i++) r->data[i]=d[i];
            r->ok=1;
        }
    }
    touch_head++;
}

static int touch_save(void)
{
    FILE *f=FIO_OpenFile("ML/LOGS/XTOUCH.LOG",O_RDWR|O_SYNC);
    if (f) FIO_SeekSkipFile(f,0,SEEK_END);
    else f=FIO_CreateFile("ML/LOGS/XTOUCH.LOG");
    if (!f) return 0;
    char line[200];
    unsigned head=touch_head,first=touch_saved;
    if (head-first>TOUCH_RECS) first=head-TOUCH_RECS;
    int n=snprintf(line,sizeof(line),"--- save at %d ms, %d events\n",(int)get_ms_clock(),(int)(head-first));
    FIO_WriteFile(f,line,n);
    for (unsigned i=first;i<head;i++) {
        const struct touch_rec *r=&touch_ring[i%TOUCH_RECS];
        n=snprintf(line,sizeof(line),"%8d %s arg %x obj %x [%x %x %x] data %x %x %x %x %x %x %x %x%s\n",
            (int)r->ms,r->param==0x7F ? "DOWN":"UP  ",(unsigned)r->arg,(unsigned)r->obj,
            (unsigned)r->rec[0],(unsigned)r->rec[1],(unsigned)r->rec[2],
            (unsigned)r->data[0],(unsigned)r->data[1],(unsigned)r->data[2],(unsigned)r->data[3],
            (unsigned)r->data[4],(unsigned)r->data[5],(unsigned)r->data[6],(unsigned)r->data[7],
            r->ok ? "":" (no data)");
        FIO_WriteFile(f,line,n);
    }
    FIO_CloseFile(f);
    touch_saved=head;
    return 1;
}

static MENU_SELECT_FUNC(touch_probe_select) { (void)priv; (void)delta; touch_save_request=1; }
static MENU_UPDATE_FUNC(touch_probe_display)
{
    MENU_SET_VALUE("%d new taps",(int)((touch_head-touch_saved)/2));
    MENU_SET_HELP("%sTap the 4 corners, then the centre, in XPan Live View; SET saves ML/LOGS/XTOUCH.LOG.",touch_status);
}

int xpan_m50_card_ok(void);     /* power-off guard, below */
static void m50_log_poll(void);  /* XPan log (ML/LOGS/XM50.LOG), below */
static void m50_live_poll(void); /* Live LUT preview, below */
static void m50_grain_poll(void); /* Film grain, below */

/* 65:24 JPEG (README "65:24 JPEG via Canon's 3:2 size"): while XPan's 65:24
 * frame is on, Canon's own 3:2 L size is 6000x2216, centred (rows 892-3108),
 * so Canon itself writes the 65:24 JPEG (header, EXIF, thumbnail, RAW crop
 * record) in the normal and the creative-filter (grain) path. Two Canon
 * values, through ML's patch API (old values checked, undone when 65:24 is
 * off): the live size table entry (RAM, via 0xB4D4, +0x4C: 4000 -> 2216; read
 * by Canon's size getter E0081E4E) and the 3:2 L encoder table (ROM
 * E0FCFF88, MMU page: w2 top 0 -> 892, w10 rows/8-1 499 -> 276). Only L 3:2
 * is changed (XPan needs Canon at 3:2, L). From XPan's task, not while
 * half-pressed, busy or recording. */
#define CROP_FULL_H 4000u
#define CROP_H 2216u
#define CROP_TOP ((CROP_FULL_H-CROP_H)/2u)      /* 892: centred */
#define ENC_L32 0xE0FCFF88u                    /* encoder table, aspect 0 (3:2), size L */
/* The save module's (FCS) own copy of the same table (README "The AF
 * block's image size"): fcsActivate copies PROP_DSDEFINE to the FCS object
 * + 0x20 at start, before the crop, and Canon's metadata writers (EXIF size,
 * AF block, maker note) read that copy. Patched with the others when it is
 * found as expected (starts with "Canon", 6000 x 4000 and 6000 x 3368 at
 * +0x48 / +0x58); otherwise the crop goes on without it. */
#define FCS_OBJECT_PTR 0x538Cu
static struct patch crop_patch[4];
static volatile int crop_on, crop_err, crop_refused, crop_fcs;   /* crop_fcs: 1 patched, -1 not found */
static volatile unsigned crop_applies, crop_removes;
static uint32_t crop_size_entry(void)           /* cached address of Canon's 3:2 L width, 0 if unexpected */
{
    uint32_t c=(uint32_t)CACHEABLE(*(const volatile uint32_t *)0xB4D4);
    if (c<0x1000 || c+0x60>=0x40000000u || (c&3)) return 0;
    const volatile uint32_t *t=(const volatile uint32_t *)UNCACHEABLE(c);
    if (t[0x48/4]!=6000 || t[0x4C/4]!=CROP_FULL_H || t[0x58/4]!=6000 || t[0x5C/4]!=3368) return 0;
    return c+0x48;
}
static uint32_t fcs_size_entry(void)            /* cached address of the FCS copy's 3:2 L width, 0 if unexpected */
{
    uint32_t o=(uint32_t)CACHEABLE(*(const volatile uint32_t *)FCS_OBJECT_PTR);
    if (o<0x1000 || o+0x20+0x600>=0x40000000u || (o&3)) return 0;
    uint32_t c=o+0x20;
    const volatile uint8_t *name=(const volatile uint8_t *)UNCACHEABLE(c);
    const volatile uint32_t *t=(const volatile uint32_t *)UNCACHEABLE(c);
    if (name[0]!='C' || name[1]!='a' || name[2]!='n' || name[3]!='o' || name[4]!='n') return 0;
    if (t[0x48/4]!=6000 || t[0x4C/4]!=CROP_FULL_H || t[0x58/4]!=6000 || t[0x5C/4]!=3368) return 0;
    return c+0x48;
}
static void crop_undo(void)
{
    for (int i=0;i<4;i++) if (crop_patch[i].addr) unpatch_memory((uintptr_t)crop_patch[i].addr);
}
static void m50_crop_poll(void)
{
    int want=xpan_photo_enabled() && xpan_photo_is_panoramic();
    if (!want) crop_refused=0;
    if (want==crop_on || (want && crop_refused)) return;
    if (get_halfshutter_pressed() || lens_info.job_state || RECORDING) return;
    if (want) {
        uint32_t e=crop_size_entry();
        const volatile uint32_t *enc=(const volatile uint32_t *)ENC_L32;
        if (!e || enc[0]!=6000 || enc[1]!=0 || enc[2]!=0 || enc[10]!=(CROP_FULL_H/8u-1u)) { crop_err=-100; crop_refused=1; return; }
        crop_patch[0]=(struct patch){ .addr=(uint8_t *)(e+4), .old_value=CROP_FULL_H, .new_value=CROP_H, .size=4,
                                      .description="XPan 65:24 JPEG: Canon 3:2 L height" };
        crop_patch[1]=(struct patch){ .addr=(uint8_t *)(ENC_L32+8), .old_value=0, .new_value=CROP_TOP, .size=4,
                                      .description="XPan 65:24 JPEG: encoder top crop" };
        crop_patch[2]=(struct patch){ .addr=(uint8_t *)(ENC_L32+40), .old_value=CROP_FULL_H/8u-1u, .new_value=CROP_H/8u-1u, .size=4,
                                      .description="XPan 65:24 JPEG: encoder rows" };
        uint32_t f=fcs_size_entry();
        int n=3;
        crop_patch[3]=(struct patch){ 0 };
        if (f) {
            crop_patch[3]=(struct patch){ .addr=(uint8_t *)(f+4), .old_value=CROP_FULL_H, .new_value=CROP_H, .size=4,
                                          .description="XPan 65:24 JPEG: save module's 3:2 L height" };
            n=4;
        }
        crop_fcs=f ? 1:-1;
        crop_err=apply_patches(crop_patch,n);
        if (crop_err) { crop_undo(); crop_refused=1; return; }
        crop_on=1; crop_applies++;
    } else {
        crop_undo();
        crop_on=0; crop_removes++;
    }
}
static void grain_worker_start(void);
static void band_restart_step(void);      /* forced Live View restart, below */
static void canon_dump_step(void);        /* Canon's debug log at the menu hang, below */
static int canon_dump_stuck_off(void);    /* ... and at a stuck power-off, below */
static void m50_log_tasks(void);          /* task states into Canon's log, below */
/* On again (README "Forced restart through Canon's MENU button path"):
 * the freezes came from ML's patch stalls, now fixed; the restart presses
 * Canon's MENU button the way the user does. */
#define M50_LV_RESTART 1

static void xpan_m50_task(void *unused)
{
    m50_look_lock=create_named_semaphore("xpan_look_lock",1);
    grain_worker_start();       /* Film grain worker task (test 6) */
    (void)unused;
    /* ISO reader: start once the camera has settled after boot. */
    uint32_t start=get_ms_clock();
    int iso_checked=0;
    while (!ml_shutdown_requested) {
        msleep(50);         /* ISO writes: quick enough for the dial */
        /* Powering off: no property writes, no card access by XPan (only
         * Canon's own log writer if the power-off is stuck, Debug info). */
        if (!xpan_m50_card_ok()) { canon_dump_stuck_off(); continue; }
        m50_iso_write_now();
        m50_expo_write_now();
        /* Learned settings (EV, WB, drive, metering, format) and LUT list. */
        if (!m50_opts_loaded && get_ms_clock()-start>3000) m50_opts_load();
        m50_ctl_learn();
        m50_ctl_write_now();
        m50_picq_write_now();
        if (get_ms_clock()-start>3000) { static uint32_t force_ms; if (get_ms_clock()-force_ms>=200) { force_ms=get_ms_clock(); m50_force_step(); } }
        if (get_ms_clock()-start>3000) m50_cgrain_write_now();
        if (get_ms_clock()-start>3000) m50_crop_poll();
        static uint32_t opts_save_ms;
        if (m50_opts_dirty && m50_opts_loaded && get_ms_clock()-opts_save_ms>2000 && NOT_RECORDING) {
            m50_opts_dirty=0; opts_save_ms=get_ms_clock();
            m50_opts_save();
        }
        static uint32_t rep_save_ms;
        if (m50_dbg() && m50_opts_loaded && get_ms_clock()-rep_save_ms>2000 && !get_halfshutter_pressed() && NOT_RECORDING) {
            rep_save_ms=get_ms_clock();
            m50_rep_save();
        }
        if (!m50_lut_scanned && get_ms_clock()-start>6000) { m50_lut_scan(); xpan_photo_hud_force_redraw(); }
        m50_log_poll();
        m50_look_poll();
        m50_grid17_poll();
        m50_live_poll();
        m50_grain_poll();
#if M50_LV_RESTART
        if (get_ms_clock()-start>3000) band_restart_step();
#endif
        canon_dump_step();
        if (touch_save_request) {
            touch_save_request=0;
            if (touch_save()) { touch_status="Saved. "; NotifyBox(2000,"Saved ML/LOGS/XTOUCH.LOG"); }
            else { touch_status="Save failed. "; NotifyBox(2000,"Could not write the log"); }
        }
        if (!iso_checked && get_ms_clock()-start>5000) { iso_checked=1; iso_reader_register(); }
        /* Settings: saved within 2 s of a change, never at power-off
         * (tasks.c ml_shutdown, config.c config_save_if_changed). */
        static uint32_t config_check_ms;
        if (get_ms_clock()-config_check_ms>=2000 && !gui_menu_shown() && NOT_RECORDING) {
            extern int config_save_if_changed(void);
            config_check_ms=get_ms_clock();
            config_save_if_changed();
        }
        if (expo_save_request) {
            expo_save_request=0;
            if (expo_save()) { expo_status="Saved. "; NotifyBox(2000,"Saved ML/LOGS/XM50EXPO.LOG"); }
            else { expo_status="Save failed. "; NotifyBox(2000,"Could not write the log"); }
        }
    }
    /* ML's power-off: a normal one cuts the power long before this ends. */
    while (m50_dbg() && !canon_dump_stuck_off()) msleep(100);
}
TASK_CREATE("xpan_m50", xpan_m50_task, 0, 0x1e, 0x1000);

/* ------------------------------------------------------------------ */
/* Boot log.                                                           */

/* 2026-11-08: the camera hangs (LED solid) when it is switched on with the
 * shutter at 1" (at 1/60 it boots; changing to 1" while running is fine).
 * Each startup stage and the first calls of the code that sees exposure
 * values leave a mark in RAM (cheap, any task, interrupts off for a few
 * instructions); a separate low-priority task appends them to
 * ML/LOGS/XBOOT.LOG every 200 ms for the first 60 s. If something
 * deadlocks, the log ends at the last mark that was reached. */
#define BOOT_MARKS 768     /* XBOOT.LOG 2026-11-09: up to 296 marks per start; + the Canon menu diagnostics (70-76) */
struct boot_mark { uint32_t ms; int id, value; const char *task; };
static struct boot_mark boot_marks[BOOT_MARKS];
static volatile unsigned boot_mark_head, boot_gui_marks;

/* Set by the shutdown properties below. From then on XPan does not touch the
 * card (boot log, init.c skips the rest of ML's startup): 2026-11-08, after
 * a power-off the camera sometimes did not respond again after the card was
 * taken out and put back (battery pull needed). The last boot log of that
 * kind stopped between "init funcs done" and "config loaded", i.e. Canon's
 * short start after closing the card/battery door shut down while ML was
 * still starting and the boot log task was writing to the card. */
volatile int m50_shutdown_seen;

void m50_boot_mark(int id, int value)
{
    if (ml_started && !m50_dbg()) return;
    /* Before ML starts Canon sends hundreds of GUI events (0x40000 every few
     * ms); 48 are enough to see the start-up pattern and leave room for the
     * marks that matter. */
    if (id==30 && ++boot_gui_marks>48) return;
    uint32_t old=cli();
    unsigned i=boot_mark_head;
    if (i<BOOT_MARKS) {
        boot_marks[i].ms=get_ms_clock(); boot_marks[i].id=id; boot_marks[i].value=value;
        boot_marks[i].task=get_current_task_name();
        boot_mark_head=i+1;
    }
    sei(old);
}

static const char *boot_mark_name(int id)
{
    switch (id) {
        case 1: return "init: card found";
        case 2: return "init: fonts loaded";
        case 3: return "init: menu_init done";
        case 4: return "init: init funcs done";
        case 5: return "init: config loaded";
        case 6: return "init: done";
        case 10: return "PROP_SHUTTER (ML raw)";
        case 18: return "PROP_SHOOTING_MODE_2 (mode)";

        case 13: return "PROP_APERTURE (ML raw)";
        case 20: return "shoot_task loop (iteration)";
        case 21: return "HUD render in";
        case 22: return "HUD render out";
        case 23: return "XPan controls step in";
        case 24: return "XPan controls step out";
        case 25: return "LV info display in";
        case 26: return "LV info display out";
        case 30: return "GUI event before ML start (type<<16 | code)";
        case 40: return "PROP_TERMINATE_SHUT_REQ";
        case 41: return "PROP_ABORT (battery/card door)";
        case 42: return "PROP_SHUTDOWN_REASON";
        case 59: return "core 1 start entry at release (0x20101231 = DryOS marker)";
        case 60: return "core 1 released (1 = yes)";
        case 61: return "MMU apply_normal_patches (0 = ok)";
        case 62: return "E00E73BC first word (0x41fce92d = Canon's, not patched)";
        /* Canon menu diagnostics (README "Canon's main menu breaks Live View") */
        case 70: return "UI: gui_state|lv<<8|MLmenu<<9|frozen<<10|uifrozen<<11|layout<<12|view<<14";
        case 71: return "XPan dims for Canon's UI (count)";
        case 72: return "XPan back from Canon's UI";
        case 73: return "button / GUI event (param)";
        case 74: return "panel: a2|a3<<4|moved<<7|Canon srcY<<8|h<<20";
        case 75: return "EVF: a2|a3<<4|moved<<7|Canon srcY<<8|h<<20";
        case 76: return "setter gap before this call (ms) | EVF<<24";
        case 77: return "setter calls in the last second: panel | EVF<<16";
        case 79: return "PROP_ICU_UILOCK (Canon's UI lock)";
        case 80: return "PROP_LV_ACTION (0 start, 1 stop)";
        case 81: return "Canon log saved (dumpf): 1 hang, 2 menu, 3 start";
        case 78: return "LV restart: 1 menu|2 close|3 done(+4 ok)|16 open fail|32 close fail, out<<8 mode<<12";
    }
    return "?";
}

static void boot_log_task(void *unused)
{
    (void)unused;
    while (!ml_started && !ml_shutdown_requested && !m50_shutdown_seen) msleep(100);   /* settings loaded */
    if (m50_shutdown_seen || !m50_dbg()) return;
    FILE *f=FIO_OpenFile("ML/LOGS/XBOOT.LOG",O_RDWR|O_SYNC);
    if (f) FIO_SeekSkipFile(f,0,SEEK_END);
    else f=FIO_CreateFile("ML/LOGS/XBOOT.LOG");
    if (!f) return;
    char line[128];
    int n=snprintf(line,sizeof(line),"=== boot, log task at %d ms, shooting mode %d, build %s %s\n",
        (int)get_ms_clock(),shooting_mode,__DATE__,__TIME__);
    FIO_WriteFile(f,line,n);
    FIO_CloseFile(f);
    unsigned written=0;
    uint32_t start=get_ms_clock();
    while (!ml_shutdown_requested && !m50_shutdown_seen && get_ms_clock()-start<60000) {
        msleep(200);
        unsigned head=boot_mark_head;
        if (head==written || m50_shutdown_seen) continue;
        f=FIO_OpenFile("ML/LOGS/XBOOT.LOG",O_RDWR|O_SYNC);
        if (!f) continue;
        FIO_SeekSkipFile(f,0,SEEK_END);
        for (; written<head; written++) {
            const struct boot_mark *m=&boot_marks[written];
            /* ML's snprintf: widths on %d work, the '-' flag on %s does not. */
            n=snprintf(line,sizeof(line),"%7d %s | %s: %d (0x%x)\n",(int)m->ms,
                m->task ? m->task:"-",boot_mark_name(m->id),m->value,(unsigned)m->value);
            FIO_WriteFile(f,line,n);
        }
        if (head>=BOOT_MARKS) FIO_WriteFile(f,"(mark buffer full)\n",19);
        FIO_CloseFile(f);
        if (head>=BOOT_MARKS) break;
    }
}

/* XPan writes nothing to the card once Canon is powering off (key log, probe
 * logs, ISO guard). 2026-11-08: after every power-off the camera stayed dead
 * until a battery pull; the 9 MB key log was still being appended to once a
 * second with no shutdown check while Canon was closing the card. */
int xpan_m50_card_ok(void)
{
    return !ml_shutdown_requested && !m50_shutdown_seen;
}

static void ui_hook_remove(int cancelled);

/* Shutdown signals (tasks.c handles the same ones for ML's own shutdown). */
static void m50_shutdown_prop(unsigned property, void *token, void *data, unsigned len)
{
    (void)token;
    int v=len ? (int)*(uint32_t *)data : -1;
    if (property==PROP_TERMINATE_SHUT_REQ) {
        m50_boot_mark(40,v);
        /* 0 = request (3 = execute comes too late and may be a stale value
         * replayed at registration; tasks.c ignores it too). */
        if (v==0) m50_shutdown_seen=1;
        else if (v==4) { m50_shutdown_seen=0; ui_hook_remove(1); }   /* cancelled */
    } else if (property==PROP_ABORT) {
        m50_boot_mark(41,v);
        if (v==1) m50_shutdown_seen=1;
    } else {
        m50_boot_mark(42,v);
        if (v>0) m50_shutdown_seen=1;
    }
    if (m50_shutdown_seen) ui_hook_remove(0);
}
/* Registered as XPan's own property slave at the very start of ML's init,
 * before ML touches the card (ML's normal handlers are registered later, in
 * the init functions). 2026-11-08: with the guard above, the camera still did
 * not respond after a card swap, and that start left no boot log at all, so
 * it stopped at or before ML's first card access (_find_ml_card, ~370 ms into
 * the short start Canon makes when the card/battery door closes). */
static unsigned early_props[]={PROP_TERMINATE_SHUT_REQ,PROP_ABORT,PROP_SHUTDOWN_REASON};
static void *volatile early_token;
static void early_token_handler(void *token) { early_token=token; }
static void *early_prop_handler(unsigned property, void *priv, void *addr, unsigned len)
{
    (void)priv;
    m50_shutdown_prop(property,0,addr,addr ? len:0);
    return _prop_cleanup(early_token,property);
}

/* init.c, my_big_init_task, before any card access. Returns 1 when Canon is
 * shutting down (do not load ML). A normal power-on reaches the card at
 * about 1000 ms; the wait to 1300 ms gives a check start time to say it is
 * one, or to power off, before ML opens a file. */
/* Set by ML's gui_main_task (gui.c) when Canon's GUI gets
 * GMT_GUICMD_START_AS_CHECK, _OPEN_SLOT_COVER or _LOCK_OFF before ML has
 * started: the short start Canon makes when the battery/card door closes,
 * which the shutdown properties alone did not catch (the camera still died
 * after every card swap). None of those commands came in any normal start in
 * XBOOT.LOG. The value is the command. */
volatile int m50_check_start;

int m50_wait_for_real_start(void)
{
    prop_register_slave(early_props,COUNT(early_props),early_prop_handler,0,early_token_handler);
    while (!m50_shutdown_seen && !m50_check_start && get_ms_clock()<1300) msleep(20);
    /* ML then stays out; the startup heartbeat (init.c) shows it on the LED. */
    return m50_check_start || m50_shutdown_seen;
}

/* Called from my_big_init_task (init.c) as soon as the card is usable. */
void m50_boot_log_start(void)
{
    /* init.c, m50_release_core1 */
    extern volatile uint32_t m50_core1_entry, m50_core1_released;
    m50_boot_mark(59, (int)m50_core1_entry);
    m50_boot_mark(60, (int)m50_core1_released);
    task_create("xpan_boot", 0x1f, 0x1000, boot_log_task, 0);
}

/* ------------------------------------------------------------------ */
/* Canon UI on top of a dimmed XPan (dim-and-freeze).                 */

/* ML and Canon share one RGBA layer on the M50; whoever writes last wins.
 * ML's GUI task sees each event before Canon's handler does, so when Canon
 * is about to show something XPan dims its layer, pushes it once and stops
 * pushing (ml_refresh_frozen); Canon then draws its UI on top. Signals
 * (README-XPAN-M50.md, Canon UI signals): event 0x79 opens/updates an
 * exposure overlay (dial, UP, LEFT), and a 0x79 with arg 5 and a first word
 * ending in 0001 closes it; SET (Q screen) and RIGHT (flash button, picture
 * style here) open dialogs that set gui_state. XPan comes back at full
 * brightness 150 ms after Canon is idle again. */
#include "bmp.h"
int xpan_photo_ui_active(void);            /* cropmarks.c */
void xpan_photo_hud_force_redraw(void);    /* xpan-photo-hud.inc */

struct disp_rec { volatile unsigned calls, changes; uint32_t p[16], a1, a2, a3, s0, s1; volatile unsigned seq; };
static struct disp_rec disp_recs[2];           /* 0 panel, 1 EVF (display probe, below) */
static volatile int canon_ui_frozen, canon_overlay_open;
static volatile int canon_layout_state;      /* Canon layout: 0 off, 1 pushing, 2 frozen */
static uint32_t canon_layout_ms;
int xpan_photo_canon_layout(void);         /* cropmarks.c */
static volatile uint32_t canon_overlay_ms, canon_frozen_ms;
static volatile unsigned canon_ui_dims;    /* shown in XPan > Canon settings help */

/* GUI task (before Canon handles the event) or shoot task. */
static void canon_ui_enter(void)
{
    if (!xpan_photo_ui_active() || gui_menu_shown() || !lv || canon_layout_state || !DISPLAY_IS_ON) return;
    uint32_t old=cli();
    int already=canon_ui_frozen;
    canon_ui_frozen=1;
    sei(old);
    if (already) return;
    ml_refresh_frozen=1;                    /* redraw_task stops copying */
    canon_frozen_ms=get_ms_clock();
    /* Dim every lit pixel; black, transparent black and empty stay. */
    uint8_t *b=bmp_vram();
    if (b) {
        /* From the EVF's top rows (above ML's 480), where the Split top bar
         * sits (camera test 2026-11-10: its lower half went grey). */
        for (int y=BMP_H_MINUS; y<480; y++) {
            uint8_t *row=b+y*BMPPITCH;
            for (int x=0; x<720; x++) {
                uint8_t c=row[x];
                if (c!=COLOR_EMPTY && c!=COLOR_BLACK && c!=COLOR_TRANSPARENT_BLACK) row[x]=COLOR_GRAY(25);
            }
        }
        refresh_yuv_from_rgb();             /* this one push, then frozen */
    }
    canon_ui_dims++;
    m50_boot_mark(71,(int)canon_ui_dims);
}

static void canon_ui_leave(void)
{
    m50_boot_mark(72,0);
    canon_ui_frozen=0;
    canon_overlay_open=0;
    ml_refresh_frozen=0;
    xpan_photo_hud_force_redraw();
    redraw();                               /* matte, HUD, Canon's LV screen */
}

/* Called from xpan_hud_controls_key for every type-0 event, before Canon. */
int xpan_m50_hud_field(void);
void xpan_m50_canon_ui_event(struct event *event, int obj0)
{
    if (event->type!=0 || IS_FAKE(event)) return;
    {
        static int n;
        if (event->param!=0x79 && n<120) { n++; m50_boot_mark(73,event->param); }
    }
    /* A HUD field is selected: the dial is XPan's, keep XPan bright. */
    if (event->param==0x79 && xpan_m50_hud_field()>=0) return;
    if (event->param==0x79) {
        if (event->arg==5 && obj0!=-1 && (obj0&0xFFFF)==0x0001) {
            canon_overlay_open=0;           /* element closed */
            return;
        }
        /* Q screen records (0x150013 open, 0x10013 close, arg 0x10) are not
         * overlays: the Q screen is a dialog, tracked by gui_state. Counting
         * its close as an open overlay held XPan dimmed for the 3 s safety
         * limit after the Q screen closed (camera test, 2026-11-08). */
        if (obj0!=-1 && (obj0&0xFFFF)==0x0013) {
            if (((uint32_t)obj0>>16)==0x0001) canon_overlay_open=0;
            return;
        }
        /* Half-press AF information (0x0002xxxx) keeps the HUD bright. */
        if (obj0!=-1 && ((uint32_t)obj0>>16)==0x0002) return;
        canon_overlay_open=1;
        canon_overlay_ms=get_ms_clock();
        canon_ui_enter();
        return;
    }
    /* Buttons that open Canon dialogs here: SET (Q screen), RIGHT (flash). */
    if (event->param==BGMT_PRESS_SET || event->param==BGMT_PRESS_RIGHT)
        canon_ui_enter();
}

/* Called from xpan_hud_controls_step (shoot task, about every 50 ms). */
static void ui_hook_install(void);        /* dial filter, below */
static void ui_hook_remove(int cancelled);
static void m50_log_ui(uint32_t st);      /* XPan log, below */
static void m50_log_mode(void);           /* XPan log, below */
void xpan_m50_canon_ui_step(void)
{
    uint32_t now=get_ms_clock();
    static uint32_t idle_since;
    int menu=gui_menu_shown();
    {
        static int last=-1;
        int st=(gui_state&0xFF)|(lv ? 1<<8:0)|(menu ? 1<<9:0)|(ml_refresh_frozen ? 1<<10:0)|(canon_ui_frozen ? 1<<11:0)
              |((canon_layout_state&3)<<12)|(xpan_photo_ui_active() ? 1<<14:0)|(DISPLAY_IS_ON ? 1<<15:0);
        if (st!=last) { last=st; m50_boot_mark(70,st); m50_log_ui((uint32_t)st); }
        m50_log_mode();
        static uint32_t rate_ms; static unsigned rate_p, rate_e;
        if (now-rate_ms>=1000) {
            unsigned cp=disp_recs[0].calls, ce=disp_recs[1].calls;
            if (rate_ms) m50_boot_mark(77,(int)(((cp-rate_p)&0xFFFF)|((ce-rate_e)<<16)));
            rate_ms=now; rate_p=cp; rate_e=ce;
        }
    }
    if (xpan_photo_ui_active() && lv) ui_hook_install();
    /* A redraw asked for while Canon had its display off (zebra.c
     * _redraw_do): once now that it is on, only back in XPan's Live View
     * with Canon idle. Anywhere else (Canon's menu, playback) Canon has drawn
     * its own screen when it turns the display on, and ML's redraw would
     * clear it (camera test 2026-11-11: Canon's menu came up blank). */
    {
        extern volatile int ml_redraw_deferred;
        if (ml_redraw_deferred && DISPLAY_IS_ON) {
            ml_redraw_deferred=0;
            if (xpan_photo_ui_active() && gui_state==GUISTATE_IDLE && !menu) { xpan_photo_hud_force_redraw(); redraw(); }
        }
    }
    /* Canon layout (INFO, cropmarks.c): XPan pushes its cleared layer with
     * the band lines once, then stays frozen so Canon's Live View screen and
     * touch controls show on top. ML's menu or leaving XPan Live View
     * unfreezes; coming back pushes and freezes again. */
    if (xpan_photo_canon_layout() && xpan_photo_ui_active() && lv && !menu) {
        /* Canon rebuilds its screen with the display off: wait until it is on. */
        if (!DISPLAY_IS_ON) { idle_since=0; return; }
        if (!canon_layout_state) {
            canon_layout_state=1; canon_layout_ms=now;
            canon_ui_frozen=0; canon_overlay_open=0; ml_refresh_frozen=0;
            xpan_photo_hud_force_redraw(); redraw();
        } else if (canon_layout_state==1 && now-canon_layout_ms>=300) {
            canon_layout_state=2;
            refresh_yuv_from_rgb();         /* cleared layer + band lines */
            ml_refresh_frozen=1;
            redraw();                       /* Canon redraws its LV screen on top */
        }
        idle_since=0;
        return;
    }
    if (canon_layout_state) {
        canon_layout_state=0;
        canon_ui_frozen=0; canon_overlay_open=0; ml_refresh_frozen=0;
        xpan_photo_hud_force_redraw(); redraw();
        idle_since=0;
        return;
    }
    int dialog=gui_state!=GUISTATE_IDLE && !menu;
    int overlay=canon_overlay_open && now-canon_overlay_ms<3000;
    if (!canon_ui_frozen) {
        if (dialog) canon_ui_enter();       /* a dialog opened some other way */
        idle_since=0;
        return;
    }
    /* ML's own menu, playback or leaving XPan Live View: back at once. */
    if (menu || !xpan_photo_ui_active() || !lv) { canon_ui_leave(); idle_since=0; return; }
    if (dialog || overlay) { idle_since=0; return; }
    if (!idle_since) idle_since=now;
    if (now-idle_since>=150 && now-canon_frozen_ms>=150) { canon_ui_leave(); idle_since=0; }
}

/* Touch. Canon's Live View touch controls (ISO corner and the rest) stay
 * live under the XPan HUD, where they cannot be seen. In the XPan views XPan
 * swallows the touch events, so touch works only where Canon's UI shows: the
 * Canon layout, a Canon dialog (Q screen) or exposure overlay on top of the
 * dimmed XPan layer, and ML's menu. M50 codes from the key log (2026-11-08):
 * 0x7F touch down, 0x80 touch up (arg 0xC, obj = touch record), in pairs
 * 80-100 ms apart for a tap. An up is swallowed only when its down was. */
#define M50_TOUCH_DOWN 0x7F
#define M50_TOUCH_UP   0x80
static unsigned touch_blocked;
/* XPan's own view is on screen (no Canon layout, dialog, overlay or ML menu). */
static int xpan_view_active(void)
{
    return xpan_photo_ui_active() && lv && !gui_menu_shown() &&
        !xpan_photo_canon_layout() && !canon_layout_state &&
        !canon_ui_frozen && gui_state==GUISTATE_IDLE;
}

/* Selection survives Canon's exposure overlays (canon_ui_frozen), not
 * dialogs, ML's menu or the Canon layout. */
static int xpan_select_ok(void)
{
    return xpan_photo_ui_active() && lv && !gui_menu_shown() &&
        !xpan_photo_canon_layout() && !canon_layout_state && gui_state==GUISTATE_IDLE;
}
#define M50_GUI_CONTROL_USED 0x61

/* Tap position: event->obj = [0, record, 0x55555555], record word 1 =
 * (y << 16) | x in LCD pixels (touch probe, 2026-11-08: corners and centre
 * landed at 28,7 / 684,60 / 49,434 / 660,443 / 368,249). */
static int m50_touch_xy(struct event *event, int *x, int *y)
{
    uint32_t o=(uint32_t)event->obj;
    if (!ram_ptr_ok(o)) return 0;
    uint32_t p=((const uint32_t *)o)[1];
    if (!ram_ptr_ok(p)) return 0;
    uint32_t v=((const uint32_t *)p)[1];
    int xx=v&0xFFFF, yy=v>>16;
    if (xx>=720 || yy>=480) return 0;
    *x=xx; *y=yy;
    return 1;
}

int xpan_photo_hud_hit_test(int x, int y);   /* xpan-photo-hud.inc */

/* HUD selection. Selectable: ISO, shutter, aperture (checked writers), the
 * learned settings EV, WB, drive, metering and format once two or more of
 * their values are recorded, and the LUT name once there are two or more
 * files. Tap a field: selected (underlined); the dial or UP/DOWN then step
 * it. Tap it again, tap anything else, leave XPan's view or wait 8 s:
 * released, and the dial is Canon's again. */
#define M50_SELECT_IDLE_MS 8000
static volatile int m50_hud_field=-1;
static volatile uint32_t m50_field_ms;
static volatile int dbg_tap_x=-1, dbg_tap_y=-1, dbg_tap_hit=-9;
/* Rows of the screen above ML's 720x480 area. Canon's Ximr context word 1
 * is the output size, height << 16 | width (XM50.LOG 2026-11-10: LCD
 * 0x01E002D0 = 720x480, EVF 0x021C02D0 = 720x540); ML's 480 rows sit in the
 * middle, and ML can draw up to 30 rows above them (BMP_H_MINUS). */
int xpan_m50_screen_extra_top(void)
{
    uint32_t out=((const volatile uint32_t *)XIMR_CONTEXT)[1];
    unsigned w=out&0xFFFF, h=out>>16;
    if (w!=720 || h<=480 || h>480-2*BMP_H_MINUS) return 0;
    return (int)(h-480)/2;
}

int xpan_m50_hud_field(void)
{
    if (m50_hud_field<0) return -1;
    /* Not released by Canon's exposure overlay (the dial still opens it on
     * the first click): only by XPan's view going away or the idle time. */
    if (!xpan_select_ok() || get_ms_clock()-m50_field_ms>M50_SELECT_IDLE_MS) {
        m50_hud_field=-1;
        return -1;
    }
    return m50_hud_field;
}
static void m50_select(int field)
{
    m50_hud_field=field; m50_field_ms=get_ms_clock();
    xpan_photo_hud_force_redraw();
}
static void m50_tap(int x, int y)
{
    int hit=xpan_photo_hud_hit_test(x,y);
    dbg_tap_x=x; dbg_tap_y=y; dbg_tap_hit=hit;
    struct m50_ctl *c=m50_ctl_for_field(hit);
    int known=hit==XPAN_HUD_ISO ? m50_iso_known :
        hit==XPAN_HUD_SHUTTER ? m50_tv_valid(last_tv) :
        hit==XPAN_HUD_APERTURE ? (lens_info.lens_exists && last_avo && last_avmax) :
        hit==XPAN_HUD_LUT ? m50_lut_count>1 :
        hit==XPAN_HUD_FORMAT ? m50_picq_known() :   /* built from the M50 PICQ layout */
        c ? c->nopts>=2 : 0;        /* learned settings: two or more options */
    if (c && !c->format && c->nopts<2 && xpan_hud_controls_field_enabled(hit))
        NotifyBox(3000,"%s: change it once in Canon's screen\nso XPan learns its values",c->name);
    if (known && xpan_hud_controls_field_enabled(hit) && xpan_m50_hud_field()!=hit) m50_select(hit);
    else if (xpan_m50_hud_field()>=0) m50_select(-1);
}

static void m50_step_field(int field, int delta)
{
    if (field==XPAN_HUD_ISO) m50_iso_write_request(m50_iso_step(delta));
    else if (field==XPAN_HUD_SHUTTER) m50_tv_write_request(m50_tv_step(delta));
    else if (field==XPAN_HUD_APERTURE) m50_av_write_request(m50_av_step(delta));
    else if (field==XPAN_HUD_LUT) { m50_lut_step(delta); xpan_photo_hud_force_redraw(); }
    else { struct m50_ctl *c=m50_ctl_for_field(field); if (c) m50_ctl_step(c,delta); }
    m50_field_ms=get_ms_clock();
}

/* UP/DOWN while a field is selected, and the GUI side of the dial. 0 = not
 * ours. Called from xpan_hud_controls_key before Canon sees the event. The
 * dial itself is stepped by the UI event filter below: these GUI events are
 * only Canon's notification, consuming them does not stop the dial. */
int xpan_m50_hud_key(struct event *event)
{
    int field=xpan_m50_hud_field();
    if (event->type!=0 || IS_FAKE(event) || field<0) return 0;
    switch (event->param) {
        case M50_GUI_CONTROL_USED:                      /* "a control was used" */
        case BGMT_WHEEL_LEFT: case BGMT_WHEEL_RIGHT:
        case BGMT_UNPRESS_UP: case BGMT_UNPRESS_DOWN:   /* Canon never saw the press */
            return 1;
        case BGMT_PRESS_UP: m50_step_field(field,1); return 1;
        case BGMT_PRESS_DOWN: m50_step_field(field,-1); return 1;
    }
    return 0;
}

/* UI event filter: the dial fix (2026-11-08, ROM 1.1.0 research,
 * README-XPAN-M50.md "Controls, step 5").
 * The M50 has no MPU message path at runtime (InitializeIntercom e062260e
 * would store mpu_recv e0622581 at 0xFB20; the RAM dump has 0 there). The
 * main CPU reads the dial itself: the "RotaryEncoder" task
 * (./EC412/RotaryEncoder.c) looks each click up in its table (ROM e0aa7598:
 * [3, 0, 0x80000407, 0x407, 0x20000b08, 0x20000b09, 0]) and calls e01e4034
 * with 0x80000407 or 0x407. e01e4654 turns that into (0x407, 1) or
 * (0x407, 0) and jumps through the UI event callback at RAM 0x5A54
 * (struct 0x5A24 + 0x30, Canon's e03d8d9b), which queues the event and
 * passes it on. Everything Canon does with a click starts there: the
 * shutter change, the "control used" GUI event 0x61, 0x02/0x03, the dialog
 * event 0x81B-0x81E and the exposure overlay (0x79). Consuming the GUI
 * events or the dialog events (an earlier build) came too late.
 * So XPan swaps that one pointer, only while it still holds Canon's
 * e03d8d9b, for a filter that calls it. No Canon code is patched. Without a
 * selected field everything is passed on. With one, a dial click steps the selected
 * field and is not passed on, so Canon never sees the click.
 * Camera test (key log, 2026-11-08): 0x407 is not the click but "dial
 * active" (flag 1 when turning starts, flag 0 about 0.5 s after it stops);
 * each click is 0x20000B08 (clockwise, Canon makes GUI 0x03 of it) or
 * 0x20000B09 (anticlockwise, GUI 0x02). With a field selected XPan steps on
 * those and keeps them; it keeps a 0x407 start and its stop as a pair. */
#define UI_CBR            0x5A54u
#define UI_CBR_CANON      0xE03D8D9Bu
#define UI_EVENT_DIAL_BUSY  0x407        /* flag 1 turning starts, flag 0 ~0.5 s after it stops */
#define UI_EVENT_DIAL_CW    0x20000B08   /* one click clockwise (Canon: GUI 0x03) */
#define UI_EVENT_DIAL_CCW   0x20000B09   /* one click anticlockwise (Canon: GUI 0x02) */
typedef int (*ui_cbr_t)(int id, int flag);
static ui_cbr_t volatile ui_orig;
static volatile int ui_hook_state;            /* 0 not tried, 1 hooked, -1 not Canon's value */
static volatile unsigned ui_dial_taken;
void xpan_hud_key_log_ui(int id, int flag, int field, int passed);

static int ui_filter(int id, int flag)
{
    static int busy_taken;    /* the "turning starts" XPan kept, so it keeps the stop too */
    int field=m50_hud_field>=0 ? xpan_m50_hud_field() : -1;
    if ((id==UI_EVENT_DIAL_CW || id==UI_EVENT_DIAL_CCW) && field>=0) {
        m50_step_field(field,id==UI_EVENT_DIAL_CW ? 1 : -1);
        ui_dial_taken++;
        xpan_hud_key_log_ui(id,flag,field,0);
        return 0;
    }
    if (id==UI_EVENT_DIAL_BUSY) {
        int take=flag ? field>=0 : busy_taken;
        busy_taken=flag ? take : 0;
        if (take) { xpan_hud_key_log_ui(id,flag,field,0); return 0; }
    }
    xpan_hud_key_log_ui(id,flag,field,1);
    return ui_orig(id,flag);
}

/* Shoot task, while XPan Live View is up. Installed once; it stays (it
 * forwards everything unless a field is selected). */
static void ui_hook_install(void)
{
    if (ui_hook_state) return;
    uint32_t v=MEM(UI_CBR);
    if (v!=UI_CBR_CANON) { ui_hook_state=-1; return; }
    ui_orig=(ui_cbr_t)v;
    MEM(UI_CBR)=(uint32_t)ui_filter;
    ui_hook_state=1;
}

/* Power-off (m50_shutdown_prop): Canon gets its own callback back, so its
 * shutdown runs with no XPan code in the dial path. 2 = removed; a cancelled
 * shutdown sets 0 again so the shoot task can put the filter back. */
static void ui_hook_remove(int cancelled)
{
    if (cancelled) { if (ui_hook_state==2) ui_hook_state=0; return; }
    if (ui_hook_state==1 && MEM(UI_CBR)==(uint32_t)ui_filter) MEM(UI_CBR)=(uint32_t)ui_orig;
    if (ui_hook_state==1) ui_hook_state=2;
}

/* AF probe (README "Touch AF and Canon's AF box"): the last tap. */
static volatile int af_tap_x=-1, af_tap_y=-1, af_tap_blocked;
/* Touch AF in XPan's view (README "Touch AF and Canon's AF box"): a tap on
 * the live picture (inside the 65:24 frame, not on an XPan field) goes to
 * Canon, which focuses there (the picture area getter below maps it to the
 * moved picture). Not passed: taps where Canon's own touch buttons sit,
 * hidden under XPan (measured on the LCD, phone photo 2026-11-10): Q (top
 * right), touch shutter and magnify (bottom corners), the bottom setting
 * bar. Canon's layout gets every tap anyway. */
static int disp_band(int *src_y, int *dst_y, int *h);
int xpan_photo_get_liveview_frame_rect(int *x, int *y, int *crop_width, int *crop_height);   /* cropmarks.c */
int xpan_photo_hud_hit_test(int x, int y);   /* xpan-photo-hud.inc */
static int m50_tap_for_canon(int x, int y)
{
    int s, d, h, fx, fy, fw, fh;
    if (!disp_band(&s,&d,&h) || !xpan_photo_get_liveview_frame_rect(&fx,&fy,&fw,&fh)) return 0;
    if (x<fx || x>=fx+fw || y<fy || y>=fy+fh) return 0;     /* outside the picture */
    if (xpan_photo_hud_hit_test(x,y)>=0) return 0;           /* an XPan field */
    if (x>=600 && y<80) return 0;                            /* Canon: Q */
    if (y>=360 && (x<160 || x>=600)) return 0;               /* Canon: touch shutter, magnify */
    if (y>=420) return 0;                                    /* Canon: setting bar */
    return 1;
}
static volatile uint32_t af_tap_ms;
static volatile unsigned af_taps;
int xpan_m50_touch_blocked(struct event *event)
{
    static int down_blocked;
    if (event->type!=0 || IS_FAKE(event)) return 0;
    if (event->param==M50_TOUCH_DOWN || event->param==M50_TOUCH_UP) touch_record(event);
    if (event->param==M50_TOUCH_UP) {
        int b=down_blocked; down_blocked=0;
        return b;
    }
    if (event->param!=M50_TOUCH_DOWN) return 0;
    { int tx,ty; if (m50_touch_xy(event,&tx,&ty)) { af_tap_x=tx; af_tap_y=ty; af_tap_ms=get_ms_clock(); af_taps++; } }
    down_blocked=xpan_view_active();
    if (down_blocked) {
        int x,y;
        if (m50_touch_xy(event,&x,&y)) {
            m50_tap(x,y);                                     /* HUD selection (releases a field) */
            if (m50_tap_for_canon(x,y)) down_blocked=0;       /* the picture: Canon's touch AF */
        }
        if (down_blocked) touch_blocked++;
    }
    af_tap_blocked=down_blocked;
    return down_blocked;
}


/* ------------------------------------------------------------------ */
/* XPan log (ML/LOGS/XM50.LOG, README "XPan log").                      */

/* One line per photo (colour table hook) and per grain stage, written later
 * by XPan's task when Canon is idle. Each ring has one producer (Canon's
 * develop task, Canon's ShtVfx task) and one consumer (XPan's task): the
 * producer fills the slot, then publishes the head after a barrier. Hooks
 * never print, block or touch the card. */
struct m50_ev { uint32_t ms, a, b, c, d, x; };
#define M50_EVS 8
struct m50_ring { struct m50_ev ev[M50_EVS]; volatile unsigned head, saved; };
static struct m50_ring m50_photo_ring, m50_grain_ring, m50_ui_ring, m50_mode_ring;
static void m50_ring_put(struct m50_ring *r, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t x)
{
    unsigned h=r->head;
    struct m50_ev *e=&r->ev[h%M50_EVS];
    e->ms=get_ms_clock(); e->a=a; e->b=b; e->c=c; e->d=d; e->x=x;
    asm volatile("dmb" ::: "memory");
    r->head=h+1;
}
/* grid: 0 Canon's table, 10 or 17 XPan's; layout: the 17-point setting. */
static void m50_log_photo(int grid, int layout, uint32_t us)
{
    m50_ring_put(&m50_photo_ring,xpan_photo_enabled(),crop_on,(uint32_t)grid|(uint32_t)layout<<8,film_stage_wanted(),us);
}
static void m50_log_grain(uint32_t ms, uint32_t grained, uint32_t rows, int late)
{
    m50_ring_put(&m50_grain_ring,ms,grained,rows,late,0);
}
/* Shoot task (xpan_m50_canon_ui_step), with Debug info only: each UI state
 * change (camera test 2026-11-11: a crash after playback in the Canon
 * layout; the boot log covers only the first 60 s). */
/* Shoot task, with Debug info only: Canon's mode values ML already
 * receives, at each change (camera test 2026-11-11: in the M50's video
 * position PROP_SHOOTING_MODE_2 reports the movie exposure mode, 3 for
 * movie manual like photo M, so is_movie_mode() never matched). */
static void m50_log_mode(void)
{
    if (!m50_dbg()) return;
    extern int shooting_mode_custom, ae_mode_movie, shooting_type, lv_movie_select;
    static uint32_t last[5]={~0u,~0u,~0u,~0u,~0u};
    uint32_t v[5]={(uint32_t)shooting_mode,(uint32_t)shooting_mode_custom,(uint32_t)ae_mode_movie,(uint32_t)shooting_type,(uint32_t)lv_movie_select};
    if (!memcmp(v,last,sizeof(v))) return;
    memcpy(last,v,sizeof(v));
    m50_ring_put(&m50_mode_ring,v[0],v[1],v[2],v[3],v[4]);
}
static void m50_log_ui(uint32_t st)
{
    if (!m50_dbg()) return;
    m50_ring_put(&m50_ui_ring,st,CURRENT_GUI_MODE,PLAY_OR_QR_MODE,LV_OVERLAYS_MODE,canon_gui_front_buffer_disabled());
}

/* ------------------------------------------------------------------ */
/* Colour table hook (MMU patch, include/platform/mmu_patches.h).      */

/* Replaces Canon's E00E73BC, called once per still develop from the
 * ShootDevelop path (E0082CC0) with the colour table Canon chose:
 *   E00E73BC(table, a1, a2, a3):
 *     return E011E194(*E081E8EE(), a1, a2, table, 0x1000, a3);
 * E011E194 picks the grid from the size (0x1000 = 10 points, 0x4CC4 = 17)
 * and sets the engine's grid register itself (E087D76C), as Canon's other
 * loader E01C7358 does for grid 17. Exactly Canon's call, with XPan's table
 * and its size (m50_look_apply) when Custom LUT is on and a LUT is loaded;
 * Canon's table always goes with Canon's size 0x1000. Runs in Canon's
 * develop task: no card access, no waiting. */
int xpan_m50_lucky_load(void *table, int a1, int a2, int a3)
{
    typedef void *(*ctx_fn)(void);
    typedef int (*load_fn)(void *ctx, int a1, int a2, void *table, int size, int a3);
    const ctx_fn canon_ctx=(ctx_fn)0xE081E8EF;         /* Thumb */
    const load_fn canon_load=(load_fn)0xE011E195;      /* Thumb */
    int size;
    void *use=m50_look_apply(table,&size);
    if (use==table) size=0x1000;                    /* Canon's table: Canon's size */
    /* grid 1: Live LUT preview graded Canon's style library, Canon's table is the look */
    m50_log_photo(use==table ? (live_applied ? 1 : 0) : size==(int)XPAN_GRID17_BYTES ? 17 : 10,M50_GRID17_TEST ? m50_grid17 : 0,m50_look_us);
    return canon_load(*(void **)canon_ctx(),a1,a2,use,size,a3);
}

/* ------------------------------------------------------------------ */
/* Film grain (README-XPAN-M50.md, "Film grain").                      */

/* The EOS M "Film" engine (xpan-film-grain.c, CPU): a grain texture made
 * once in XPan's task, added to the luma of the developed YUV image (words
 * U Y0 V Y1, signed chroma), with the EOS M's mid-tone weighting. Applied in
 * Canon's own grain stage (below), from Canon's input to Canon's output. */
static CONFIG_INT("xpan.film.grain",film_grain,0);
static CONFIG_INT("xpan.film.grain.strength",grain_strength,25);
static CONFIG_INT("xpan.film.grain.size",grain_size,1);   /* 0 fine, 1 medium, 2 coarse */
int m50_picq_raw_wanted(void);
int xpan_film_grain_enabled(void) { return film_grain && grain_strength>0 && !m50_picq_raw_wanted(); }
/* XPan's grain in Canon's grain stage (README "XPan's own grain in Canon's
 * filter stage"; the JPEG-task grain held ShtEncode and hung the camera). */
#define M50_STAGE_GRAIN 1
/* Only while XPan is on, in photo mode (as on the EOS M, xpan-film-grain.c):
 * with XPan off the camera is plain Canon (camera test 2026-11-10: photos
 * slow with XPan off, the grain was still running). */
static int film_stage_wanted(void) { return M50_STAGE_GRAIN && film_grain && grain_strength>0 && xpan_photo_enabled() && !is_movie_mode() && !m50_picq_raw_wanted(); }
/* 256, not the EOS M's 512 (camera test 2026-11-09: the camera crashed
 * after the photo was saved, even with the grain cut off at 2 s). ML gives
 * allocations of 512 KB or more from shoot_malloc, Canon's shooting pool,
 * which Canon expects empty ("can lock completely, Err 70, or crash",
 * src/mem.c); the 512x512 int16 build buffer was exactly 512 KB. At 256 the
 * build buffer is 128 KB and the texture 64 KB, both ordinary heap. */
#define FILM_TEX 256u
static int8_t *film_tex;
static int16_t film_amp[256];
static volatile int film_ready;
/* Grain lock (review 2026-11-09): a DryOS semaphore, initial count 1. The
 * hook takes it without blocking (take_semaphore_now) and the worker gives
 * it back when done; the texture rebuild takes it with a timeout. cli() only
 * masks the current core, and on the M50 tasks run on both cores. */
static struct semaphore *grain_lock;
static unsigned film_strength=0xffffffffu, film_size=0xffffffffu;
static char grain_status[64]="off";
static uint32_t film_rng(uint32_t *state)
{
    uint32_t x=*state; x^=x<<13; x^=x>>17; x^=x<<5; return *state=x;
}
static unsigned film_isqrt(unsigned v)
{
    unsigned r=0,b=1u<<30;
    while (b>v) b>>=2;
    while (b) { if (v>=r+b) { v-=r+b; r=(r>>1)+b; } else r>>=1; b>>=2; }
    return r;
}
/* XPan task: build the texture for the current strength and size. */
static int film_prepare(void)
{
    unsigned strength=(unsigned)COERCE(grain_strength,1,100),size=(unsigned)COERCE(grain_size,0,2);
    if (film_ready && film_tex && film_strength==strength && film_size==size) return 1;
    if (!grain_lock || take_semaphore(grain_lock,1000)) return 0;   /* 0 = taken */
    film_ready=0;
    if (!film_tex) film_tex=malloc(FILM_TEX*FILM_TEX);
    int16_t *g=malloc(FILM_TEX*FILM_TEX*sizeof(int16_t));
    int16_t *line=malloc(FILM_TEX*sizeof(int16_t));
    if (!film_tex || !g || !line) {
        if (g) free(g);
        if (line) free(line);
        snprintf(grain_status,sizeof(grain_status),"no memory for the grain texture");
        give_semaphore(grain_lock);
        return 0;
    }
    uint32_t rng=0x9e3779b9u;
    for (unsigned i=0;i<FILM_TEX*FILM_TEX;i++) {
        uint32_t r=film_rng(&rng);
        g[i]=(int16_t)((int)(r&255)+(int)((r>>8)&255)+(int)((r>>16)&255)+(int)(r>>24)-510);
    }
    static const unsigned passes[3]={1,2,4};
    for (unsigned pass=0;pass<passes[size];pass++) {
        for (unsigned y=0;y<FILM_TEX;y++) {
            int16_t *row=g+y*FILM_TEX;
            for (unsigned x=0;x<FILM_TEX;x++) line[x]=row[x];
            for (unsigned x=0;x<FILM_TEX;x++)
                row[x]=(int16_t)((line[(x-1)&(FILM_TEX-1)]+2*line[x]+line[(x+1)&(FILM_TEX-1)])/4);
        }
        for (unsigned x=0;x<FILM_TEX;x++) {
            for (unsigned y=0;y<FILM_TEX;y++) line[y]=g[y*FILM_TEX+x];
            for (unsigned y=0;y<FILM_TEX;y++)
                g[y*FILM_TEX+x]=(int16_t)((line[(y-1)&(FILM_TEX-1)]+2*line[y]+line[(y+1)&(FILM_TEX-1)])/4);
        }
    }
    long long sum=0,sq=0;
    for (unsigned i=0;i<FILM_TEX*FILM_TEX;i++) { sum+=g[i]; sq+=(long long)g[i]*g[i]; }
    int mean=(int)(sum/(int)(FILM_TEX*FILM_TEX));
    unsigned sd=film_isqrt((unsigned)(sq/(FILM_TEX*FILM_TEX)-(long long)mean*mean));
    if (!sd) sd=1;
    for (unsigned i=0;i<FILM_TEX*FILM_TEX;i++) {
        int v=(g[i]-mean)*32/(int)sd;
        film_tex[i]=(int8_t)COERCE(v,-127,127);
    }
    free(line); free(g);
    /* Peak grain (8-bit levels, 1 sigma) = strength*0.12, shaped
     * 0.15+0.85*sqrt(4t(1-t)): strongest at mid grey (EOS M values). */
    for (unsigned y=0;y<256;y++) {
        unsigned q=film_isqrt(4u*y*(255u-y)*15378u)*1000u/31623u;
        unsigned shape=150u+850u*MIN(q,1000u)/1000u;
        film_amp[y]=(int16_t)(strength*12u*shape*8u/100000u);
    }
    /* The stage workers run on both CPUs (README "Grain speed"): write the
     * texture and the amplitude table back to RAM (clean only: never drops
     * data, even in a cache line shared with a neighbour). */
    dcache_clean((uint32_t)film_tex,FILM_TEX*FILM_TEX);
    dcache_clean((uint32_t)film_amp,sizeof(film_amp));
    film_strength=strength; film_size=size;
    snprintf(grain_status,sizeof(grain_status),"ready (strength %d, size %d)",(int)strength,(int)size);
    asm volatile("dmb" ::: "memory");
    film_ready=1;
    give_semaphore(grain_lock);
    return 1;
}
/* RAM is inner write-back and outer (L2) non-cacheable (Canon's MMU tables,
 * README "Camera test 5"): only Canon's L1 cache calls matter. */
extern void dcache_invalidate(uint32_t addr, uint32_t size);
extern void dcache_clean_invalidate(uint32_t addr, uint32_t size);   /* E0577834, MCR c7,c14,1 */
#define FILM_BAND_ROWS 16u                      /* 16 x 12000 bytes, whole cache lines */
static volatile int grain_abort;                /* Canon stopped waiting: stop at the next band */
/* Grain stage: Canon's input row to Canon's output row, every word written
 * (width even), luma grained, chroma copied, EOS M arithmetic (texture
 * index (x+ox)&255 kept in t); written so the loop stays in registers with
 * one load per word (the -Os build spilled to the stack and loaded each
 * source word twice, README "Grain speed"). */
__attribute__((optimize("O2")))
static void film_row_copy(uint32_t *restrict dst, const uint32_t *restrict src, unsigned r, unsigned width, unsigned seed)
{
    const int16_t *restrict amp=film_amp;
    const unsigned by=r>>7;
    for (unsigned x0=0,bx=0;x0<width;x0+=64u,bx++) {
        uint32_t h=(bx*0x9e3779b1u)^(by*0x85ebca77u)^seed; h^=h>>15; h*=0x2c1b3c6du; h^=h>>12;
        const unsigned oy=(h>>9)&(FILM_TEX-1);
        const int8_t *restrict tex=film_tex+(((r+oy)&(FILM_TEX-1))*FILM_TEX);
        unsigned t=(x0+h)&(FILM_TEX-1);
        const unsigned n=MIN(64u,width-x0)>>1;
        const uint32_t *restrict s=src+(x0>>1);
        uint32_t *restrict d=dst+(x0>>1);
        for (unsigned i=0;i<n;i++) {
            const uint32_t w=s[i];
            int y0v=(int)((w>>8)&255u),y1v=(int)(w>>24);
            y0v+=(tex[t]*amp[y0v])>>8;
            y1v+=(tex[(t+1)&(FILM_TEX-1)]*amp[y1v])>>8;
            t=(t+2)&(FILM_TEX-1);
            y0v=y0v<0 ? 0 : y0v>255 ? 255 : y0v;
            y1v=y1v<0 ? 0 : y1v>255 ? 255 : y1v;
            d[i]=(w&0x00ff00ffu)|((uint32_t)y0v<<8)|((uint32_t)y1v<<24);
        }
    }
}
/* Whole image, in bands of whole cache lines, through the cached aliases:
 * invalidate the input before reading (written by Canon's DMA), invalidate
 * the output before writing and clean it after (read by the next stage's
 * DMA), leave no image lines cached. Grain until FILM_STAGE_BUDGET_US, then
 * plain copies (about 0.3 s for the whole image), so the output is always
 * complete well inside Canon's 10 s stage wait. Stops writing at once if the
 * stage stopped waiting (grain_abort). Returns the rows grained. */
#define FILM_STAGE_BUDGET_US 6000000u
static unsigned film_stage_apply(uint32_t in, uint32_t out, unsigned width, unsigned first, unsigned rows, unsigned seed, int grain)
{
    const unsigned pitch=width*2u;
    uint32_t start=get_us_clock();
    unsigned grained=0;
    for (unsigned y0=first;y0<rows;y0+=FILM_BAND_ROWS) {
        if (grain_abort) break;
        unsigned n=MIN(FILM_BAND_ROWS,rows-y0), bytes=n*pitch;
        uint32_t src=(uint32_t)CACHEABLE(in+y0*pitch), dst=(uint32_t)CACHEABLE(out+y0*pitch);
        int g=grain && film_tex && get_us_clock()-start<FILM_STAGE_BUDGET_US;
        dcache_invalidate(src,bytes);
        dcache_invalidate(dst,bytes);
        if (g) { for (unsigned i=0;i<n;i++) film_row_copy((uint32_t *)(dst+i*pitch),(const uint32_t *)(src+i*pitch),y0+i,width,seed); grained+=n; }
        else memcpy((void *)dst,(const void *)src,bytes);
        dcache_clean_invalidate(dst,bytes);
        dcache_invalidate(src,bytes);
    }
    return grained;
}
static volatile int grain_late;
/* Grain stage workers (README "Grain speed"): one per CPU, each grains its
 * own half of the image (rows [first, end), whole bands), its own 6 s cap.
 * Cache facts 2026-11-10: caches on, RAM inner write-back but outer (L2)
 * non-cacheable, so only the 32 KB L1 helps; the work is CPU bound and splits
 * cleanly. CPU 0 is mostly idle during the stage (ShtVfx waits blocked). */
struct stage_part { uint32_t in, out; unsigned width, first, end, seed; volatile unsigned grained; volatile uint32_t us, cpu; struct semaphore *req, *done; };
static struct stage_part stage_parts[2];
static volatile int stage_workers;          /* tasks started (1 or 2) */
static void stage_worker(void *arg)
{
    struct stage_part *pt=&stage_parts[(int)(uintptr_t)arg];
    while (!ml_shutdown_requested) {
        if (take_semaphore(pt->req,1000)) continue;      /* 0 = taken */
        uint32_t t0=get_us_clock();
        pt->cpu=get_cpu_id();
        pt->grained=film_stage_apply(pt->in,pt->out,pt->width,pt->first,pt->end,pt->seed,1);
        pt->us=get_us_clock()-t0;
        give_semaphore(pt->done);
    }
}
static void stage_workers_start(void)
{
    static const char *const names[2]={"xpan_stage1","xpan_stage0"};
    static const int cpus[2]={1,0};
    for (int k=0;k<2;k++) {
        stage_parts[k].req=create_named_semaphore(k ? "xpan_stage_req0":"xpan_stage_req1",0);
        stage_parts[k].done=create_named_semaphore(k ? "xpan_stage_done0":"xpan_stage_done1",0);
        if (!stage_parts[k].req || !stage_parts[k].done) break;
        task_create_ex(names[k],0x1c,0x2000,stage_worker,(void *)(uintptr_t)k,cpus[k]);
        stage_workers=k+1;
    }
}
static void grain_worker_start(void)
{
    grain_lock=create_named_semaphore("xpan_grain_lock",1);
    if (grain_lock) stage_workers_start();
}
/* Grain stage (Canon's ShtVfx task, from xpan_m50_cf_grain_run): XPan's
 * grain from Canon's input to Canon's output instead of Canon's B/W engine.
 * The worker does it; ShtVfx waits blocked, as it waits for Canon's engine
 * (E086D982), so no Canon task is kept busy. Job: +0 input, +0xC output,
 * +0x10/+0x14/+0x18 width, +0x1C rows (probe 2026-11-10: 6000 x 4000, 155 ms
 * for Canon's engine). Returns 1 when the output is written, 0 to let
 * Canon's driver run (unexpected job: Canon's own B/W result). */
static volatile unsigned sg_applied, sg_copied, sg_skipped;
static volatile int sg_code;
static volatile uint32_t sg_ms, sg_grained, sg_rows;
#define STAGE_WAIT_MS 9000                      /* Canon's stage waits 10 s */
static int m50_stage_grain(const uint32_t *job)
{
    if (!film_stage_wanted()) return 0;
    uint32_t in=job[0], out=job[3], width=job[4], rows=job[7];
    uint32_t ci=(uint32_t)CACHEABLE(in), co=(uint32_t)CACHEABLE(out), size=width*2u*rows;
    int ok=job[5]==width && job[6]==width && width>=64 && width<=8192 && !(width&15)
        && rows>=FILM_BAND_ROWS && rows<=8192
        && !(in&31) && !(out&31) && ci>=0x100000 && co>=0x100000
        && ci+size<=0x40000000u && co+size<=0x40000000u && (ci+size<=co || co+size<=ci);
    if (!ok || !grain_lock || !stage_workers) { sg_skipped++; sg_code=2; return 0; }
    uint32_t t0=get_ms_clock();
    int locked=!take_semaphore_now(grain_lock);   /* 0 = taken */
    if (!locked || !film_ready) {
        /* texture being built (settings just changed): colour, no grain */
        if (locked) give_semaphore(grain_lock);
        grain_abort=0;
        film_stage_apply(in,out,width,0,rows,0,0);
        sg_copied++; sg_code=1; sg_grained=0; sg_rows=rows; sg_ms=get_ms_clock()-t0;
        return 1;
    }
    const int n=stage_workers;
    const unsigned half=n==2 ? (rows/2)&~(FILM_BAND_ROWS-1) : rows;
    const uint32_t seed=get_ms_clock()*2654435761u;
    grain_abort=0;
    for (int k=0;k<n;k++) {
        struct stage_part *pt=&stage_parts[k];
        pt->in=in; pt->out=out; pt->width=width; pt->seed=seed;
        pt->first=k ? half:0; pt->end=k ? rows:half;
        pt->grained=0; pt->us=0;
        while (!take_semaphore_now(pt->done)) ;              /* drop a stale "done" */
    }
    asm volatile("dmb" ::: "memory");
    for (int k=0;k<n;k++) give_semaphore(stage_parts[k].req);
    int got[2]={0,0}, late=0;
    for (int k=0;k<n;k++) {                     /* both within STAGE_WAIT_MS in total */
        int left=STAGE_WAIT_MS-(int)(get_ms_clock()-t0);
        got[k]=!take_semaphore(stage_parts[k].done,MAX(left,1));
        if (!got[k]) late=1;
    }
    if (late) {
        grain_abort=1;                          /* never expected: each worker caps its grain at 6 s */
        grain_late++; sg_code=3;
        for (int k=0;k<n;k++) if (!got[k]) take_semaphore(stage_parts[k].done,1000);
    }
    give_semaphore(grain_lock);
    sg_applied++; sg_grained=stage_parts[0].grained+(n==2 ? stage_parts[1].grained:0);
    sg_rows=rows; sg_ms=get_ms_clock()-t0;
    m50_log_grain(sg_ms,sg_grained,rows,late);
    return 1;
}

/* XPan task: keep the texture matching the settings. */
static void m50_grain_poll(void)
{
    if (!M50_STAGE_GRAIN) return;
    if (!film_grain || get_halfshutter_pressed()) return;
    unsigned strength=(unsigned)COERCE(grain_strength,1,100),size=(unsigned)COERCE(grain_size,0,2);
    if (!film_ready || film_strength!=strength || film_size!=size) film_prepare();
}
static MENU_SELECT_FUNC(grain_select) { (void)priv; (void)delta; film_grain=!film_grain; }
static MENU_UPDATE_FUNC(grain_display)
{
    uint32_t cur[10];
    int on=cf_lv_read(cur) && cur[0]==1;
    MENU_SET_VALUE("%s",film_grain ? "ON":"OFF");
    MENU_SET_ENABLED(film_grain);
    MENU_SET_HELP("Film grain on the saved JPEG. PLAY: strength and size.");
    if (film_grain && m50_picq_raw_wanted()) MENU_SET_WARNING(MENU_WARN_ADVICE,"Paused while a RAW format is chosen: Canon's filter stage allows only JPEG.");
    else if (film_grain && !m50_pasm()) MENU_SET_WARNING(MENU_WARN_ADVICE,"Film grain runs in P, Tv, Av and M.");
    (void)on;
}
static MENU_UPDATE_FUNC(grain_diag_display)
{
    uint32_t cur[10];
    int on=cf_lv_read(cur) && cur[0]==1;
    MENU_SET_VALUE("%d photos",(int)sg_applied);
    MENU_SET_HELP("%sCanon filter stage %s. Last %d ms, %d of %d rows grained; no grain %d, skipped %d (reason %d). %s",
        cgrain_why,on ? "on":"off",(int)sg_ms,(int)sg_grained,(int)sg_rows,(int)sg_copied,(int)sg_skipped,(int)sg_code,grain_status);
}
static MENU_UPDATE_FUNC(look_diag_display)
{
    MENU_SET_VALUE("%d graded",(int)m50_look_graded);
    MENU_SET_HELP("Canon's colour table: %s. Kept Canon's: %d, flags %x, %d us.",
        m50_look_status,(int)m50_look_kept,(unsigned)m50_look_flags,(int)m50_look_us);
}
static MENU_SELECT_FUNC(debug_info_select) { (void)priv; (void)delta; xpan_photo_debug_toggle(); }
static struct menu_entry *m50_diag_entry;    /* XPan > Diagnostics, set when the menu is added */
static MENU_UPDATE_FUNC(debug_info_display)
{
    MENU_SET_VALUE("%s",xpan_photo_debug_enabled() ? "ON":"OFF");
    MENU_SET_ENABLED(xpan_photo_debug_enabled());
    /* ML does not update a hidden row, so this visible one shows or hides it. */
    if (m50_diag_entry) m50_diag_entry->shidden=!xpan_photo_debug_enabled();
}

/* Grain stage (README "XPan's own grain in Canon's filter stage"): Canon's
 * creative filter grain driver run (E086DA10, RoughMonochromeYuv.c), called
 * by StartRoughMonoPath in the CreativeFilterPath task after Canon locked the
 * engine resources and turned the clock on; Canon's art filter task waits for
 * the path's answer (E0545D52, 10 s). XPan's grain runs here instead of
 * Canon's B/W engine; any other job runs Canon's driver. */
int xpan_m50_cf_grain_tramp(void *job);
asm(
    ".text\n.syntax unified\n.thumb\n.align 2\n"
    ".global xpan_m50_cf_grain_tramp\n.thumb_func\n"
    "xpan_m50_cf_grain_tramp:\n"
    "    push {r4, r5, lr}\n"
    "    mov r4, r0\n"
    "    sub sp, #0x5c\n"
    "    movs r1, #0x18\n"
    "    ldr.w pc, 1f\n"
    ".align 2\n"
    "1:  .word 0xE086DA19\n"          /* E086DA18, Thumb */
);
static int cfg_ram(uint32_t a) { return a>=0x1000 && a<0x80000000 && !(a&3); }
int xpan_m50_cf_grain_run(void *job)
{
    if (!cfg_ram((uint32_t)job)) return xpan_m50_cf_grain_tramp(job);
    int xpan=m50_stage_grain((const uint32_t *)job);
    return xpan ? 0 : xpan_m50_cf_grain_tramp(job);
}

/* ------------------------------------------------------------------ */
/* Live LUT preview (README "Live LUT preview").                        */

/* First camera test (2026-11-09): writing the look into the eight Live
 * View copies (0x9E6938...) made Live View flicker between the look and
 * Canon's: Canon rebuilds those copies continuously (the RAM dump caught a
 * task doing it: ColorLucky.c E0630767/E0630B9D, ColorHistAlgor.c
 * E03831CD), by blending tables read 4 KB / 8 KB apart from a base: the
 * decrypted style library (0x694820..., never changed in any scan). The
 * photo develop also takes its table from the library (0x69A820).
 * So with Live LUT preview ON, XPan grades the library tables themselves
 * (each built from Canon's original, same conversion, LUT, strength and tone
 * base as the photo; originals kept on the heap). Canon's rebuilds then make
 * graded Live View tables, and the photo comes out graded from the library:
 * the colour-table hook passes Canon's table through meanwhile (no double
 * grade). OFF puts the originals back. */
static CONFIG_INT("xpan.m50.live", m50_live_on, 0);
#define LIB_FROM  0x00680000
#define LIB_TO    0x00720000
#define LIB_MAX   40
#define LIVE_TABLE_BYTES 4096
static uint32_t lib_addr[LIB_MAX];
static uint32_t lib_hash_orig[LIB_MAX], lib_hash_put[LIB_MAX];
static volatile unsigned live_lost;
static uint32_t lib_hash(const volatile uint32_t *t);
static uint32_t *lib_orig;                      /* heap: originals, LIB_MAX tables */
static uint32_t *live_tmp;                      /* heap: one table */
static int lib_count;
static volatile int live_applied;
static uint32_t live_flags;
static char live_status[64]="off";   /* declared above for the menu */
static unsigned lutp_index(unsigned r, unsigned g, unsigned b);
static int lutp_is_table(const uint32_t *t);
/* Find the library tables once and keep their originals. */
static int lib_find(void)
{
    if (lib_count) return lib_count;
    static uint32_t a[LIB_MAX];
    int n=0;
    const unsigned white=lutp_index(8,8,8);
    for (uint32_t p=LIB_FROM+4*white; p<LIB_TO && n<LIB_MAX; p+=4) {
        if (*(volatile uint32_t *)p!=0x20000000) continue;
        const uint32_t *t=(const uint32_t *)(p-4*white);
        if (!lutp_is_table(t)) continue;
        a[n++]=(uint32_t)t;
        p+=LIVE_TABLE_BYTES-4;                  /* next table starts after this one */
    }
    if (!n) return 0;
    lib_orig=malloc(n*LIVE_TABLE_BYTES);
    live_tmp=malloc(LIVE_TABLE_BYTES);
    if (!lib_orig || !live_tmp) {
        if (lib_orig) free(lib_orig);
        if (live_tmp) free(live_tmp);
        lib_orig=live_tmp=0;
        snprintf(live_status,sizeof(live_status),"no memory for %d tables",n);
        return 0;
    }
    for (int i=0;i<n;i++) {
        lib_addr[i]=a[i];
        memcpy(lib_orig+i*1024,(const void *)a[i],LIVE_TABLE_BYTES);
        lib_hash_orig[i]=lib_hash_put[i]=lib_hash(lib_orig+i*1024);
    }
    lib_count=n;
    return n;
}
/* Canon decrypts a second library copy while the EVF is on (camera test
 * 2026-11-10: 40 tables instead of 20, the new set at 0x6A0858). Before each
 * write the table must still hold Canon's original or what XPan last wrote:
 * anything else means Canon freed or reused that memory, and XPan drops the
 * address instead of writing into it. */
static uint32_t lib_hash(const volatile uint32_t *t)
{
    uint32_t s=0;
    for (unsigned i=0;i<1024;i++) s=(s<<5|s>>27)^t[i];
    return s;
}
static void lib_put(int i, const uint32_t *table)
{
    if (!lib_addr[i]) return;
    uint32_t now=lib_hash((const volatile uint32_t *)lib_addr[i]);
    if (now!=lib_hash_orig[i] && now!=lib_hash_put[i]) { lib_addr[i]=0; live_lost++; return; }
    memcpy((void *)lib_addr[i],table,LIVE_TABLE_BYTES);
    dcache_clean(lib_addr[i],LIVE_TABLE_BYTES);
    lib_hash_put[i]=lib_hash(table);
}
/* XPan task, every 50 ms (works every 500 ms). */
static void m50_live_poll(void)
{
    static uint32_t last_ms;
    static int applied_index=-1, applied_strength=-1, applied_tone=-1, applied_identity=-1;
    if (get_ms_clock()-last_ms<500) return;
    last_ms=get_ms_clock();
    /* Video (README-XPAN-M50.md "Video mode"): the LUT on the movie through
     * the same graded library, with XPan's movie ON/OFF (cropmarks.c) and
     * whatever Live LUT preview (a photo option, hidden in video) says. */
    extern int xpan_movie_enabled(void);
    int want=m50_look_on && m50_cube_valid && (!m50_tone_cineon || m50_bases[0].valid)
             && (is_movie_mode() ? xpan_movie_enabled() : m50_live_on && xpan_photo_enabled());
    if (!want) {
        if (live_applied) {
            live_applied=0;                     /* the hook grades photos again */
            for (int i=0;i<lib_count;i++) lib_put(i,lib_orig+i*1024);
            snprintf(live_status,sizeof(live_status),"Canon's %d library tables back",lib_count);
        }
        return;
    }
    if (live_applied && applied_index==m50_cube_loaded_index && applied_strength==m50_look_strength &&
        (applied_tone!=0)==(m50_tone_cineon!=0) && applied_identity==m50_cube_identity) return;
    if (!lib_find()) { if (!lib_orig) return; snprintf(live_status,sizeof(live_status),"library tables not found"); return; }
    if (!m50_look_lock || take_semaphore_now(m50_look_lock)) return;   /* retry next poll */
    if (!m50_cube_valid) { give_semaphore(m50_look_lock); return; }
    const struct xpan_lut *base=m50_tone_cineon ? &m50_bases[0].lut : 0;
    unsigned strength=(unsigned)COERCE(m50_look_strength,0,100)*65535u/100;
    int graded=0, kept=0;
    for (int i=0;i<lib_count;i++) {
        struct xpan_native_cube_stats stats;
        uint32_t flags=0;
        if (xpan_native_table_prepare_cube(live_tmp,lib_orig+i*1024,&m50_cube,strength,
                m50_cube_identity,base,&stats,&flags)) { lib_put(i,live_tmp); graded++; }
        else { lib_put(i,lib_orig+i*1024); kept++; live_flags=flags; }
    }
    give_semaphore(m50_look_lock);
    live_applied=1;
    applied_index=m50_cube_loaded_index; applied_strength=m50_look_strength;
    applied_tone=m50_tone_cineon; applied_identity=m50_cube_identity;
    snprintf(live_status,sizeof(live_status),"%d library tables graded, %d kept (flags %x), %d dropped",graded,kept,(unsigned)live_flags,(int)live_lost);
}

/* ------------------------------------------------------------------ */
/* Picture-style colour table test (Live LUT preview's library search).  */

/* The M50 picture-style colour table ("Lucky", 10x10x10) has the EOS M
 * layout (ROM default E0B18330: grey axis 0..512, same primary signs). */
static unsigned lutp_index(unsigned r, unsigned g, unsigned b)
{
    return 5*(g%5+5*(b%5))+(b>4 ? 513:0)+(g>4 ? 257:0)+r+(r>4 ? 124:0);
}
static int lutp_chroma(uint32_t v) { v&=1023; return v&512 ? (int)v-1024:(int)v; }
static int lutp_is_table(const uint32_t *t)
{
    if (t[0] || t[lutp_index(9,9,9)]!=0x20000000) return 0;
    unsigned prev=0;
    for (unsigned n=0;n<10;n++) {
        uint32_t w=t[lutp_index(n,n,n)];
        if (w&0xC00FFFFF) return 0;              /* grey: no chroma, top bits clear */
        if ((w>>20)<prev) return 0;
        prev=w>>20;
    }
    uint32_t r=t[lutp_index(8,0,0)], g=t[lutp_index(0,8,0)], b=t[lutp_index(0,0,8)];
    return lutp_chroma(r)>0 && lutp_chroma(r>>10)<0 && lutp_chroma(g)<0 && lutp_chroma(g>>10)<0 &&
           lutp_chroma(b)<0 && lutp_chroma(b>>10)>0;
}
/* ------------------------------------------------------------------ */
/* Live picture display probe (README "Live View crop: the display's    */
/* image layer"). Read only.                                            */

/* Canon's display driver hands the live picture to each output through
 * dispVramCtrl: localSetOnlyParamIMGRepVram ("set vram Panel addr : %08x")
 * calls E01389AC (panel) or E01390E0 (EVF) with a parameter block p (p+8 is
 * the picture address it logs), and those call the image layer setter
 * E0154B64 / E01550F8 with p's words +8..+0x28. Before moving the picture
 * there, this records what Canon passes on the LCD and in the EVF: the
 * first 16 words of p, the other arguments, a count. Both functions start
 * with push.w {r4-r8,sb,lr}; mov r4,r0; adds r0,#0x14 (8 bytes, no pc-
 * relative code); the trampolines replay them. Runs in Canon's display
 * task or its callback: plain stores only. */
int xpan_m50_disp_panel_tramp(void *p, int a1, int a2, int a3, int s0, int s1);
int xpan_m50_disp_evf_tramp(void *p, int a1, int a2, int a3, int s0, int s1);
asm(
    ".text\n.syntax unified\n.thumb\n.align 2\n"
    ".global xpan_m50_disp_panel_tramp\n.thumb_func\n"
    "xpan_m50_disp_panel_tramp:\n"
    "    push.w {r4, r5, r6, r7, r8, r9, lr}\n"   /* E01389AC */
    "    mov r4, r0\n"                            /* E01389B0 */
    "    adds r0, #0x14\n"                        /* E01389B2 */
    "    ldr.w pc, 1f\n"
    ".align 2\n"
    "1:  .word 0xE01389B5\n"                      /* E01389B4, Thumb */
    ".global xpan_m50_disp_evf_tramp\n.thumb_func\n"
    "xpan_m50_disp_evf_tramp:\n"
    "    push.w {r4, r5, r6, r7, r8, r9, lr}\n"   /* E01390E0 */
    "    mov r4, r0\n"                            /* E01390E4 */
    "    adds r0, #0x14\n"                        /* E01390E6 */
    "    ldr.w pc, 2f\n"
    ".align 2\n"
    "2:  .word 0xE01390E9\n"                      /* E01390E8, Thumb */
);
static void disp_record(struct disp_rec *r, const uint32_t *p, int a1, int a2, int a3, int s0, int s1)
{
    r->calls++;
    if (!m50_dbg() || !cfg_ram((uint32_t)p)) return;
    int same=r->a1==(uint32_t)a1 && r->a2==(uint32_t)a2 && r->a3==(uint32_t)a3;
    /* +8: the address, and +0x38: a counter (first probe: +2 per call, it
     * used up the log lines at start) change every call */
    for (int i=0;i<16 && same;i++) if (i!=2 && i!=14 && r->p[i]!=p[i]) same=0;
    r->p[2]=p[2]; r->p[14]=p[14];
    if (same && r->changes) return;
    r->seq++;                                   /* odd while writing (read by XPan's task) */
    asm volatile("dmb" ::: "memory");
    for (int i=0;i<16;i++) r->p[i]=p[i];
    r->a1=a1; r->a2=a2; r->a3=a3; r->s0=s0; r->s1=s1;
    asm volatile("dmb" ::: "memory");
    r->seq++;
    r->changes++;
}
/* 65:24 live picture (README "Canon's main menu breaks Live View: the
 * cause"). Canon's display setters (E01389AC panel, E01390E0 EVF) take a
 * block whose words 5..10 are source x/y, destination x/y and shown w/h; the
 * display hardware crops and moves the picture itself (no copy, no CPU per
 * frame). XPan gives Canon's setter a copy of the block with the 65:24 band:
 * the centred rows the JPEG keeps as the source, the layout's row (Top 0,
 * Split 60, Bottom 214) as the destination; even values, as in Canon's own
 * geometry tables. Canon's own block is never written.
 *
 * When: only inside Canon's transition call. Canon changes Live View
 * geometry only there: a setter call with a2 != 0 when Live View stops or
 * starts, then normal frames (a2 == 0); its own crops (16:9, 4:3, 1:1)
 * arrive that way. The first M50 placement switched on normal frames (at
 * boot, on a layout change): the next Live View stop / start then sometimes
 * never restarted (black screen, frozen Live View, no power-off) or ended in
 * Err 70. So the band is decided on a transition call and kept, unchanged,
 * for every normal frame until the next one; the first frames after the
 * hooks are installed (Live View already running since Canon's own start)
 * keep Canon's geometry until Canon's next transition (menu, playback ...).
 * A block that is not Canon's plain Live View block (magnified, another
 * mode) goes through as Canon's. */
int xpan_photo_get_preview_rect(int width, int height, int *x, int *y, int *crop_width, int *crop_height);   /* cropmarks.c */
int xpan_photo_get_preview_frame_y(int width, int height);                                                  /* cropmarks.c */
/* The band for the current settings: LCD rows (source s, destination d,
 * height h) and the EVF's, all even and inside Canon's 3:2 picture. */
static int band_target(int *s, int *d, int *h, int *es, int *ed, int *eh)
{
    if (!lv || lv_dispsize!=1 || !xpan_photo_enabled() || !xpan_photo_is_panoramic() || is_movie_mode() || xpan_photo_canon_layout()) return 0;
    int x, y, w, hh;
    if (!xpan_photo_get_preview_rect(720,480,&x,&y,&w,&hh)) return 0;
    int dd=xpan_photo_get_preview_frame_y(720,480);
    y&=~1; hh&=~1; dd&=~1;
    if (x || w!=720 || y<0 || hh<2 || y+hh>480 || dd<0 || dd+hh>480) return 0;
    *s=y; *d=dd; *h=hh;
    *eh=((1024*24+65)/130)*2;                   /* 378: 65:24 of 1024 */
    *es=(682-*eh)/2;                            /* 152: centred, as the JPEG */
    *ed=((dd*1024+360)/720)&~1;
    if (*ed+*eh>682) *ed=682-*eh;
    return 1;
}
static int disp_lcd_block(const uint32_t *p)
{
    return cfg_ram((uint32_t)p) && p[0]==1 && p[3]==736 && p[4]==480 && !p[5] && !p[6] && !p[7] && !p[8]
        && p[9]==720 && p[10]==480;
}
static int disp_evf_block(const uint32_t *p)
{
    return cfg_ram((uint32_t)p) && p[0]==1 && p[1]==1 && p[3]==1024 && p[4]==768 && !p[5] && !p[6] && !p[7] && !p[8]
        && p[9]==1024 && p[10]==682;
}
/* Per output (0 panel, 1 EVF): the geometry in force since Canon's last
 * transition call. Written by Canon's display task only (the setter), read
 * by the touch / AF code. */
struct band_latch { volatile int mv; int s, d, h; volatile unsigned transitions; };
static struct band_latch band_latch[2];
static void band_decide(struct band_latch *l, int k)
{
    int s=0, d=0, h=0, es=0, ed=0, eh=0;
    int mv=band_target(&s,&d,&h,&es,&ed,&eh);
    if (k) { s=es; d=ed; h=eh; }
    l->s=s; l->d=d; l->h=h;
    asm volatile("dmb" ::: "memory");
    l->mv=mv;
    l->transitions++;
}
/* The band as the panel shows it now (touch and AF code): 0 when the
 * picture is Canon's. */
static int disp_band(int *src_y, int *dst_y, int *h)
{
    const struct band_latch *l=&band_latch[0];
    if (!l->mv || !lv || lv_dispsize!=1) return 0;
    *src_y=l->s; *dst_y=l->d; *h=l->h;
    return 1;
}
static void disp_mark(int id, const uint32_t *p, int a2, int a3, int moved)
{
    static int last[2]={-1,-1};
    int k=id-74, ok=cfg_ram((uint32_t)p);
    int v=(a2&0xF)|((a3&0xF)<<4)|(moved ? 1<<7:0)|(ok ? (int)((p[6]&0x3FF)<<8) | (int)((p[10]&0x3FF)<<20):0);
    if (v!=last[k]) { last[k]=v; m50_boot_mark(id,v); }
}
/* UI-lock log (read only, README "Forced restart: camera test"): how long
 * Canon's display setter pauses around a Live View stop / start. */
static void disp_gap(int k)
{
    if (!m50_dbg()) return;
    static uint32_t last[2];
    uint32_t now=get_ms_clock();
    if (last[k] && now-last[k]>100) m50_boot_mark(76,(int)((now-last[k])&0xFFFFFF)|(k<<24));
    last[k]=now;
}
static int disp_hook(int k, void *p, int a2, uint32_t q[16])
{
    struct band_latch *l=&band_latch[k];
    if (a2) band_decide(l,k);                   /* Canon's transition: the only place the geometry changes */
    int blk=k ? disp_evf_block((const uint32_t *)p) : disp_lcd_block((const uint32_t *)p);
    /* Only while Canon shows Live View: quick review and playback use the
     * same block (camera test 2026-11-11: the review image sat at the band's
     * place, playback images came up stretched until Canon's next
     * transition call). */
    int lv_shown=lv && !PLAY_OR_QR_MODE;
    int mv=blk && l->mv && lv_shown;
    if (mv) { memcpy(q,p,16*sizeof(uint32_t)); q[6]=l->s; q[8]=l->d; q[10]=l->h; }
    return mv;
}
int xpan_m50_disp_panel(void *p, int a1, int a2, int a3, int s0, int s1)
{
    disp_record(&disp_recs[0],(const uint32_t *)p,a1,a2,a3,s0,s1);
    disp_gap(0);
    uint32_t q[16];
    int mv=disp_hook(0,p,a2,q);
    disp_mark(74,(const uint32_t *)p,a2,a3,mv);
    return xpan_m50_disp_panel_tramp(mv ? (void *)q : p,a1,a2,a3,s0,s1);
}
int xpan_m50_disp_evf(void *p, int a1, int a2, int a3, int s0, int s1)
{
    disp_record(&disp_recs[1],(const uint32_t *)p,a1,a2,a3,s0,s1);
    disp_gap(1);
    uint32_t q[16];
    int mv=disp_hook(1,p,a2,q);
    disp_mark(75,(const uint32_t *)p,a2,a3,mv);
    return xpan_m50_disp_evf_tramp(mv ? (void *)q : p,a1,a2,a3,s0,s1);
}

/* Canon's debug log at the menu hang (README "Canon's own debug log at the
 * hang"): Canon announces the Live View start (PROP_LV_ACTION 0); if its
 * display setter then gets no call for 2.5 s, XPan asks Canon to save its
 * own stored debug messages (event procedure "dumpf": Canon's log task
 * writes the file; nothing waits, nothing allocated). Once per start, plus
 * on request from the menu (a good cycle, to compare). */
static volatile unsigned lv_start_calls;
static volatile uint32_t lv_start_ms;
static volatile int lv_start_pending, canon_dump_request;
static int canon_dumped;
static unsigned canon_dumps;
/* XPan task, every 50 ms. */
/* Test of the MMU patch fix (README "Menu hang: Canon's own log"): one
 * Canon log per start, 12 s after boot, to check that ML's start no longer
 * leaves Live View errors (Blanking DeadLine, OVERRUN, imgCtrl). */
#define M50_BOOT_DUMP 0      /* test of the stall fix done: no errors in 3 starts */
static void canon_dump_step(void)
{
    if (!m50_dbg()) { lv_start_pending=0; canon_dump_request=0; return; }
#if M50_BOOT_DUMP
    static int boot_dumped;
    if (!boot_dumped && get_ms_clock()>12000) {
        boot_dumped=1; canon_dumps++;
        m50_boot_mark(81,3);
        call("dumpf");
        return;
    }
#endif
    if (canon_dump_request) {
        canon_dump_request=0;
        canon_dumps++;
        m50_boot_mark(81,2);
        m50_log_tasks();                /* a good state, to compare */
        call("dumpf");
        return;
    }
    if (!lv_start_pending) return;
    if (disp_recs[0].calls+disp_recs[1].calls-lv_start_calls>=3) { lv_start_pending=0; return; }
    if (get_ms_clock()-lv_start_ms<2500) return;
    lv_start_pending=0;
    if (canon_dumped) return;
    canon_dumped=1; canon_dumps++;
    m50_boot_mark(81,1);
    call("dumpf");
}
/* Stuck power-off (camera test 2026-11-11: black screen after playback in
 * the Canon layout, then the power-off never ended, LED on, battery pull;
 * the earlier menu hang's Canon log had "[PO] ERROR LvCtrl ResTimeout"):
 * with Debug info ON, if the camera is still running 5 s after the power-off
 * started, XPan asks Canon to save its debug log, once. Canon keeps its
 * recent messages in RAM, so the log covers the hang before it. Only Canon's
 * own log writer touches the card; a normal power-off ends before 5 s.
 * XPan task, while powering off. Returns 1 once done (or Debug info OFF). */
/* Every task's state, into Canon's own debug log (DryosDebugMsg, no card
 * access), just before Canon saves it: name, state, wait type and object,
 * saved PC/LR and the code addresses found on its stack (Canon's ROM or
 * ML's text). Camera test 2026-11-11 (LOG010.LOG): after playback -> Live
 * View in the Canon layout, GuiMainTask and ML's shoot_task both stopped at
 * the same moment while Live View kept running; this shows what each one
 * waits for. Only reads DryOS's task records. */
#include "tasks.h"
extern unsigned int task_max;
extern struct semaphore *winsys_sem;
extern void *bmp_lock;
extern char _text_start[], _text_end[];
static int m50_code_addr(uint32_t w)
{
    if (!(w&1)) return 0;
    return (w>=0xE0000000u && w<0xE2000000u) || (w>=(uint32_t)_text_start && w<(uint32_t)_text_end);
}
/* Canon's log keeps about 60 characters and a few values per message
 * (LOG011.LOG 2026-11-11: longer lines were cut), so each record is split
 * into short lines: XPT state, XPC PC/LR/full task id, XPS stack code
 * addresses (tasks waiting on a semaphore, flag, queue send or mutex, and
 * ML's tasks), XPM the first words of each mutex a task waits on (its
 * owner), XPR where ML's redraw is (zebra.c _redraw_do). */
static int m50_task_detail(const struct task_attr_str *a)
{
    const char *n=a->name;
    if (n && n[0]>='a' && n[0]<='z') return 1;                 /* ML's tasks */
    return a->state && a->wait_id!=0 && a->wait_id!=4;          /* waits on more than a sleep or its queue */
}
static void m50_log_tasks(void)
{
    extern volatile int ml_redraw_stage;
    extern volatile uint32_t ml_redraw_stage_ms;
    DryosDebugMsg(DM_MAGIC,3,"XPAN tasks %d cur %s",(int)task_max,get_current_task_name());
    DryosDebugMsg(DM_MAGIC,3,"XPAN locks winsys %x bmp %x",(unsigned)winsys_sem,(unsigned)bmp_lock);
    DryosDebugMsg(DM_MAGIC,3,"XPR stage %d for %d ms",ml_redraw_stage,(int)(get_ms_clock()-ml_redraw_stage_ms));
    uint32_t mutexes[8]; int nm=0;
    for (int id=1; id<=(int)task_max && id<256; id++) {
        struct task_attr_str a;
        if (get_task_info_by_id(1,id,&a)) continue;
        const struct task *t=first_task+id;
        DryosDebugMsg(DM_MAGIC,3,"XPT %d %s s%d w%d o%x",id,a.name ? a.name : "?",(int)a.state,(int)a.wait_id,(unsigned)t->waitObjId);
        if (a.state && a.wait_id==6 && t->waitObjId && nm<8) {
            int k=0; while (k<nm && mutexes[k]!=t->waitObjId) k++;
            if (k==nm) mutexes[nm++]=t->waitObjId;
        }
        if (!m50_task_detail(&a)) continue;
        uint32_t pc=0, lr=0, code[8]={0};
        int n=0;
        uint32_t c=(uint32_t)a.context, lo=a.stack, hi=a.stack+a.size;
        if (c && !(c&3) && c>=lo && c+0x40<=hi) {
            pc=a.context->pc; lr=a.context->lr;
            for (uint32_t p=c+0x40; p+4<=hi && p<c+0x40+96*4 && n<8; p+=4) {
                uint32_t w=*(const uint32_t *)p;
                if (m50_code_addr(w)) code[n++]=w;
            }
        }
        DryosDebugMsg(DM_MAGIC,3,"XPC %d %x %x id %x",id,(unsigned)pc,(unsigned)lr,(unsigned)t->taskId);
        DryosDebugMsg(DM_MAGIC,3,"XPS %d %x %x %x %x",id,(unsigned)code[0],(unsigned)code[1],(unsigned)code[2],(unsigned)code[3]);
        DryosDebugMsg(DM_MAGIC,3,"XPS %d %x %x %x %x",id,(unsigned)code[4],(unsigned)code[5],(unsigned)code[6],(unsigned)code[7]);
    }
    for (int k=0;k<nm;k++) {
        const volatile uint32_t *o=(const volatile uint32_t *)mutexes[k];
        if ((uint32_t)o<0x1000 || (uint32_t)o>=0x40000000u || ((uint32_t)o&3)) continue;
        DryosDebugMsg(DM_MAGIC,3,"XPM %x %x %x %x %x",(unsigned)o,(unsigned)o[0],(unsigned)o[1],(unsigned)o[2],(unsigned)o[3]);
        DryosDebugMsg(DM_MAGIC,3,"XPM %x %x %x %x %x",(unsigned)o,(unsigned)o[4],(unsigned)o[5],(unsigned)o[6],(unsigned)o[7]);
    }
}
static int canon_dumped_off;
static int canon_dump_stuck_off(void)
{
    static uint32_t off_ms;
    if (canon_dumped_off || !m50_dbg()) return 1;
    if (!m50_shutdown_seen && !ml_shutdown_requested) { off_ms=0; return 0; }
    if (!off_ms) { off_ms=get_ms_clock()|1; return 0; }
    if (get_ms_clock()-off_ms<5000) return 0;
    canon_dumped_off=1; canon_dumps++;
    m50_log_tasks();
    call("dumpf");
    return 1;
}
static MENU_SELECT_FUNC(canon_dump_select) { (void)priv; (void)delta; canon_dump_request=1; }
static MENU_UPDATE_FUNC(canon_dump_display)
{
    MENU_SET_VALUE("%s",canon_dumped ? "saved at a hang" : "");
    MENU_SET_RINFO("%d",(int)canon_dumps);
}

/* UI-lock log: Canon's UI lock and its Live View stop / start
 * announcements, as Canon sends them (read only, PropMgr task). */
static void m50_uilock_prop(unsigned property, void *token, void *data, unsigned len)
{
    (void)token;
    static unsigned n;
    if (!len) return;
    if (property==PROP_LV_ACTION && *(const uint32_t *)data==0) {
        lv_start_calls=disp_recs[0].calls+disp_recs[1].calls;
        lv_start_ms=get_ms_clock();
        lv_start_pending=1;
    }
    if (n>=200) return;
    n++;
    m50_boot_mark(property==PROP_ICU_UILOCK ? 79:80,(int)*(const uint32_t *)data);
}
REGISTER_PROP_HANDLER_EX(PROP_ICU_UILOCK, m50_uilock_prop, 0);
REGISTER_PROP_HANDLER_EX(PROP_LV_ACTION, m50_uilock_prop, 0);

/* Forced Live View restart (README "Canon's main menu breaks Live View:
 * forced restart"). The band changes only on Canon's transition call, and
 * the one Live View restart XPan can ask Canon for is its menu
 * (Canon makes the transition call when the menu closes). When the band on
 * the active output differs from the settings (power-on, a layout or
 * 65:24 / 3:2 change) for 1 s with Canon idle, XPan presses MENU through
 * Canon's own button queue (fake_simple_button: Canon's IDLEHandler then
 * does what it does for the real button), and presses it again once Canon
 * is in its menu and its Live View has stopped (no display setter call for
 * 300 ms). SetGUIRequestMode from XPan's task skipped Canon's button
 * handling and froze Live View (766c9f6c6f54). Once per change: the same
 * target is never tried twice; a step that does not come within 3 s (a
 * bound) ends the forced restarts until the next start. */
static CONFIG_INT("xpan.m50.lv_restart", m50_lv_restart, 1);
enum { BR_IDLE, BR_OPEN, BR_CLOSE, BR_OFF };
static int br_state, br_k;
static uint32_t br_ms, br_stable_ms, br_key, br_tried=0xFFFFFFFFu;
static unsigned br_trans;
static volatile unsigned br_done, br_failed;
/* 1 when output k shows the band the settings want; *key names that band. */
static int band_check(int k, uint32_t *key)
{
    int s=0, d=0, h=0, es=0, ed=0, eh=0;
    int mv=band_target(&s,&d,&h,&es,&ed,&eh);
    if (k) { s=es; d=ed; h=eh; }
    if (!mv) s=d=h=0;
    *key=((uint32_t)k<<31)|((uint32_t)mv<<30)|((uint32_t)(s&0x3FF)<<20)|((uint32_t)(d&0x3FF)<<10)|(uint32_t)(h&0x3FF);
    const struct band_latch *l=&band_latch[k];
    return mv ? l->mv && l->s==s && l->d==d && l->h==h : !l->mv;
}
/* The output Canon is drawing Live View on: 0 panel, 1 EVF, -1 unclear. */
static int band_active_output(void)
{
    static unsigned last0, last1;
    static uint32_t ms;
    static int act=-1;
    uint32_t now=get_ms_clock();
    if (now-ms<500) return act;
    unsigned c0=disp_recs[0].calls, c1=disp_recs[1].calls;
    int p=c0!=last0, e=c1!=last1;
    last0=c0; last1=c1; ms=now;
    act=p && !e ? 0 : e && !p ? 1 : -1;
    return act;
}
/* XPan task, every 50 ms. */
static void band_restart_step(void)
{
    uint32_t now=get_ms_clock();
    static unsigned quiet_calls;
    static uint32_t quiet_ms;
    unsigned calls=disp_recs[0].calls+disp_recs[1].calls;
    if (calls!=quiet_calls) { quiet_calls=calls; quiet_ms=now; }
    if (br_state==BR_OPEN) {
        /* In Canon's menu, Live View stopped: as when the user presses MENU again. */
        if (MENU_MODE && !lv && now-quiet_ms>=300) {
            br_trans=band_latch[br_k].transitions;
            fake_simple_button(BGMT_MENU);
            br_state=BR_CLOSE; br_ms=now;
            m50_boot_mark(78,2|(br_k<<8));
        } else if (now-br_ms>3000) {
            br_state=BR_OFF; br_failed++;
            m50_boot_mark(78,16|(br_k<<8)|((CURRENT_GUI_MODE&0xFF)<<12)|((gui_state&0xFF)<<20)|(lv ? 1<<28:0));
        }
        return;
    }
    if (br_state==BR_CLOSE) {
        if (lv && band_latch[br_k].transitions!=br_trans) {
            uint32_t key;
            int ok=band_check(br_k,&key);
            br_state=BR_IDLE; br_done++; br_stable_ms=0;
            m50_boot_mark(78,3|(ok ? 4:0)|(br_k<<8));
        } else if (now-br_ms>3000) {
            br_state=BR_OFF; br_failed++;
            m50_boot_mark(78,32|(br_k<<8));
        }
        return;
    }
    if (br_state!=BR_IDLE) return;
    int k=band_active_output();
    int idle=m50_lv_restart && k>=0 && lv && lv_dispsize==1 && !is_movie_mode() && xpan_photo_ui_active()
        && gui_state==GUISTATE_IDLE && !gui_menu_shown() && !xpan_photo_canon_layout() && !canon_layout_state
        && !canon_ui_frozen && !get_halfshutter_pressed() && !lens_info.job_state && NOT_RECORDING;
    uint32_t key=0;
    if (!idle || band_check(k,&key) || key==br_tried) { br_stable_ms=0; return; }
    if (!br_stable_ms || key!=br_key) { br_key=key; br_stable_ms=now ? now:1; return; }
    if (now-br_stable_ms<1000) return;
    br_tried=key; br_k=k; br_stable_ms=0;
    br_state=BR_OPEN; br_ms=now;
    m50_boot_mark(78,1|(k<<8));
    fake_simple_button(BGMT_MENU);
}
static MENU_UPDATE_FUNC(lv_restart_display)
{
    MENU_SET_VALUE("%s",!m50_lv_restart ? "OFF" : br_state==BR_OFF ? "stopped" : "ON");
    MENU_SET_RINFO("%d/%d",(int)br_done,(int)br_failed);
    MENU_SET_ENABLED(m50_lv_restart);
}

/* ------------------------------------------------------------------ */
/* Touch AF and Canon's AF box probe (README "Touch AF and Canon's AF    */
/* box"). Read only.                                                    */

/* Canon's AF frame state machine (GmtWakuState.c, wkTouchMoveAFFrame)
 * converts a touch into the AF frame position through E04235CC/E04236A2,
 * which both call E0423406(frame, r1, r2, r3, 7 stack words): it adds the
 * picture area (a table in sensor units: X 276, W 6000, Y 48, H 4000 in the
 * RAM dump) to the touch position given in s1/s2 and writes the frame
 * (frame+8 / +0xA). Since the picture moved, Canon's box and touch AF are
 * off by the band's move. This records, for each call, the arguments and
 * the frame it wrote, with the last tap's screen position, to choose where
 * the correction goes. E0423406 starts with push.w {r0-r11,lr}; movs r6,#0;
 * sub sp,#0x14 (8 bytes, no pc-relative code); the trampoline replays them.
 * Also Canon's AF frame property as ML sees it (shoot.c get_afframe_rect). */
struct af_rec { uint32_t ms, tap_ms; int tap_x, tap_y, tap_blocked; uint32_t r1, r2, r3, s[7]; uint16_t before[8], after[8]; };
#define AF_RECS 8
static struct af_rec af_recs[AF_RECS];
static volatile unsigned af_head, af_saved, af_calls;
int xpan_m50_af_conv_tramp(void *frame, int r1, int r2, int r3, int s0, int s1, int s2, int s3, int s4, int s5, int s6);
asm(
    ".text\n.syntax unified\n.thumb\n.align 2\n"
    ".global xpan_m50_af_conv_tramp\n.thumb_func\n"
    "xpan_m50_af_conv_tramp:\n"
    "    push.w {r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, lr}\n"   /* E0423406 */
    "    movs r6, #0\n"                                                     /* E042340A */
    "    sub sp, #0x14\n"                                                   /* E042340C */
    "    ldr.w pc, 1f\n"
    ".align 2\n"
    "1:  .word 0xE042340F\n"                                                /* E042340E, Thumb */
);
/* Bottom layout (camera test 2026-11-10: taps read as if at the top): the
 * band moves down there, so the area top would go negative, and Canon keeps
 * it unsigned 16-bit (E04266A4: uxth). The area getter then stays Canon's,
 * and for a tap XPan passed to Canon in the last 500 ms the converter gets
 * the tap row moved back by the band's move (s2, sensor units) instead:
 * focus lands on what was tapped. Canon's box keeps the unmoved row there. */
static volatile int touch_delta_s;
int xpan_m50_area_cy(void);             /* below: also works out touch_delta_s */
int xpan_m50_af_conv(void *frame, int r1, int r2, int r3, int s0, int s1, int s2, int s3, int s4, int s5, int s6)
{
    af_calls++;
    unsigned h=af_head;
    struct af_rec *r=&af_recs[h%AF_RECS];
    int ok=m50_dbg() && cfg_ram((uint32_t)frame);   /* records only with Debug info */
    r->ms=get_ms_clock(); r->tap_ms=af_tap_ms; r->tap_x=af_tap_x; r->tap_y=af_tap_y; r->tap_blocked=af_tap_blocked;
    r->r1=r1; r->r2=r2; r->r3=r3;
    r->s[0]=s0; r->s[1]=s1; r->s[2]=s2; r->s[3]=s3; r->s[4]=s4; r->s[5]=s5; r->s[6]=s6;
    for (int i=0;i<8;i++) r->before[i]=ok ? ((const uint16_t *)frame)[i] : 0;
    (void)xpan_m50_area_cy();           /* the current layout's correction, not a stale one */
    int ty=s2, td=touch_delta_s;
    if (td>0 && !af_tap_blocked && get_ms_clock()-af_tap_ms<500) ty=s2>td ? s2-td : 0;
    int ret=xpan_m50_af_conv_tramp(frame,r1,r2,r3,s0,s1,ty,s3,s4,s5,s6);
    for (int i=0;i<8;i++) r->after[i]=ok ? ((const uint16_t *)frame)[i] : 0;
    asm volatile("dmb" ::: "memory");
    if (ok) af_head=h+1;
    return ret;
}

/* Picture area centre row (README "Touch AF and Canon's AF box"). Canon's
 * AF frame code works in sensor units over a picture area table
 * (*(*0x7144+0x28): +8 top 48, +0xA height 4000 in the RAM dump); its
 * centre-row getter E042667C feeds the area top (E04266A4) used by the
 * touch converter E0423406 (probe 2026-11-10: frame row = tap row x 4000/480
 * + top). With the 65:24 band moved by the display (dst - src rows), this
 * returns the centre moved the other way, in sensor units: a tap then maps
 * to the scene point under it, and Canon draws its AF box on the moved
 * picture (screen row = (frame - top) x 480/4000). Same delta in the EVF
 * (the band is moved the same way, scaled). 0 when the picture is not moved.
 * The trampoline runs Canon's first four instructions with the table
 * pointer's literal value (E0426A68 = 0x7144) and continues at E0426684. */
static volatile int area_delta_s;
int xpan_m50_area_cy_tramp(void);
asm(
    ".text\n.syntax unified\n.thumb\n.align 2\n"
    ".global xpan_m50_area_cy_tramp\n.thumb_func\n"
    "xpan_m50_area_cy_tramp:\n"
    "    ldr r0, 1f\n"                   /* E042667C: ldr r0,[pc,#0x3e8] = 0x7144 */
    "    ldr r0, [r0, #0x28]\n"          /* E042667E */
    "    ldrh r1, [r0, #8]\n"            /* E0426680 */
    "    ldrh r0, [r0, #0xa]\n"          /* E0426682 */
    "    ldr.w pc, 2f\n"
    ".align 2\n"
    "1:  .word 0x00007144\n"
    "2:  .word 0xE0426685\n"             /* E0426684, Thumb: add.w r0,r1,r0,lsr #1; uxth; bx lr */
);
int xpan_m50_area_cy(void)
{
    int c=xpan_m50_area_cy_tramp();
    int s, d, h;
    if (!disp_band(&s,&d,&h)) { area_delta_s=0; touch_delta_s=0; return c; }
    const uint16_t *a=*(const uint16_t * const *)(0x7144+0x28);
    int H=cfg_ram((uint32_t)a) ? a[5] : 0;
    if (H<480 || H>8000) { area_delta_s=0; touch_delta_s=0; return c; }
    int ds=(d-s)*H/480;                 /* screen rows the band moved, in sensor units */
    /* The area top (centre - H/2) is unsigned in Canon's code: only a move
     * that keeps it at 0 or above goes here (Top, Split); a larger downward
     * move (Bottom) is corrected on the tap instead (touch_delta_s). */
    if (c-ds-H/2<0) { area_delta_s=0; touch_delta_s=ds; return c; }
    area_delta_s=ds; touch_delta_s=0;
    return c-ds>0xFFFF ? 0xFFFF : c-ds;
}

/* AF box on screen, Bottom layout (README "Touch AF and Canon's AF box").
 * First try (E0425318, the rectangle relative to the area) moved 9
 * rectangles per tap and the box did not move: not the drawing. The box
 * comes from E042268A(out, r1, r2, mode, s0, s1, frame): it writes the
 * frame's on-screen rectangle relative to the picture area: out+4/+6 = x/y
 * (frame - size/2 - area origin), out+8/+0xA = size (halfwords), using the
 * area top getter, which is why moving the area fixed Top and Split. In
 * Bottom the area cannot move (its top would go below 0), so the row Canon
 * hands back (out+6) is moved down by the band's move (touch_delta_s,
 * sensor units). Not for its caller in Canon's AF area calculation
 * (E042279A, returns to E04227CB): the focus area keeps Canon's values
 * (focus already lands right in Bottom). The function starts with push.w
 * {r2-r11,ip,lr}; mov r4,r0; mov r8,r3 (8 bytes, no pc-relative code). */
int xpan_m50_af_box_tramp(void *out, int r1, int r2, int r3, int s0, int s1, int s2);
asm(
    ".text\n.syntax unified\n.thumb\n.align 2\n"
    ".global xpan_m50_af_box_tramp\n.thumb_func\n"
    "xpan_m50_af_box_tramp:\n"
    "    push.w {r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, lr}\n"   /* E042268A */
    "    mov r4, r0\n"                                                  /* E042268E */
    "    mov r8, r3\n"                                                  /* E0422690 */
    "    ldr.w pc, 1f\n"
    ".align 2\n"
    "1:  .word 0xE0422693\n"                                            /* E0422692, Thumb */
);
struct box_caller { uint32_t lr; volatile unsigned n, moved; };
#define BOX_CALLERS 8
static struct box_caller box_callers[BOX_CALLERS];
static volatile unsigned box_ncallers;
int xpan_m50_af_box(void *out, int r1, int r2, int r3, int s0, int s1, int s2)
{
    uint32_t lr=(uint32_t)__builtin_return_address(0);
    int ret=xpan_m50_af_box_tramp(out,r1,r2,r3,s0,s1,s2);
    (void)xpan_m50_area_cy();             /* the current layout's correction */
    int td=touch_delta_s, moved=0;
    if (td>0 && lr!=0xE04227CB && cfg_ram((uint32_t)out)) {
        int16_t *row=(int16_t *)((uint8_t *)out+6);
        *row=(int16_t)(*row+td);
        moved=1;
    }
    unsigned n=box_ncallers, i;
    for (i=0;i<n && box_callers[i].lr!=lr;i++) ;
    if (i==n && n<BOX_CALLERS) { box_callers[n].lr=lr; asm volatile("dmb" ::: "memory"); box_ncallers=n+1; }
    if (i<BOX_CALLERS) { box_callers[i].n++; if (moved) box_callers[i].moved++; }
    return ret;
}

/* ------------------------------------------------------------------ */
/* Hilton probe (README "Still processing path: where a LUT could act   */
/* earlier"). Read only.                                                */

/* Canon's black level / white balance stage on the 14-bit Bayer data takes
 * its registers from two blocks of the shot's parameters: HiltonObwbReg (21
 * words: clip level, black offsets, enables, the four gains, gain shifts,
 * a third word per channel) and HiltonCorrReg (13 words). Canon's
 * E08DB920(obwb, corr) hands them to the register writers:
 *     if (obwb) E08E56F6(obwb);
 *     if (corr) return E08E57AA(corr);
 * called from the normal pre-develop (E08AF01A, through E08C7FC4) and the
 * recover pre-develop (E08B02BA, through E08C8A54), stills only.
 * XPan's function does exactly that, with Canon's own pointers, after
 * copying the two blocks for the log (one record per caller). Nothing of
 * Canon's is written; no blocking, no card, no allocation. */
struct hil_rec { uint32_t lr; volatile unsigned calls, seq; uint32_t ms, obwb_addr, corr_addr, obwb[21], corr[13]; int stops; };
#define HIL_CALLERS 4
static struct hil_rec hil_recs[HIL_CALLERS];
static volatile unsigned hil_ncallers, hil_other;
static int hil_readable(uint32_t a)
{
    return !(a&3) && ((a>=0x1000 && a<0x80000000u-0x60) || (a>=0xE0000000u && a<0xE2000000u-0x60));
}
/* Highlight headroom, calibration test (README "Highlight headroom on the
 * 14-bit data"): Canon's writer gets a copy of the shot's OBWB block with
 * the four white balance gains (words 9-12, 0x400 = 1.0) lowered by 1 or 2
 * EV, so the linear data stays below Canon's clip and the highlights reach
 * the colour table. Same white balance (all four gains alike), Canon's black
 * level and clip unchanged, Canon's block never written. Only for XPan's
 * stills, only for the layout the probe logged (white 8000..16383, enables
 * 1, gains 0x100..0xFFF, gain shifts 0). Canon's writer copies the block to
 * the hardware registers (D0435000) during the call, so the copy only has to
 * live for that call; one still develop at a time.
 * Camera test 2026-11-10 (README "Highlight headroom on the 14-bit data"):
 * -1 EV holds all the sensor's highlights. So it is part of the tone base
 * "Cineon log +1 EV", and only when that photo will get the M50's CIN1EV
 * base: the same conditions as the colour-table hook's (Custom LUT on, a LUT
 * loaded, the base loaded, no Live LUT preview, XPan's stills); the hook
 * then picks CIN1EV for this photo (hil_shot_lowered). */
static uint32_t hil_copy[21];
static int hil_layout_ok(const uint32_t *b)
{
    if (b[0]<8000 || b[0]>16383) return 0;
    for (int k=5;k<9;k++) if (b[k]!=1) return 0;
    for (int k=9;k<13;k++) if (b[k]<0x100 || b[k]>0xFFF) return 0;
    for (int k=13;k<17;k++) if (b[k]) return 0;
    return 1;
}
static void hil_record(const uint32_t *obwb, const uint32_t *corr, uint32_t lr, int stops)
{
    if (!m50_dbg()) return;
    unsigned n=hil_ncallers, i;
    for (i=0;i<n && hil_recs[i].lr!=lr;i++) ;
    if (i==n) {
        if (n>=HIL_CALLERS) { hil_other++; return; }
        hil_recs[n].lr=lr;
        asm volatile("dmb" ::: "memory");
        hil_ncallers=n+1;
    }
    struct hil_rec *r=&hil_recs[i];
    r->seq++;                                   /* odd while writing (read by XPan's task) */
    asm volatile("dmb" ::: "memory");
    r->ms=get_ms_clock(); r->obwb_addr=(uint32_t)obwb; r->corr_addr=(uint32_t)corr; r->stops=stops;
    int ro=hil_readable((uint32_t)obwb), rc=hil_readable((uint32_t)corr);
    for (int k=0;k<21;k++) r->obwb[k]=ro ? obwb[k] : 0;
    for (int k=0;k<13;k++) r->corr[k]=rc ? corr[k] : 0;
    asm volatile("dmb" ::: "memory");
    r->seq++;
    r->calls++;
}
int xpan_m50_hilton(void *obwb, void *corr)
{
    typedef int (*set_fn)(void *block);
    const set_fn canon_obwb=(set_fn)0xE08E56F7;        /* Thumb */
    const set_fn canon_corr=(set_fn)0xE08E57AB;        /* Thumb */
    const uint32_t *b=(const uint32_t *)obwb;
    void *use=obwb;
    int stops=0;
    hil_shot_lowered=0;
    if (m50_tone_cineon==2 && obwb) {
        int base_ok=m50_look_on && m50_cube_valid && m50_bases[0].valid && m50_bases[1].valid && !live_applied;
        if (base_ok && hil_readable((uint32_t)b) && xpan_photo_enabled() && !is_movie_mode() && hil_layout_ok(b)) {
            stops=1;
            for (int k=0;k<21;k++) hil_copy[k]=b[k];
            for (int k=9;k<13;k++) hil_copy[k]=(b[k]+(1u<<(stops-1)))>>stops;
            asm volatile("dmb" ::: "memory");
            use=hil_copy; hil_lowered++;
            hil_shot_lowered=1;                 /* the colour-table hook of this photo: CIN1EV */
        } else hil_refused++;
    }
    hil_record(b,(const uint32_t *)corr,(uint32_t)__builtin_return_address(0),stops);
    int ret=(int)obwb;                                 /* Canon leaves r0 as it is without a block */
    if (use) ret=canon_obwb(use);
    if (corr) return canon_corr(corr);
    return ret;
}

/* XPan task: the log lines, written only when Canon is idle (no half-press,
 * no shot in progress, events at least 2 s old). Also Canon's display layer
 * (bmp.c, redraw_task) and the first 48 words of Canon's Ximr context when
 * they change and then stay the same for 1 s (EVF study: the HUD sits lower
 * in the EVF; at most 8 per start). */
extern volatile unsigned rgb_layer_changes, rgb_layer_skips;
extern volatile uint32_t rgb_layer_marv, rgb_layer_data, rgb_layer_w, rgb_layer_h;
#define XIMR_WORDS 48
static void m50_log_poll(void)
{
    if (!m50_dbg()) return;
    static uint32_t ximr_hash, ximr_logged, ximr_since;
    static int ximr_logs, started;
    static unsigned layer_seen, lost_logged;
    uint32_t now=get_ms_clock();
    const volatile uint32_t *x=(const volatile uint32_t *)XIMR_CONTEXT;
    uint32_t h=2166136261u;
    for (int i=0;i<XIMR_WORDS;i++) h=(h^x[i])*16777619u;
    if (h!=ximr_hash) { ximr_hash=h; ximr_since=now; }
    int layer_new=rgb_layer_changes!=layer_seen;
    int ximr_new=ximr_logs<8 && ((h!=ximr_logged && now-ximr_since>1000) || layer_new);   /* with each layer change too */
    int photo_new=m50_photo_ring.head!=m50_photo_ring.saved, grain_new=m50_grain_ring.head!=m50_grain_ring.saved;
    int ui_new=m50_ui_ring.head!=m50_ui_ring.saved || m50_mode_ring.head!=m50_mode_ring.saved;
    int lost_new=live_lost!=lost_logged;
    static unsigned disp_seen[2];
    static int disp_logs;
    int disp_new=disp_logs<16 && (disp_recs[0].changes!=disp_seen[0] || disp_recs[1].changes!=disp_seen[1]);
    int af_new=af_head!=af_saved;
    static int aff_last[6]={-1,-1,-1,-1,-1,-1};
    int aff[6]={0,0,0,0,0,0};
    extern int get_afframe_rect(int *x, int *y, int *w, int *h, int *W, int *H);
    int aff_ok=get_afframe_rect(&aff[0],&aff[1],&aff[2],&aff[3],&aff[4],&aff[5]);
    int aff_new=aff_ok && memcmp(aff,aff_last,sizeof(aff));
    static unsigned taps_seen;
    int tap_new=af_taps!=taps_seen;
    static unsigned hil_seen[HIL_CALLERS];
    static int hil_logs;
    int hil_new=0;
    for (unsigned k=0;k<hil_ncallers && k<HIL_CALLERS;k++) if (hil_logs<24 && hil_recs[k].calls!=hil_seen[k]) hil_new=1;
    if (!ximr_new && !layer_new && !photo_new && !grain_new && !ui_new && !lost_new && !disp_new && !af_new && !aff_new && !tap_new && !hil_new) return;
    if (get_halfshutter_pressed() || lens_info.job_state || RECORDING) return;
    if (photo_new && now-m50_photo_ring.ev[(m50_photo_ring.head-1)%M50_EVS].ms<2000) return;
    if (grain_new && now-m50_grain_ring.ev[(m50_grain_ring.head-1)%M50_EVS].ms<2000) return;
    FILE *f=FIO_OpenFile("ML/LOGS/XM50.LOG",O_RDWR|O_SYNC);
    if (f) FIO_SeekSkipFile(f,0,SEEK_END);
    else f=FIO_CreateFile("ML/LOGS/XM50.LOG");
    if (!f) return;
    char line[160];
    int len;
    if (!started) {
        started=1;
        len=snprintf(line,sizeof(line),"=== start (%d ms)\n",(int)now);
        FIO_WriteFile(f,line,len);
    }
    if (layer_new) {
        layer_seen=rgb_layer_changes;
        len=snprintf(line,sizeof(line),"%8d display layer: MARV %08x data %08x %dx%d, %d changes, %d refreshes skipped (layer under 960x540)\n",
            (int)now,(unsigned)rgb_layer_marv,(unsigned)rgb_layer_data,(int)rgb_layer_w,(int)rgb_layer_h,
            (int)rgb_layer_changes,(int)rgb_layer_skips);
        FIO_WriteFile(f,line,len);
    }
    if (ximr_new) {
        ximr_logged=h; ximr_logs++;
        for (int k=0;k<XIMR_WORDS;k+=12) {
            if (k) len=snprintf(line,sizeof(line),"                    +%02x:",k*4);
            else len=snprintf(line,sizeof(line),"%8d Ximr %08x +00:",(int)now,(unsigned)XIMR_CONTEXT);
            for (int i=k;i<k+12;i++) len+=snprintf(line+len,sizeof(line)-len," %x",(unsigned)x[i]);
            len+=snprintf(line+len,sizeof(line)-len,"\n");
            FIO_WriteFile(f,line,len);
        }
    }
    unsigned head=m50_photo_ring.head, i=m50_photo_ring.saved;
    if (head-i>M50_EVS) i=head-M50_EVS;
    for (;i<head;i++) {
        const struct m50_ev *e=&m50_photo_ring.ev[i%M50_EVS];
        int grid=e->c&0xFF, layout=(e->c>>8)&0xFF;
        char lut[48];
        if (grid==17) snprintf(lut,sizeof(lut),"17-point (layout %d), %d us",layout,(int)e->x);
        else if (grid==10) snprintf(lut,sizeof(lut),"10-point%s, %d us",layout ? " (17-point not used)":"",(int)e->x);
        else if (grid==1) snprintf(lut,sizeof(lut),"from the graded library (Live LUT)");
        else snprintf(lut,sizeof(lut),"Canon's");
        len=snprintf(line,sizeof(line),"%8d photo: XPan %s, 65:24 crop %s, LUT %s, grain %s\n",(int)e->ms,
            e->a ? "on":"off",e->b ? "on":"off",lut,e->d ? "on":"off");
        FIO_WriteFile(f,line,len);
    }
    m50_photo_ring.saved=head;
    head=m50_grain_ring.head; i=m50_grain_ring.saved;
    if (head-i>M50_EVS) i=head-M50_EVS;
    for (;i<head;i++) {
        const struct m50_ev *e=&m50_grain_ring.ev[i%M50_EVS];
        len=snprintf(line,sizeof(line),"%8d grain stage: %d ms, %d of %d rows grained%s\n",(int)e->ms,
            (int)e->a,(int)e->b,(int)e->c,e->d ? ", LATE" : "");
        FIO_WriteFile(f,line,len);
    }
    m50_grain_ring.saved=head;
    head=m50_ui_ring.head; i=m50_ui_ring.saved;
    if (head-i>M50_EVS) i=head-M50_EVS;
    for (;i<head;i++) {
        const struct m50_ev *e=&m50_ui_ring.ev[i%M50_EVS];
        len=snprintf(line,sizeof(line),"%8d UI: gui_state %d lv %d MLmenu %d frozen %d uifrozen %d layout %d view %d display %d | guimode %d play %d overlays %d front off %d\n",
            (int)e->ms,(int)(e->a&0xFF),(int)((e->a>>8)&1),(int)((e->a>>9)&1),(int)((e->a>>10)&1),(int)((e->a>>11)&1),
            (int)((e->a>>12)&3),(int)((e->a>>14)&1),(int)((e->a>>15)&1),(int)e->b,(int)e->c,(int)e->d,(int)e->x);
        FIO_WriteFile(f,line,len);
    }
    m50_ui_ring.saved=head;
    head=m50_mode_ring.head; i=m50_mode_ring.saved;
    if (head-i>M50_EVS) i=head-M50_EVS;
    for (;i<head;i++) {
        const struct m50_ev *e=&m50_mode_ring.ev[i%M50_EVS];
        len=snprintf(line,sizeof(line),"%8d MODE: shooting_mode %d (0x%x) custom %d (0x%x) ae_mode_movie %d shooting_type %d lv_movie_select %d\n",
            (int)e->ms,(int)e->a,(unsigned)e->a,(int)e->b,(unsigned)e->b,(int)e->c,(int)e->d,(int)e->x);
        FIO_WriteFile(f,line,len);
    }
    m50_mode_ring.saved=head;
    for (int k=0;k<2 && disp_new;k++) {
        struct disp_rec c;
        unsigned s1, tries=0;
        do { s1=disp_recs[k].seq; asm volatile("dmb" ::: "memory"); c=disp_recs[k]; asm volatile("dmb" ::: "memory"); }
        while (((s1&1) || s1!=disp_recs[k].seq) && ++tries<8);
        if (c.changes==disp_seen[k]) continue;
        disp_seen[k]=c.changes; disp_logs++;
        len=snprintf(line,sizeof(line),"%8d display %s: %d calls, change %d, args %x %x %x, stack %x %x\n",(int)now,k ? "EVF":"panel",
            (int)c.calls,(int)c.changes,(unsigned)c.a1,(unsigned)c.a2,(unsigned)c.a3,(unsigned)c.s0,(unsigned)c.s1);
        FIO_WriteFile(f,line,len);
        len=snprintf(line,sizeof(line),"         p:");
        for (int i=0;i<16;i++) len+=snprintf(line+len,sizeof(line)-len," %x",(unsigned)c.p[i]);
        len+=snprintf(line+len,sizeof(line)-len,"\n");
        FIO_WriteFile(f,line,len);
    }
    for (unsigned k=0;k<hil_ncallers && k<HIL_CALLERS && hil_new;k++) {
        static struct hil_rec c;                /* XPan's task only: off its 4 KB stack */
        unsigned s1, tries=0;
        do { s1=hil_recs[k].seq; asm volatile("dmb" ::: "memory"); c=hil_recs[k]; asm volatile("dmb" ::: "memory"); }
        while (((s1&1) || s1!=hil_recs[k].seq) && ++tries<8);
        if (c.calls==hil_seen[k] || hil_logs>=24) continue;
        hil_seen[k]=c.calls; hil_logs++;
        len=snprintf(line,sizeof(line),"%8d Hilton: caller %08x, %d calls (%d from other callers), obwb %08x corr %08x, %s\n",(int)c.ms,
            (unsigned)c.lr,(int)c.calls,(int)hil_other,(unsigned)c.obwb_addr,(unsigned)c.corr_addr,
            c.stops==1 ? "gains -1 EV (Cineon log +1 EV)" : "Canon's gains");
        FIO_WriteFile(f,line,len);
        for (int w=0;w<21;w+=7) {
            len=snprintf(line,sizeof(line),"         obwb +%02x:",w*4);
            for (int i=w;i<w+7;i++) len+=snprintf(line+len,sizeof(line)-len," %x",(unsigned)c.obwb[i]);
            len+=snprintf(line+len,sizeof(line)-len,"\n");
            FIO_WriteFile(f,line,len);
        }
        for (int w=0;w<13;w+=7) {
            len=snprintf(line,sizeof(line),"         corr +%02x:",w*4);
            for (int i=w;i<w+7 && i<13;i++) len+=snprintf(line+len,sizeof(line)-len," %x",(unsigned)c.corr[i]);
            len+=snprintf(line+len,sizeof(line)-len,"\n");
            FIO_WriteFile(f,line,len);
        }
    }
    if (tap_new) {
        taps_seen=af_taps;
        len=snprintf(line,sizeof(line),"%8d tap at %d,%d (%s), %d taps, converter calls %d, area shift %d\n",(int)af_tap_ms,af_tap_x,af_tap_y,
            af_tap_blocked ? "kept by XPan":"passed to Canon",(int)af_taps,(int)af_calls,(int)area_delta_s);
        FIO_WriteFile(f,line,len);
        len=snprintf(line,sizeof(line),"         tap correction %d (Bottom); box callers:",(int)touch_delta_s);
        for (unsigned i=0;i<box_ncallers && i<BOX_CALLERS;i++)
            len+=snprintf(line+len,sizeof(line)-len," %08x %d/%d moved",(unsigned)box_callers[i].lr,(int)box_callers[i].moved,(int)box_callers[i].n);
        len+=snprintf(line+len,sizeof(line)-len,"\n");
        FIO_WriteFile(f,line,len);
    }
    {
        unsigned head=af_head, i=af_saved;
        if (head-i>AF_RECS) i=head-AF_RECS;
        for (;i<head;i++) {
            const struct af_rec *r=&af_recs[i%AF_RECS];
            len=snprintf(line,sizeof(line),"%8d AF converter: tap %d,%d at %d ms; r1 %x r2 %x r3 %x; stack %d %d %d %d %d %d %d\n",(int)r->ms,
                r->tap_x,r->tap_y,(int)r->tap_ms,(unsigned)r->r1,(unsigned)r->r2,(unsigned)r->r3,
                (int)r->s[0],(int)r->s[1],(int)r->s[2],(int)r->s[3],(int)r->s[4],(int)r->s[5],(int)r->s[6]);
            FIO_WriteFile(f,line,len);
            len=snprintf(line,sizeof(line),"         frame before %d %d %d %d %d %d %d %d, after %d %d %d %d %d %d %d %d\n",
                r->before[0],r->before[1],r->before[2],r->before[3],r->before[4],r->before[5],r->before[6],r->before[7],
                r->after[0],r->after[1],r->after[2],r->after[3],r->after[4],r->after[5],r->after[6],r->after[7]);
            FIO_WriteFile(f,line,len);
        }
        af_saved=head;
    }
    if (aff_new) {
        memcpy(aff_last,aff,sizeof(aff));
        len=snprintf(line,sizeof(line),"%8d AF frame property: %d,%d size %dx%d of %dx%d\n",(int)now,aff[0],aff[1],aff[2],aff[3],aff[4],aff[5]);
        FIO_WriteFile(f,line,len);
    }
    if (lost_new) {
        lost_logged=live_lost;
        len=snprintf(line,sizeof(line),"%8d live LUT: %d library tables dropped (Canon reused the memory)\n",(int)now,(int)lost_logged);
        FIO_WriteFile(f,line,len);
    }
    FIO_CloseFile(f);
}

static MENU_UPDATE_FUNC(crop_display)
{
    MENU_SET_VALUE("%s",crop_on ? "6000x2216 (centre)" : crop_refused ? "refused" : "off (3:2 frame)");
    MENU_SET_RINFO("%d/%d",(int)crop_applies,(int)crop_removes);
    MENU_SET_HELP("Canon writes the 65:24 JPEG itself (Canon at 3:2, L). Last result %d.%s Save module's table (EXIF, AF, aspect): %s. Live View band: %s.",
        (int)crop_err,crop_refused ? " Canon's tables were not as expected: nothing changed." : "",
        crop_fcs>0 ? "2216" : crop_fcs<0 ? "not found" : "-",
        band_latch[0].mv ? "on" : "Canon's picture");
}

/* XPan > ... on the M50 (README "XPan menu"): the user's items fill the
 * placeholders of cropmarks.c's XPan menu; everything for development is in
 * one Diagnostics submenu at the end. Rows without a priv set their own
 * enabled state (MENU_SET_ENABLED), or ML draws them OFF. */
static struct menu_entry xpan_m50_menu[]={
    /* select_Q: Custom LUT is a submenu inside XPan's submenu, and ML's
     * default Q/PLAY action only toggles one level (from a submenu it goes
     * back), so it must open explicitly, as on the EOS M. */
    { .name="Custom LUT", .select=look_select, .update=look_display, .icon_type=IT_BOOL,
      .select_Q=menu_open_submenu,
      .help="LUT on the saved JPEG. LEFT/RIGHT: on/off. PLAY: LUT options.",
      .children=(struct menu_entry[]) {
        { .name="Cube file", .select=cube_file_select, .update=cube_file_display, .icon_type=IT_DICE,
          .help="LUT from ML/XPAN/LUTS, same as the HUD's LUT field." },
        { .name="Refresh Cube list", .select=cube_refresh_select, .update=cube_refresh_display,
          .help="Rescan ML/XPAN/LUTS and reload the selected file." },
        { .name="Cube strength", .priv=&m50_look_strength, .max=100, .unit=UNIT_PERCENT,
          .update=strength_display, .help="How much of the LUT, 0..100%. 0 keeps the tone base." },
        { .name="Tone base", .priv=&m50_tone_cineon, .max=2, .update=tone_display, .icon_type=IT_DICE,
          .choices=CHOICES("Canon Neutral","Cineon log","Cineon +1EV"),
          .help="What the LUT receives: Canon Neutral, Cineon log, or Cineon log with +1 EV highlight headroom." },
        { .name="Live LUT preview", .select=live_lut_select, .update=live_lut_display, .icon_type=IT_BOOL,
          .help="Experimental: the LUT also in Live View." },
#if M50_GRID17_TEST
        { .name="Colour grid", .select=grid17_select, .update=grid17_display, .icon_type=IT_DICE,
          .help="Table size for the photo's LUT: Canon's 10 points, or the M50's 17 (layout test)." },
#endif
        MENU_EOL
      } },
    { .name="Film grain", .select=grain_select, .update=grain_display, .icon_type=IT_BOOL,
      .select_Q=menu_open_submenu,
      .help="Film grain on the saved JPEG. LEFT/RIGHT: on/off. PLAY: options.",
      .children=(struct menu_entry[]) {
        { .name="Strength", .priv=&grain_strength, .min=1, .max=100, .unit=UNIT_PERCENT,
          .help="Grain amount: 25 subtle, 100 strong (strongest in mid-tones)." },
        { .name="Grain size", .priv=&grain_size, .max=2, .icon_type=IT_DICE,
          .choices=CHOICES("Fine","Medium","Coarse"),
          .help="Fine, Medium or Coarse grain." },
        MENU_EOL
      } },
    { .name="Diagnostics", .icon_type=IT_SUBMENU, .select=menu_open_submenu, .select_Q=menu_open_submenu,
      .shidden=1,                                 /* shown with Debug info ON (debug_info_display) */
      .help="For development: Canon's state, probes and logs. Shown with Debug info ON.",
      .children=(struct menu_entry[]) {
        { .name="65:24 JPEG crop", .update=crop_display, .icon_type=IT_ALWAYS_ON,
          .help="With 65:24 on, Canon writes a 6000x2216 JPEG (centre band). Needs Canon at 3:2, L." },
#if M50_LV_RESTART
        { .name="Live View band restart", .priv=&m50_lv_restart, .max=1, .update=lv_restart_display,
          .help="Opens and closes Canon's menu once when the 65:24 band must change (power-on, layout).",
          .help2="Canon moves the picture only when Live View restarts. Done/failed counts on the right." },
#endif
        { .name="Photo LUT", .update=look_diag_display, .icon_type=IT_ALWAYS_ON,
          .help="Canon's colour table for the last photos." },
        { .name="Grain stage", .update=grain_diag_display, .icon_type=IT_ALWAYS_ON,
          .help="Canon's filter stage for the film grain." },
        { .name="Canon settings", .update=settings_display, .icon_type=IT_ALWAYS_ON,
          .help="What Canon reports for the settings XPan relies on." },
        { .name="Learned settings", .select=opts_forget_select, .update=opts_display, .icon_type=IT_ALWAYS_ON,
          .help="EV, WB, drive, metering and format values recorded from Canon (XPan writes only these)." },
        { .name="Save Canon log", .select=canon_dump_select, .update=canon_dump_display, .icon_type=IT_ACTION,
          .help="Canon writes its own debug messages to the card (dumpf). Use after a good MENU cycle.",
          .help2="Saved by itself, once, when Live View does not come back after the menu." },
        { .name="Touch probe", .select=touch_probe_select, .update=touch_probe_display,
          .help="Records where taps land, for touch-to-select on the HUD." },
        { .name="Exposure probe", .select=expo_select, .update=expo_display,
          .help="Records Canon's raw shutter/aperture/ISO values. Read only." },
        MENU_EOL
      } },
    { .name="Debug info", .select=debug_info_select, .update=debug_info_display, .icon_type=IT_BOOL,
      .help="Development: logs, probes and the Diagnostics submenu. OFF: nothing runs for them." },
};

/* Called once from zebra.c after the Shoot > XPan menu is registered. */
void xpan_native_look_add_menu(void)
{
    menu_add("XPan",xpan_m50_menu,COUNT(xpan_m50_menu));
    for (unsigned i=0;i<COUNT(xpan_m50_menu);i++)
        if (streq(xpan_m50_menu[i].name,"Diagnostics")) m50_diag_entry=&xpan_m50_menu[i];
}
