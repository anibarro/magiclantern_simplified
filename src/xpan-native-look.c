/* Automatic native color-table composition at Canon's LUT-enable decision.
 * Canon owns RAW development, engine locking, transfer, encoding and file saving.
 * The hook prepares only 1000 lookup entries, never image pixels, and does no I/O. */
#include "dryos.h"
#include "lens.h"
#include "menu.h"
#include "fileprefix.h"
#include "notify_box.h"
#include "patch.h"
#include "cropmarks.h"
#include "xpan-native-table.h"
#include "xpan-native-cube.h"
#include "picstyle.h"
#include "propvalues.h"
#include "config.h"
#include "xpan-mode.h"
#include "xpan-live-native.h"
#include "xpan-live-color-probe.h"
#ifdef CONFIG_XPAN
#include "xpan-film-grain.h"
#endif
#include "xpan-native-jpeg.h"

#if !defined(CONFIG_EOSM) || defined(CONFIG_QEMU) || !defined(CONFIG_XPAN)
#error Native look proof requires the physical EOS M 2.0.2 build.
#endif

/* Test26 proves enable0 skips the old FF42D1AC transfer entirely. Intercept
 * the decision after LDRH, before Canon tests for disabled/invalid enables. */
#define LOOK_HOOK 0xff42d178
#define LOOK_INSTRUCTION 0xe2430cff
#define TABLE_WORDS 1024
#define TABLE_BYTES 4096
#define FRONT_PREFIX_BYTES 0x38

/* Session settings are saved by the normal ML configuration mechanism. */
static CONFIG_INT("xpan.look",selected_look,0);
static CONFIG_INT("xpan.cube.strength",cube_strength,100);
static CONFIG_INT("xpan.lut.lcd",live_preview_enabled,1);
/* Custom .cube only: lower Canon's JPEG_OBWB gains by 0/1/2 stops so RAW
 * highlights are not clipped inside Canon's render (Test68), and compose the
 * matching Neutral re-expansion base under the Cube in the same shot. */
/* 0 Canon Neutral (no headroom), 1 Cineon log base (gains -1 EV + CINEON7.CUBE),
 * 2 Calibration (gains -2 EV, Canon render untouched, identity Cube). */
static CONFIG_INT("xpan.base",headroom_stops,1);
/* JPEG_OBWB gain reduction: Cineon log uses -1 EV (v8: halves the shadow
 * expansion that posterized LUT output; HTP still keeps RAW white below the
 * table's top, J~249). Calibration keeps the -2 EV used to fit model v7.
 * CINEON7.CUBE must be generated for the same value (xpan_cineon_base_v7.py). */
/* XPan > Highlight headroom (Cineon log only): +1 EV (smoother shadows) or
 * +2 EV (one more stop of RAW highlight colour, coarser deep shadows). */
static CONFIG_INT("xpan.headroom.ev",headroom_ev,1);
static unsigned armed_headroom_ev=1;
#define XPAN_HEADROOM_STOPS(base) ((base)==2 ? 2u:(armed_headroom_ev==2 ? 2u:1u))
/* Movie LUT (experiment): same Cube/strength/Tone base applied to the Live
 * View colour table in movie mode, to check whether Canon's H.264 recording
 * carries it. Independent of the photo XPan switch. */
static CONFIG_INT("xpan.movie_lut",movie_lut_enabled,0);
static volatile int movie_lut_ready;
/* Movie mode: the LUT follows Custom LUT ON. */
int xpan_movie_enabled(void);   /* cropmarks.c: XPan ON/OFF in movie mode */
int xpan_movie_lut_wanted(void) { return selected_look==3 && is_movie_mode() && xpan_movie_enabled(); }
static unsigned armed_headroom;
static volatile unsigned headroom_shots,headroom_last;
/* Last photo's headroom decision, shown in the menu (no I/O in the hook):
 * 0 applied, 1 off, 2 HTP/ALO, 3 OBWB check (headroom_obwb_code), 4 table
 * prepare failed (format_flags), 5 not reached (look not armed / guard),
 * 6 Cineon base file missing, 7 Angel (tone row) check failed. */
static volatile int headroom_angel_code;
static volatile int headroom_code=5,headroom_obwb_code;
static unsigned armed_cube_strength;
static struct xpan_native_cube_stats cube_stats,preview_cube_stats;
static volatile int look_busy,look_cancel,look_state;
static const char *look_status="Starting XPan";
static void *look_allocation,*preview_allocation;
static uint32_t *look_output,*look_actual,*preview_output,*preview_actual;
static uint32_t *look_front,*preview_front,*look_obwb,*preview_obwb,*look_angel,*preview_angel;
static int hook_installed,patch_result;
static uint32_t original_address,hook_us,matched,actual_hash,output_hash;
static uint32_t source_stable,look_reason,format_flags;
static int armed_look;
static struct card_info *armed_shooting_card;
static unsigned first_transfer_file,preview_attempted,preview_matched;
static uint32_t preview_source_hash,preview_output_hash,preview_us,preview_flags;
static unsigned enable_overrides,last_original_enable,last_effective_enable;
/* Hooks stay installed; only a fully prepared idle configuration is published. */
static volatile int automatic_capture_ready;
static unsigned automatic_look_file=0xffffffffu,automatic_look_folder=0xffffffffu;
/* Bounded capture diagnostics distinguish a missing native transfer from a
 * rejected guard or a reset counter. Retain four JPEG-stage records across
 * JPEG-only and RAW+JPEG shots; all report formatting stays in the worker. */
static volatile unsigned transfer_calls,ready_transfer_calls,accepted_transfer_calls;
static volatile unsigned last_guard_mask,last_transfer_r0,last_transfer_r1,last_transfer_r3,last_transfer_r4;
static volatile unsigned capture_trace_count;
static volatile struct {
    unsigned file,quality,ready,look,style,kind,enable,source,profile,config;
    unsigned calls,ready_calls,accepted,matched,preview,reason,flags;
} capture_trace[4];

void xpan_native_look_note_jpeg(void)
{
    struct card_info *card=get_shooting_card();
    unsigned slot=capture_trace_count%COUNT(capture_trace);
    capture_trace[slot].file=card ? card->file_number:0;
    capture_trace[slot].quality=pic_quality;
    capture_trace[slot].ready=automatic_capture_ready;
    capture_trace[slot].look=armed_look;
    capture_trace[slot].style=lens_info.raw_picstyle;
    /* This is Canon's fixed FOD RAM block already used by the exact guard.
     * Observe it at JPEG setup even when the conditional LUT call never ran. */
    capture_trace[slot].kind=*(volatile uint32_t *)0x701bc;
    capture_trace[slot].enable=*(volatile uint16_t *)0x701ec;
    capture_trace[slot].source=*(volatile uint32_t *)0x701e8;
    capture_trace[slot].profile=*(volatile uint32_t *)0x701b8;
    capture_trace[slot].config=*(volatile uint32_t *)0x701c0;
    capture_trace[slot].calls=transfer_calls;
    capture_trace[slot].ready_calls=ready_transfer_calls;
    capture_trace[slot].accepted=accepted_transfer_calls;
    capture_trace[slot].matched=matched;
    capture_trace[slot].preview=preview_matched;
    capture_trace[slot].reason=look_reason;
    capture_trace[slot].flags=format_flags;
    __asm__ __volatile__("" ::: "memory");
    capture_trace_count++;
}

/* Photo base for the selected Tone base. Headroom render -> CINEON7; if a
 * guard rejects the headroom render for this shot (e.g. unverified ISO), the
 * Neutral -> Cineon base (same as the LCD) keeps the LUT on Cineon input, so
 * the JPEG still matches the preview, only without the extra headroom. */
static const struct xpan_lut *photo_base(unsigned headroom)
{
    if (headroom==1) return xpan_native_base_table();
    if (headroom==0 && armed_headroom==1) return xpan_native_base_lv_table();
    return NULL;
}
/* LCD base matching the photo's Cineon base (NULL: preview over Neutral). */
static const struct xpan_lut *live_base(void)
{
    return headroom_stops==1 ? xpan_native_base_lv_table():NULL;
}
int xpan_native_look_live_requested(void)
{
    unsigned strength=(unsigned)COERCE(cube_strength,0,100)*65535u/100;
    return automatic_capture_ready && live_preview_enabled && selected_look==3 && xpan_photo_ui_active() &&
        !lens_info.job_state && !ml_shutdown_requested && lv_dispsize<=5 &&
        (!gui_menu_shown() || menu_active_but_hidden()) &&
        ((strength && !xpan_native_cube_identity()) || live_base());
}

/* Movie LUT: the prepared table replaces Canon's Live View table whenever
 * movie mode runs with Neutral; no photo-mode or menu conditions. */
int xpan_native_look_movie_requested(void)
{
    return selected_look==3 && xpan_movie_enabled() && movie_lut_ready && is_movie_mode() && lv &&
        !ml_shutdown_requested && lens_info.raw_picstyle==0x84;
}

static void prepare_live_preview(struct card_info *card)
{
    if (!live_preview_enabled || selected_look!=3 || !card) return;
    if (!xpan_native_cube_load(card,&look_cancel) || look_cancel ||
        ml_shutdown_requested || get_ml_card()!=card || lens_info.job_state ||
        !xpan_photo_ui_active()) return;
    /* Prepare only the small hardware table; no unused 256KiB CPU map. */
    xpan_live_native_prepare(xpan_native_cube_table(),xpan_native_cube_revision(),
        (unsigned)COERCE(cube_strength,0,100)*65535u/100,
        xpan_native_cube_identity(),live_base(),&look_cancel);
}

static uint32_t table_hash(const uint32_t *table)
{
    uint32_t hash=2166136261u;
    for (unsigned i=0; i<TABLE_WORDS; i++) hash=(hash^table[i])*16777619u;
    return hash;
}

static int valid_source(uint32_t address)
{
    /* Inspect only the small native table, never sensor/YUV memory or MMIO. */
    uint32_t base=address&~0x40000000u;
    return !(address&3) && ((base>=0x10000 && base<=0x08000000-TABLE_BYTES) ||
                            (address>=0xff000000u && address<=0xfffff000u));
}

static unsigned preview_headroom;
static int native_preview_table(uint32_t source)
{
    if (!matched || preview_attempted || !preview_allocation ||
        !armed_shooting_card || (unsigned)armed_shooting_card->file_number!=first_transfer_file) return 0;
    preview_attempted=1; look_state=2;
    uint32_t entered=(uint32_t)get_us_clock();
    /* Our redirected pointer must never become the source of another grade.
     * Canon's original source pointer is unchanged by the first transfer. */
    uint32_t physical=source&~0x40000000u;
    if (valid_source(source) && physical!=((uint32_t)look_output&~0x40000000u) &&
        physical!=((uint32_t)preview_output&~0x40000000u)) {
        const volatile uint32_t *table=(const volatile uint32_t *)source;
        for (unsigned i=0; i<TABLE_WORDS; i++) preview_actual[i]=table[i];
        int stable=1;
        for (unsigned i=0; i<TABLE_WORDS; i++) if (preview_actual[i]!=table[i]) stable=0;
        preview_source_hash=table_hash(preview_actual);
        int prepared=0;
        preview_headroom=0;
        if (stable) {
            if (armed_look==3 && armed_headroom && !get_htp() && get_alo()==ALO_OFF &&
                (armed_headroom==2 || xpan_native_base_table()) &&
                xpan_native_obwb_headroom(preview_obwb,
                    *(volatile uint32_t *)(0x701b8+0x28),XPAN_HEADROOM_STOPS(armed_headroom))==1 &&
                xpan_native_angel_shadow(preview_angel,*(volatile uint32_t *)(0x701b8+0x2c))==1)
                preview_headroom=armed_headroom;
            if (armed_look==3)
                prepared=xpan_native_table_prepare_cube(preview_output,preview_actual,
                    xpan_native_cube_table(),armed_cube_strength,
                    xpan_native_cube_identity() || preview_headroom==2,
                    photo_base(preview_headroom),
                    &preview_cube_stats,&preview_flags);
            else prepared=xpan_native_table_prepare(preview_output,preview_actual,armed_look,&preview_flags);
        }
        if (prepared) {
            preview_output_hash=table_hash(preview_output);
            preview_matched=1;
        }
    }
    preview_us=(uint32_t)get_us_clock()-entered;
    __asm__ __volatile__("" ::: "memory");
    look_state=3;
    return preview_matched;
}

static void native_redirect_front(uint32_t *regs,uint32_t *prefix,uint32_t *output,uint32_t *obwb,uint32_t *angel)
{
    /* From FF42D178 until the POP at FF42D204, Canon reads only offsets30,
     * 2C and28 of R4. Supply a private complete prefix through offset34.
     * The outer function restores the caller's original R4 from its stack;
     * the Canon-owned FOD block and table stay unchanged. */
    const volatile uint32_t *original=(const volatile uint32_t *)regs[4];
    for (unsigned i=0; i<FRONT_PREFIX_BYTES/4; i++) prefix[i]=original[i];
    prefix[0x30/4]=(uint32_t)output;
    prefix[0x34/4]=(prefix[0x34/4]&0xffff0000u)|1;
    /* The headroom base in 'output' assumes this lowered-gain OBWB copy;
     * both are published together or not at all (Test68 front +0x28). */
    if (obwb) prefix[0x28/4]=(uint32_t)obwb;
    if (angel) prefix[0x2c/4]=(uint32_t)angel; /* HTP luma row (shadow room) */
    __asm__ __volatile__("" ::: "memory");
    last_original_enable=regs[3]; last_effective_enable=1;
    if (!regs[3]) enable_overrides++;
    /* R3 is already loaded by FF42D174. Setting it as well as the private
     * prefix makes Canon enable and load the validated table itself. */
    regs[3]=1; regs[4]=(uint32_t)prefix;
}

static void native_table_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    transfer_calls++;
    /* The shared Canon path also runs outside photo LV. Gate by the mode and
     * the prepared configuration, not by menu visibility or a30s arm window. */
    if (!automatic_capture_ready || !xpan_photo_enabled() || is_movie_mode()) return;
    ready_transfer_calls++;
    last_transfer_r0=regs[0]; last_transfer_r1=regs[1]; last_transfer_r4=regs[4];
    last_transfer_r3=regs[3];
    struct card_info *card=get_shooting_card();
    if (!card) return;
    if ((unsigned)card->file_number!=automatic_look_file ||
        (unsigned)card->folder_number!=automatic_look_folder || card!=armed_shooting_card)
    {
        automatic_look_file=card->file_number; automatic_look_folder=card->folder_number;
        armed_shooting_card=card; first_transfer_file=card->file_number;
        /* Each photo gets independent first/embedded-preview outputs. Unknown
         * formats still pass through unchanged under the original guards. */
        matched=preview_attempted=preview_matched=0;
        actual_hash=output_hash=hook_us=preview_source_hash=preview_output_hash=preview_us=0;
        format_flags=preview_flags=XPAN_NATIVE_TABLE_NOT_CHECKED;
        headroom_code=5; headroom_obwb_code=0;
        look_state=1;
    }
    if (!armed_look) {
        return; /* Canon passthrough still clears per-photo counters. */
    }
    if (look_state!=1 && look_state!=3) return;
    /* The new site precedes Canon's optional transfer. Accept the same fixed
     * full-photo kind17 block, adding its ROM profile/header identities.
     * Only known enable0/1 values are eligible; FFFF and other modes pass on.
     * No bypass is changed until the source table is stable and validated. */
    uint32_t source=*(volatile uint32_t *)0x701e8;
    unsigned enable=*(volatile uint16_t *)0x701ec;
    last_guard_mask=(pc!=LOOK_HOOK ? 1:0) | (regs[3]!=enable ? 2:0) |
        (regs[4]!=0x701b8 ? 4:0) |
        (!source ? 8:0) |
        (*(volatile uint32_t *)0x701bc!=0x17 ? 16:0) |
        (enable>1 ? 32:0) | (lens_info.raw_picstyle!=0x84 ? 64:0) |
        (*(volatile uint32_t *)0x701b8!=0xff92da38 ||
         *(volatile uint32_t *)0x701c0!=0xff92da94 ? 128:0);
    if (last_guard_mask) return;
    accepted_transfer_calls++;
    /* An identity Cube/zero strength should preserve the original bypass,
     * including the enable0 case. It is not a request to enable hardware. */
    if (armed_look==3 && (!armed_cube_strength || xpan_native_cube_identity()) && !armed_headroom)
    { look_reason=6; return; }
    if (look_state==3) {
        if (native_preview_table(source))
            native_redirect_front(regs,preview_front,preview_output,
                preview_headroom ? preview_obwb:NULL,preview_headroom ? preview_angel:NULL);
        return;
    }
    first_transfer_file=armed_shooting_card ? armed_shooting_card->file_number:0;
    look_state=2;
    uint32_t started=(uint32_t)get_us_clock();
    original_address=source; look_reason=1;
    if (valid_source(original_address)) {
        const volatile uint32_t *source=(const volatile uint32_t *)original_address;
        /* Capture only Canon's actual table. The old card-fixture comparison
         * and its per-shot observation files are no longer part of capture. */
        for (unsigned i=0; i<TABLE_WORDS; i++) look_actual[i]=source[i];
        actual_hash=table_hash(look_actual);
        source_stable=1;
        for (unsigned i=0; i<TABLE_WORDS; i++) {
            if (source[i]!=look_actual[i]) source_stable=0;
        }
        look_reason=source_stable ? 3:2;
        /* Test13 proves the native Neutral tone/color entries vary per shot.
         * Validate the captured domain and grade that small table directly;
         * reference differences remain diagnostics, not a frozen calibration. */
        int prepared=0;
        unsigned headroom=0;
        if (source_stable) {
            /* A rejected OBWB layout falls back to the normal base; the reason
             * is kept for the menu. Headroom assumes Neutral without HTP/ALO. */
            if (armed_look==3) {
                if (!armed_headroom) headroom_code=1;
                else if (get_htp() || get_alo()!=ALO_OFF) headroom_code=2;
                else if (armed_headroom==1 && !xpan_native_base_table()) headroom_code=6;
                else {
                    headroom_obwb_code=xpan_native_obwb_headroom(look_obwb,
                        *(volatile uint32_t *)(0x701b8+0x28),XPAN_HEADROOM_STOPS(armed_headroom));
                    headroom_angel_code=headroom_obwb_code==1 ?
                        xpan_native_angel_shadow(look_angel,*(volatile uint32_t *)(0x701b8+0x2c)):0;
                    if (headroom_obwb_code!=1) headroom_code=3;
                    else if (headroom_angel_code!=1) headroom_code=7;
                    else { headroom=armed_headroom; headroom_code=0; }
                }
            }
            if (armed_look==3)
                prepared=xpan_native_table_prepare_cube(look_output,look_actual,
                    xpan_native_cube_table(),armed_cube_strength,
                    xpan_native_cube_identity() || headroom==2,
                    photo_base(headroom),&cube_stats,&format_flags);
            else prepared=xpan_native_table_prepare(look_output,look_actual,armed_look,&format_flags);
        }
        if (!prepared && armed_look==3) headroom_code=4;
        if (prepared) {
            output_hash=table_hash(look_output);
            /* Publish only a fully validated output. Canon's original LUT
             * enable, transfer, locking and development code performs the job. */
            native_redirect_front(regs,look_front,look_output,headroom ? look_obwb:NULL,headroom ? look_angel:NULL);
            headroom_last=headroom; if (headroom) headroom_shots++;
            matched=1; look_reason=armed_look ? 5:4;
        }
    }
    hook_us=(uint32_t)get_us_clock()-started;
    __asm__ __volatile__("" ::: "memory");
    look_state=3;
}

/* Menu and idle workflow share these private capture helpers. */
#include "xpan-workflow.inc"
