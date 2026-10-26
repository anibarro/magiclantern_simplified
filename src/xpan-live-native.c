/* Native EOS M LCD Cube preview. Prepare1000 nodes once per selection;
 * Canon uploads/evaluates them. No image pixels are processed here. */
#include "dryos.h"
#include "lens.h"
#include "patch.h"
#include "propvalues.h"
#include "xpan-mode.h"
#include "xpan-native-table.h"
#include "xpan-live-native.h"
#include "xpan-live-color-probe.h"
#if !defined(CONFIG_EOSM) || defined(CONFIG_QEMU) || !defined(CONFIG_XPAN)
#error Native LCD preview requires the physical EOS M hardware candidate.
#endif

/* Exact Neutral base captured twice in Test34; coefficients differ from the
 * JPEG fallback. This is a known-domain guard, not a color-space calibration. */
#include "xpan-live-neutral.inc"
#include "xpan-live-neutral-enabled.inc"
static uint32_t output[2][1024] __attribute__((aligned(64)));
static volatile int active_slot,inflight=-1,ready;
static unsigned prepared_revision,prepared_strength;
static uint32_t prepared_hash,prepare_us,format_flags;
static volatile unsigned redirected,completed,blocked,guard_mask,verify_us;
static int installed,install_attempted,install_error;
static const char *prepare_status="Waiting for Cube";
static volatile unsigned lcd_guard,original_enable,last_variant;
static volatile uint32_t observed_hash;
static unsigned variant_calls[2],enable_calls[2],unknown_calls;
static uint32_t queued_unknown_hash;
/* Record exposure/path transitions, not every frame or alternating buffer
 * address. This preserves the useful case when a later upload looks normal. */
struct lcd_case {
    uint32_t ms,mask,variant,enable,hash,source,iso,av,tv;
};
static struct lcd_case cases[8];
static unsigned case_count;

static void record_lcd_case(unsigned mask,unsigned variant,unsigned enable,
    uint32_t source,uint32_t hash)
{
    struct lcd_case current={get_ms_clock(),mask,variant,enable,hash,source,
        lens_info.iso,lens_info.raw_aperture,lens_info.raw_shutter};
    uint32_t old=cli(); lcd_guard=mask;
    last_variant=variant; original_enable=enable; observed_hash=hash;
    if (case_count) {
        const struct lcd_case *last=&cases[(case_count-1)%COUNT(cases)];
        if (last->mask==mask && last->variant==variant && last->enable==enable &&
            last->hash==hash && last->iso==current.iso && last->av==current.av && last->tv==current.tv)
        { sei(old); return; }
    }
    cases[case_count%COUNT(cases)]=current; case_count++; sei(old);
}

static uint32_t native_hash(const uint32_t *table)
{
    uint32_t hash=2166136261u;
    for (unsigned i=0;i<1024;i++) hash=(hash^table[i])*16777619u;
    return hash;
}
void xpan_live_native_pause(void)
{
    /* Keep both allocations alive while an earlier DMA may still read them. */
    uint32_t old=cli(); ready=0; sei(old);
}
const char *xpan_live_native_status(void)
{
    if (!installed) return "Hardware completion hook unavailable";
    if (!ready) return prepare_status;
    /* Only the scoped LCD path changes lcd_guard. Unrelated Canon transfers
     * must not hide an actual preview rejection or invent a false warning. */
    if (lcd_guard==64) return "Hardware waiting for known Neutral LCD table";
    if (lcd_guard && lcd_guard!=2) return "Hardware LCD path waiting";
    if (redirected) return "Hardware LCD preview active";
    if (guard_mask) return "Hardware waiting for matching Neutral LV";
    return "Hardware ready; waiting for upload";
}
unsigned xpan_live_native_log_revision(void)
{
    /* Exposure/path transitions request one rate-limited idle log update. */
    return case_count;
}
static void native_complete(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)regs; (void)stack; (void)pc;
    /* Canon's copy-complete callback precedes its semaphore release. The
     * lookup source is no longer read by DMA; the LCD reads separate YUV. */
    if (inflight>=0) { completed++; inflight=-1; }
}
void xpan_live_native_init(unsigned observer_hooks)
{
    if (install_attempted || !(observer_hooks&1)) return;
    install_attempted=1;
    /* Observer validates source selection. Check copy completion before
     * trusting the lifetime of our retained double-buffered table sources. */
    if (*(volatile uint32_t *)0x29140!=0xe92d4010 ||
        *(volatile uint32_t *)0x29164!=0xeaff768e ||
        *(volatile uint32_t *)0x287a4!=0x29140 ||
        *(volatile uint32_t *)0x28514!=0xebff7967 ||
        *(volatile uint32_t *)0xff50c058!=0xeb2c72d1 ||
        native_hash(xpan_live_neutral)!=0xd5c31d39u ||
        native_hash(xpan_live_neutral_enabled)!=0xcccf5d4du)
    { install_error=-1; return; }
    install_error=patch_hook_function(0x29140,0xe92d4010,native_complete,"XPan LCD table copy complete");
    installed=!install_error;
}
int xpan_live_native_prepare(const struct xpan_lut *cube,unsigned revision,
    unsigned strength,int identity,const struct xpan_lut *base,volatile int *cancel)
{
    xpan_live_native_pause();
    if (!cube || !revision) return 0;
    if ((identity || !strength) && !base) { prepare_status="Identity / zero strength; Canon bypass"; return 1; }
    uint32_t started=(uint32_t)get_us_clock();
    int slot=active_slot^1;
    /* Wait only in the idle worker if this old generation is still a DMA
     * source. Never allocate, wait or prepare a Cube inside Canon's hook. */
    uint32_t waiting=get_ms_clock();
    while (inflight==slot) {
        if (ml_shutdown_requested || (cancel && *cancel) || get_ms_clock()-waiting>1000)
        { prepare_status="Hardware copy busy; retry look selection"; return 0; }
        msleep(1);
    }
    struct xpan_native_cube_stats stats;
    if (!xpan_native_table_prepare_cube(output[slot],xpan_live_neutral,cube,
        strength,identity,base,&stats,&format_flags))
    { prepare_status="Hardware table validation failed"; return 0; }
    /* DMA sees the uncached alias only after all cached writes are cleaned.
     * Keep the patch-aware wrapper so existing ML ROM hooks survive. */
    sync_caches();
    if (ml_shutdown_requested || (cancel && *cancel)) return 0;
    prepared_hash=native_hash(output[slot]); prepared_revision=revision; prepared_strength=strength;
    prepare_us=(uint32_t)get_us_clock()-started;
    uint32_t old=cli(); active_slot=slot; guard_mask=lcd_guard=0; ready=1; sei(old);
    prepare_status="Hardware table prepared";
    return 1;
}
/* Movie LUT diagnostics: [site common/Diana][bank0/1/other]. */
static volatile unsigned movie_seen[2][3],movie_redirected,movie_last_caller,movie_last_hash,movie_last_enable;
void xpan_live_native_movie_diag(char *text,unsigned size)
{
    snprintf(text,size,"seen C %d/%d/%d D %d/%d/%d, applied %d, caller %x, en %d, hash %x",
        movie_seen[0][0],movie_seen[0][1],movie_seen[0][2],movie_seen[1][0],movie_seen[1][1],movie_seen[1][2],
        movie_redirected,movie_last_caller,movie_last_enable,movie_last_hash);
}
void xpan_live_native_transfer(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    if (is_movie_mode()) {
        /* Movie LUT: record every Live View table upload, then replace the
         * bank-1 table with the prepared one. Canon's movie table is not yet
         * known, so the photo source-hash check is not applied here; only
         * Neutral and an enable value of 0/1 are required. */
        unsigned site=pc==0x285bc ? 0:1,bank=regs[8]<2 ? regs[8]:2;
        movie_seen[site][bank]++;movie_last_caller=stack[13];movie_last_enable=regs[6];
        uint32_t mp=regs[9]&~0x40000000u;
        if (!(regs[9]&3) && mp>=0x10000 && mp<=0x10000000-4096) movie_last_hash=native_hash((const uint32_t *)regs[9]);
        if (regs[8]!=1 || regs[6]>1 || !installed || !xpan_native_look_movie_requested()) return;
        uint32_t old=cli();
        /* Every upload reads the same, already prepared active slot, so a copy
         * still in flight is no reason to let Canon's table through (v11 let
         * ~9 % of uploads pass). prepare() writes only the other slot. */
        if (!ready) { blocked++; sei(old); return; }
        int slot=active_slot; inflight=slot;
        regs[9]=(uint32_t)UNCACHEABLE(output[slot]); regs[6]=1;
        movie_redirected++; redirected++; sei(old);
        return;
    }
    /* The same bank1 LCD uploader receives bypassed AND enabled Neutral
     * sources. Test56 captures an enabled Neutral case excluded by the old
     * gate; the screenshots show loss of the look after exposure changes. */
    if (pc!=0x285bc || regs[8]!=1) { guard_mask=4; return; }
    if (stack[13]!=0xff50c05c) { guard_mask=16; return; }
    unsigned enable=regs[6]; original_enable=enable;
    uint32_t mask=(!installed || !ready ? 1:0) |
        (!xpan_native_look_live_requested() ? 2:0) |
        (lens_info.raw_picstyle!=0x84 || enable>1 ? 8:0);
    if (mask) { guard_mask=mask; record_lcd_case(mask,0,enable,regs[9],0); return; }
    uint32_t physical=regs[9]&~0x40000000u;
    if ((regs[9]&3) || physical<0x10000 || physical>0x10000000-4096)
    { guard_mask=32; record_lcd_case(32,0,enable,regs[9],0); return; }
    uint32_t started=(uint32_t)get_us_clock();
    const volatile uint32_t *source=(const volatile uint32_t *)regs[9];
    /* Recognize exact captured sources without broadening the JPEG validator:
     * the enabled LCD reference has Cb excursions to263. The OUTPUT remains
     * the same fixed, validated Test34 base+Cube in both cases, rather than
     * stacking Canon's enabled table with our look. No photo pixels are read. */
    unsigned variants=3;
    uint32_t hash=2166136261u;
    for (unsigned i=0;i<1024;i++) {
        uint32_t word=source[i]; hash=(hash^word)*16777619u;
        if (word!=xpan_live_neutral[i]) variants&=~1u;
        if (word!=xpan_live_neutral_enabled[i]) variants&=~2u;
    }
    observed_hash=hash; last_variant=variants;
    if (!variants) {
        unknown_calls++; guard_mask=64;
        record_lcd_case(64,0,enable,regs[9],hash);
        if (hash!=queued_unknown_hash) {
            queued_unknown_hash=hash; xpan_live_color_probe_queue_unknown();
        }
        verify_us=(uint32_t)get_us_clock()-started; return;
    }
    variant_calls[variants==1 ? 0:1]++; enable_calls[enable]++;
    uint32_t old=cli();
    if (!ready || inflight>=0) {
        blocked++; guard_mask=128; sei(old);
        record_lcd_case(128,variants,enable,regs[9],hash); return;
    }
    int slot=active_slot; inflight=slot;
    /* R9 feeds the original MOV R2,R9. R6 feeds Canon's enable function.
     * Canon's prologue/epilogue restore both caller registers. Canon owns
     * the copy, enable write, callback and semaphore sequencing. */
    regs[9]=(uint32_t)UNCACHEABLE(output[slot]); regs[6]=1;
    redirected++; guard_mask=0; sei(old);
    record_lcd_case(0,variants,enable,(uint32_t)source,hash);
    verify_us=(uint32_t)get_us_clock()-started;
}
