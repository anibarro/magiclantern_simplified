/* XPan on the Canon 6D (firmware 1.1.6): stand-ins for the EOS M-only stages
 * and the capture probe used to port them.
 *
 * Shared with the EOS M (other files): framing, HUD, controls, menus, Cube
 * parsing and the native colour-table look (xpan-native-look.c, hooked in the
 * 6D's TTL develop routine). Not ported yet, because the 6D builds its stills
 * differently: the EOS M develops RAW to YUV in one Canon job (FOD) and
 * encodes the JPEG in a second one (TTJ). XPan crops the JPEG in the TTJ job
 * and adds grain and the watermark to the YUV image in between. The 6D's TTL
 * routine instead carries one register profile with both the develop and the
 * JPEG engine settings (ROM FFD4EE68: C0F37xxx/C0F08xxx/C0F1Axxx develop
 * registers, then C0F11xxx/C0F13xxx JPEG registers, full height 3648).
 * Where a crop and a YUV stage can be inserted has to be seen on the camera.
 *
 * The probe (XPan > 6D capture probe) records Canon's job blocks at the TTL
 * entry and at the TTJ entry and exit for every photo, plus the colour-table
 * job identity seen by the look hook, and writes them to
 * ML/LOGS/X6DPROBE.LOG. It only reads: no Canon value is changed.
 *
 * First log (2026-11-07): an L JPEG shot never reached TTL or TTJ, and the
 * one TTL call (RAW+JPEG) was a 5568x3708 job with LuckyEnable 0xFFFF (table
 * off). So the probe also records Canon's own debug messages (DryosDebugMsg):
 * Canon's develop routines announce themselves there ("[TTL] START ..."). */
#include "dryos.h"
#include "menu.h"
#include "patch.h"
#include "lens.h"
#include "propvalues.h"
#include "fileprefix.h"
#include "xpan-mode.h"
#include "xpan-lut.h"
#include "xpan-native-jpeg.h"
#include "xpan-live-native.h"
#include "xpan-live-color-probe.h"
#include "xpan-film-grain.h"
#include "timer.h"
#include "tasks.h"

#if !defined(CONFIG_6D) || !defined(CONFIG_XPAN) || defined(CONFIG_QEMU)
#error xpan-6d.c is the physical 6D 1.1.6 XPan build only.
#endif

/* ------------------------------------------------------------------ */
/* Stages that are not ported yet: report it, never hook anything.     */

static const char not_ported[]="Not on the 6D yet";

/* JPEG 65:24 crop (EOS M: TTJ job descriptor). The workflow still installs
 * the look hook, so start/playback report success without patching. */
void xpan_native_jpeg_set_automatic(void) {}
int xpan_native_jpeg_start(int crop_mode) { (void)crop_mode; return 1; }
int xpan_native_jpeg_stop(int idle) { (void)idle; return 1; }
int xpan_native_jpeg_save(struct card_info *card, unsigned number) { (void)card; (void)number; return 0; }
int xpan_native_jpeg_hooks_remain(void) { return 0; }
const char *xpan_native_jpeg_status(void) { return "6D: JPEG crop not ported yet"; }
unsigned xpan_native_jpeg_crop_count(void) { return 0; }
void xpan_native_jpeg_diag(unsigned d[11]) { for (int i=0;i<11;i++) d[i]=0; }
int xpan_native_jpeg_is_job(uint32_t address) { (void)address; return 0; }
int xpan_native_jpeg_playback_enable(void) { return 1; }
const char *xpan_native_jpeg_playback_status(void) { return "6D: not needed without the crop"; }
int xpan_native_jpeg_playback_save(struct card_info *card) { (void)card; return 0; }
int xpan_native_jpeg_log_number_available(struct card_info *card, unsigned number) { (void)card; (void)number; return 0; }

/* Live View / movie LUT (EOS M: LCD colour-transfer hooks in RAM code). */
void xpan_live_native_init(unsigned observer_hooks) { (void)observer_hooks; }
void xpan_live_native_pause(void) {}
const char *xpan_live_native_status(void) { return "6D: Live View LUT not ported yet"; }
int xpan_live_native_prepare(const struct xpan_lut *cube,unsigned revision,
    unsigned strength,int identity,const struct xpan_lut *base,volatile int *cancel)
{ (void)cube; (void)revision; (void)strength; (void)identity; (void)base; (void)cancel; return 1; }
void xpan_live_native_transfer(uint32_t *regs,uint32_t *stack,uint32_t pc) { (void)regs; (void)stack; (void)pc; }
int xpan_live_native_log(char *text,unsigned size) { if (size) text[0]=0; return 0; }
unsigned xpan_live_native_log_revision(void) { return 0; }
void xpan_live_native_movie_diag(char *text,unsigned size) { snprintf(text,size,"%s",not_ported); }
void xpan_live_color_probe_poll(struct card_info *card) { (void)card; }
void xpan_live_color_probe_queue_unknown(void) {}

/* Saved-image grain and watermark (EOS M: YUV stage between FOD and TTJ). */
int xpan_film_grain_enabled(void) { return 0; }
int xpan_watermark_enabled(void) { return 0; }
void xpan_film_grain_poll(struct card_info *card) { (void)card; }

/* ------------------------------------------------------------------ */
/* Capture probe.                                                      */

/* 6D 1.1.6 sites, each a relocatable instruction (MOV/ADD), checked with its
 * neighbours before patching. */
#define TTL_ENTRY 0xff41203cu          /* MOV R4,R0: R0 = TTL job block */
#define TTJ_ENTRY 0xff407044u          /* MOV R4,R0: R0 = TTJ job descriptor */
#define TTJ_AFTER 0xff4076d8u          /* ADD SP,SP,#0x18: R4 = configured job */
#define LOOK_SITE 0xff412550u          /* xpan-native-look.c LOOK_HOOK */
#define BUTTON_SITE 0xff0cc1a8u        /* xpan-hud-controls.c BUTTON_DISPATCH */
/* DIGIC 5 patch_cache: one hook per I-cache index (address bits 0x7E0). */
#if ((TTL_ENTRY^TTJ_ENTRY)&0x7e0u)==0 || ((TTL_ENTRY^TTJ_AFTER)&0x7e0u)==0 || \
    ((TTJ_ENTRY^TTJ_AFTER)&0x7e0u)==0 || ((TTL_ENTRY^LOOK_SITE)&0x7e0u)==0 || \
    ((TTJ_ENTRY^LOOK_SITE)&0x7e0u)==0 || ((TTJ_AFTER^LOOK_SITE)&0x7e0u)==0 || \
    ((TTL_ENTRY^BUTTON_SITE)&0x7e0u)==0 || ((TTJ_ENTRY^BUTTON_SITE)&0x7e0u)==0 || \
    ((TTJ_AFTER^BUTTON_SITE)&0x7e0u)==0
#error 6D probe hooks share an I-cache index with another XPan hook.
#endif

#define PROBE_WORDS 72                 /* 0x120 bytes of each block */
#define PROBE_RECORDS 48
struct probe_record {
    uint32_t site,ms,file,lr,block,r1,r2,r3;
    uint32_t words[PROBE_WORDS];
};
static struct probe_record *records;
static volatile unsigned record_count,record_skipped,record_calls;
static unsigned saved_count;
static int probe_on;                   /* not saved: OFF after a restart */
static unsigned installed;             /* bit 0 TTL, 1 TTJ entry, 2 TTJ exit */
static uint32_t probe_error,guard_address,guard_actual;
static const char *probe_status="OFF";

static int block_in_ram(uint32_t address)
{
    uint32_t ram=address&~0x40000000u;
    return !(address&3) && ram>=0x10000 && ram<=0x20000000u-PROBE_WORDS*4;
}

static void probe_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    (void)stack;
    if (!probe_on || !records) return;
    record_calls++;
    uint32_t block=pc==TTJ_AFTER ? regs[4]:regs[0];
    if (record_count>=PROBE_RECORDS) { record_skipped++; return; }
    struct probe_record *r=&records[record_count];
    struct card_info *card=get_shooting_card();
    r->site=pc; r->ms=get_ms_clock(); r->file=card ? card->file_number:0;
    r->lr=regs[13]; r->block=block; r->r1=regs[1]; r->r2=regs[2]; r->r3=regs[3];
    if (block_in_ram(block)) {
        const volatile uint32_t *w=(const volatile uint32_t *)block;
        for (unsigned i=0;i<PROBE_WORDS;i++) r->words[i]=w[i];
    } else {
        for (unsigned i=0;i<PROBE_WORDS;i++) r->words[i]=0;
    }
    __asm__ __volatile__("" ::: "memory");
    record_count++;
}

/* Canon's debug messages. On the 6D, Magic Lantern already replaces
 * DryosDebugMsg (RAM 0x67C8) with hacked_DebugMsg (gui-common.c,
 * CONFIG_LVAPP_HACK_DEBUGMSG: Canon's own logger never runs), and that
 * replacement passes every message here. Only the raw arguments are stored;
 * they are formatted when saved. The ring keeps running until a photo has
 * finished, then freezes 1.5 s later so the shot's messages are still there
 * when the log is saved. */
#define DM_ARGS 6
struct dm_entry {
    uint32_t seq,timer,fmt,args[DM_ARGS];
    const char *task;
    uint8_t cls,level;
};
static struct dm_entry *dm;
static unsigned dm_entries;
static volatile unsigned dm_head,dm_saved;
static volatile int dm_frozen,dm_installed;

void xpan_6d_debugmsg(int class, int level, const char *fmt, va_list ap)
{
    if (!probe_on || dm_frozen || !dm) return;
    if (class==21) return;             /* ENGIO register writes: thousands per frame */
    uint32_t old=cli(); unsigned i=dm_head++; sei(old);
    struct dm_entry *e=&dm[i%dm_entries];
    e->seq=i; e->timer=GET_DIGIC_TIMER();
    e->cls=class; e->level=level; e->fmt=(uint32_t)fmt;
    /* Words beyond the real arguments are only stack contents, never used
     * unless the format string asks for them. */
    for (unsigned k=0;k<DM_ARGS;k++) e->args[k]=va_arg(ap,uint32_t);
    e->task=get_current_task_name();
}

static int readable(uint32_t p)
{
    uint32_t ram=p&~0x40000000u;
    return (p>=0xff000000u && p<0xffffff00u) || (ram>=0x1000 && ram<0x20000000u-256);
}
/* Canon's format strings with the recorded words; %s only from readable
 * memory, at most DM_ARGS arguments. */
static int dm_format(char *out, int size, const struct dm_entry *e)
{
    if (!readable(e->fmt)) return snprintf(out,size,"(fmt %08x)",e->fmt);
    const char *p=(const char *)e->fmt;
    int n=0; unsigned k=0;
    for (int guard=0; *p && n<size-1 && guard<240; p++,guard++) {
        if (*p=='\n' || *p=='\r') continue;
        if (*p!='%') { out[n++]=*p; continue; }
        p++;
        if (*p=='%') { out[n++]='%'; continue; }
        char spec[16]; int s=0; spec[s++]='%';
        while (*p && strchr("-+ #0123456789.lhz",*p)) { if (*p!='l' && *p!='h' && *p!='z' && s<12) spec[s++]=*p; p++; }
        if (!*p) break;
        uint32_t v=k<DM_ARGS ? e->args[k]:0; k++;
        if (*p=='s') {
            if (!readable(v)) { n+=snprintf(out+n,size-n,"<%08x>",v); }
            else {
                const char *str=(const char *)v;
                for (int i=0; i<64 && str[i] && n<size-1; i++) out[n++]=(str[i]>=32 && str[i]<127) ? str[i]:'.';
            }
        } else if (strchr("diuxXcop",*p)) {
            spec[s++]=*p; spec[s]=0;
            n+=snprintf(out+n,size-n,spec,v);
        } else out[n++]='?';
        if (n>size-1) n=size-1;
    }
    out[n]=0;
    return n;
}

static int dm_install(void)
{
    if (dm_installed) return 1;
    if (!dm) {
        /* As many entries as the AllocateMemory pool gives (6144 down to 1536). */
        static const unsigned sizes[]={6144,3072,1536};
        for (unsigned i=0; i<COUNT(sizes) && !dm; i++) {
            dm_entries=sizes[i]; dm=malloc(dm_entries*sizeof(*dm));
        }
        if (!dm) { probe_status="No memory for the debug log"; return 0; }
    }
    dm_installed=1;
    return 1;
}

static int probe_install(void)
{
    static const uint32_t guards[][2]={
        /* TTL: PUSH {R4-R9,LR}; MOV R4,R0; MOV R3,R0 */
        {0xff412038,0xe92d43f0},{TTL_ENTRY,0xe1a04000},{0xff412040,0xe1a03000},
        /* TTJ: PUSH {R4-R8,LR}; MOV R4,R0; MOV R3,R0 ... ADD SP,#0x18; POP */
        {0xff407040,0xe92d41f0},{TTJ_ENTRY,0xe1a04000},{0xff407048,0xe1a03000},
        {TTJ_AFTER,0xe28dd018},{0xff4076dc,0xe8bd81f0},
    };
    for (unsigned i=0;i<COUNT(guards);i++) {
        uint32_t actual=*(volatile uint32_t *)guards[i][0];
        if (actual!=guards[i][1]) {
            guard_address=guards[i][0]; guard_actual=actual;
            probe_status="ROM mismatch: not a 6D 1.1.6?";
            return 0;
        }
    }
    if (!records) records=malloc(PROBE_RECORDS*sizeof(*records));
    if (!records) { probe_status="No memory for the probe"; return 0; }
    static const struct { uint32_t site,instr,bit; const char *name; } sites[]={
        {TTL_ENTRY,0xe1a04000,1,"XPan 6D probe TTL"},
        {TTJ_ENTRY,0xe1a04000,2,"XPan 6D probe TTJ entry"},
        {TTJ_AFTER,0xe28dd018,4,"XPan 6D probe TTJ exit"},
    };
    for (unsigned i=0;i<COUNT(sites);i++) {
        if (installed&sites[i].bit) continue;
        probe_error=patch_hook_function(sites[i].site,sites[i].instr,probe_hook,sites[i].name);
        if (probe_error) { probe_status="Hook failed: disable all modules"; return 0; }
        installed|=sites[i].bit;
    }
    if (!dm_install()) return 0;
    /* Hooks stay until power-off (as on the EOS M); OFF only stops recording. */
    return 1;
}

static void hex_line(FILE *f, const char *prefix, const uint32_t *w, unsigned n)
{
    char line[160]; int len=snprintf(line,sizeof(line),"%s",prefix);
    for (unsigned i=0;i<n && len<(int)sizeof(line)-10;i++) len+=snprintf(line+len,sizeof(line)-len," %08x",w[i]);
    len+=snprintf(line+len,sizeof(line)-len,"\n");
    FIO_WriteFile(f,line,len);
}

void xpan_native_capture_diag(char *buf, int size);   /* xpan-native-look.c */
static int probe_save(void)
{
    unsigned count=record_count;
    FILE *f=FIO_CreateFileOrAppend("ML/LOGS/X6DPROBE.LOG");
    if (!f) return 0;
    char line[320];
    int n=snprintf(line,sizeof(line),
        "=== XPan 6D capture probe; records %d (new %d) skipped %d calls %d installed %x error %x guard %08x=%08x\n"
        "    quality %08x mode %d style %x aspect %d/%d xpan %d pano %d\n",
        count,count-saved_count,record_skipped,record_calls,installed,probe_error,guard_address,guard_actual,
        (unsigned)pic_quality,shooting_mode,lens_info.raw_picstyle,
        xpan_image_aspect_ratio_valid,(int)xpan_image_aspect_ratio,xpan_photo_enabled(),xpan_photo_is_panoramic());
    FIO_WriteFile(f,line,n);
    char diag[240]; xpan_native_capture_diag(diag,sizeof(diag));
    n=snprintf(line,sizeof(line),"    look: %s\n",diag); FIO_WriteFile(f,line,n);
    for (unsigned i=saved_count;i<count;i++) {
        const struct probe_record *r=&records[i];
        const char *site=r->site==TTL_ENTRY ? "TTL":r->site==TTJ_ENTRY ? "TTJ-in":"TTJ-out";
        n=snprintf(line,sizeof(line),"#%d %s ms %d file %d lr %08x block %08x r1 %08x r2 %08x r3 %08x\n",
            i,site,r->ms,r->file,r->lr,r->block,r->r1,r->r2,r->r3);
        FIO_WriteFile(f,line,n);
        for (unsigned k=0;k<PROBE_WORDS;k+=8) {
            char prefix[16]; snprintf(prefix,sizeof(prefix),"  +%03x:",k*4);
            hex_line(f,prefix,&r->words[k],MIN(8,PROBE_WORDS-k));
        }
    }
    saved_count=count;
    /* Canon debug messages since the last save (the ring keeps the newest). */
    if (dm) {
        unsigned head=dm_head,first=dm_saved;
        if (head-first>dm_entries) {
            n=snprintf(line,sizeof(line),"--- %d older Canon messages overwritten\n",head-first-dm_entries);
            FIO_WriteFile(f,line,n); first=head-dm_entries;
        }
        n=snprintf(line,sizeof(line),"--- Canon debug messages %d..%d (+us since previous, 20-bit timer)\n",first,head);
        FIO_WriteFile(f,line,n);
        uint32_t last_timer=0;
        for (unsigned i=first;i<head;i++) {
            const struct dm_entry *e=&dm[i%dm_entries];
            if (e->seq!=i) continue;       /* still being written */
            uint32_t dt=(e->timer-last_timer)&0xfffff; last_timer=e->timer;
            char msg[220]; dm_format(msg,sizeof(msg),e);
            n=snprintf(line,sizeof(line),"%6d +%6d %-12.12s %02x/%d %s\n",i,i==first ? 0:(int)dt,
                readable((uint32_t)e->task) ? e->task:"?",e->cls,e->level,msg);
            if (n>(int)sizeof(line)-1) n=sizeof(line)-1;
            FIO_WriteFile(f,line,n);
        }
        dm_saved=head;
    }
    FIO_CloseFile(f);
    return 1;
}

static volatile int probe_request,save_request;
static MENU_SELECT_FUNC(probe_select) { (void)priv; (void)delta; probe_request=1; }
static MENU_UPDATE_FUNC(probe_display)
{
    MENU_SET_VALUE("%s",probe_on ? "ON":"OFF");
    MENU_SET_ENABLED(probe_on);
    MENU_SET_HELP("%s. %d jobs (%d not saved), %d Canon messages (%d not saved).",probe_status,
        record_count,record_count-saved_count,dm_head,dm_head-dm_saved);
}
static MENU_SELECT_FUNC(save_select) { (void)priv; (void)delta; save_request=1; }
static MENU_UPDATE_FUNC(save_display)
{
    MENU_SET_VALUE("%d jobs, %d msgs",record_count-saved_count,dm_head-dm_saved);
    if (!records) MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Turn the probe ON and take photos first.");
}
static MENU_UPDATE_FUNC(not_ported_display)
{
    MENU_SET_VALUE("%s",not_ported);
    MENU_SET_WARNING(MENU_WARN_NOT_WORKING,"Needs a 6D capture probe log first.");
}

static struct menu_entry xpan_6d_menu[]={
    { .name="6D capture probe", .select=probe_select, .update=probe_display, .icon_type=IT_BOOL,
      .select_Q=menu_open_submenu,
      .help="Records Canon's jobs and debug messages, one photo at a time. Read only. PLAY: save.",
      .children=(struct menu_entry[]) {
        { .name="Save probe log", .select=save_select, .update=save_display,
          .help="Appends the new records to ML/LOGS/X6DPROBE.LOG; then the next photo is recorded." },
        MENU_EOL
      } },
    { .name="Film grain", .update=not_ported_display,
      .help="Saved-JPEG grain: the EOS M stage between develop and JPEG is not found on the 6D yet." },
    { .name="Watermark", .update=not_ported_display,
      .help="Saved-JPEG watermark: same stage as the film grain." },
};

/* Called by xpan_native_look_add_menu (xpan-workflow.inc). */
void xpan_film_grain_add_menu(void) { menu_add("XPan",xpan_6d_menu,COUNT(xpan_6d_menu)); }

/* File writes and patching happen in this task, never in a menu callback. */
static void xpan_6d_task(void *unused)
{
    (void)unused;
    int was_busy=0; uint32_t idle_since=0;
    while (!ml_shutdown_requested) {
        msleep(200);
        /* Freeze the Canon message ring 1.5 s after a photo has finished. */
        if (probe_on && dm && !dm_frozen) {
            if (lens_info.job_state) { was_busy=1; idle_since=0; }
            else if (was_busy) {
                if (!idle_since) idle_since=get_ms_clock();
                else if (get_ms_clock()-idle_since>=1500) {
                    dm_frozen=1; was_busy=0; idle_since=0;
                    probe_status="Photo recorded: save the log";
                    NotifyBox(3000,"XPan probe: photo recorded, save the log");
                }
            }
        }
        if (probe_request) {
            probe_request=0;
            if (probe_on) { probe_on=0; probe_status="OFF (hooks idle until restart)"; }
            else if (lens_info.job_state) probe_status="Camera busy; try again";
            else if (probe_install()) { probe_on=1; probe_status="Recording"; }
        }
        if (save_request && !lens_info.job_state) {
            save_request=0;
            if (records) {
                if (probe_save()) NotifyBox(2000,"Saved ML/LOGS/X6DPROBE.LOG");
                else NotifyBox(2000,"Could not write the probe log");
                dm_frozen=0; if (probe_on) probe_status="Recording: take the next photo";
            }
        }
    }
}
TASK_CREATE("xpan_6d", xpan_6d_task, 0, 0x1e, 0x1000);
