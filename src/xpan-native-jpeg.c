/* Observe Canon JPEG setup and provide guarded XPan crop/mask experiments.
 * Pixel buffers stay on Canon hardware paths; the optional mask comparison
 * changes one ENGIO shadow value for one capture and restores it after drain. */
#include "dryos.h"
#include "patch.h"
#include "fileprefix.h"
#include "xpan-native-jpeg.h"
#include "cropmarks.h"
#include "xpan-mode.h"
#include "lens.h"

#if !defined(CONFIG_EOSM) || defined(CONFIG_QEMU) || !defined(CONFIG_XPAN)
#error Native JPEG observation requires the physical EOS M 2.0.2 build.
#endif

#define JPEG_BEFORE 0xff4288b0u
#define JPEG_AFTER  0xff428f44u
#define MAX_SNAPSHOTS 32
#define CORE_REGISTERS 14
static int descriptor_in_ram(uint32_t address);

/* Read the same RAM shadow that Canon's FF2C1730/shamem_read uses. These are
 * addresses identifying shadow slots, never direct reads of device registers.
 * Include all layout words of the two possible input and two output channels. */
static const uint32_t shadow_registers[]={
    0xc0f13000,0xc0f13008,0xc0f1300c,0xc0f13010,0xc0f13014,0xc0f13018,0xc0f1301c,
    0xc0f13020,0xc0f13100,0xc0f13104,0xc0f1134c,0xc0f1138c,0xc0f1158c,0xc0f1140c,
    0xc0f26c04,0xc0f26c08,0xc0f26c0c,0xc0f26c10,0xc0f26c14,0xc0f26c18,
    0xc0f26c1c,0xc0f26c20,0xc0f26c24,0xc0f26c28,
    0xc0f26804,0xc0f26808,0xc0f2680c,0xc0f26810,0xc0f26814,0xc0f26818,
    0xc0f2681c,0xc0f26820,0xc0f26824,0xc0f26828,
    0xc0f26504,0xc0f26508,0xc0f2650c,0xc0f26510,0xc0f26514,0xc0f26518,
    0xc0f2651c,0xc0f26520,0xc0f26524,0xc0f26528,
    0xc0f26604,0xc0f26608,0xc0f2660c,0xc0f26610,0xc0f26614,0xc0f26618,
    0xc0f2661c,0xc0f26620,0xc0f26624,0xc0f26628,
};
struct jpeg_snapshot {
    uint32_t stage,relative_us,hook_us,caller_lr,descriptor,shadow_base,shadow_valid;
    /* Channel identities come from this request, rather than a thumbnail's
     * fixed channel assignment. Zero denotes an absent/unsupported channel. */
    uint32_t parameters[44],channel_bases[4],shadow[COUNT(shadow_registers)];
};
/* Allocate only when manually arming; retain the small record buffer until
 * power-off so incomplete cleanup cannot leave a hook pointing to freed RAM. */
static struct jpeg_snapshot *snapshots;
static volatile unsigned snapshot_count,skipped,active,in_hook;
static volatile unsigned calls,invalid_address,reentered;
static unsigned installed;
static uint32_t started_us,patch_error,unpatch_error;
/* Preserve setup evidence even when no picture can be armed. */
static uint32_t failed_stage,guard_address,guard_expected,guard_actual;
static char jpeg_status[64]="Ready";
static unsigned crop_mode,crop_applied,crop_rejected,crop_reason;
static int automatic_mode;
static struct card_info *automatic_card;
static unsigned automatic_file=0xffffffffu,automatic_folder=0xffffffffu;
void xpan_native_jpeg_set_automatic(void) { automatic_mode=1; }

#ifdef CONFIG_XPAN
#define CROP_COPIES 4
#define PROFILE_WORDS 256
/* The original FF3FF1A0 hook shared locked I-cache index 0x1A0 with
 * the native look hook FF42D1AC. Use MOVLO before the alignment tests:
 * capacity has already been compared, and the library preserves those flags. */
#define PLAYBACK_CHECK 0xff3ff198u
#define PLAYBACK_INSTRUCTION 0x33a00005u
/* DrawAspectRatioFrame has already hidden its old controls before this CMP.
 * For an exact XPan raster, force its existing early-return path rather than
 * drawing Canon's 16:9 guide over an already cropped preview. */
#define PLAYBACK_FRAME_CHECK 0xff3b4088u
#define PLAYBACK_FRAME_INSTRUCTION 0xe3500000u
#if ((PLAYBACK_FRAME_CHECK & 0x7e0u) == (PLAYBACK_CHECK & 0x7e0u)) || \
    ((PLAYBACK_FRAME_CHECK & 0x7e0u) == (0xff42d1acu & 0x7e0u)) || \
    ((PLAYBACK_FRAME_CHECK & 0x7e0u) == (JPEG_BEFORE & 0x7e0u)) || \
    ((PLAYBACK_FRAME_CHECK & 0x7e0u) == (JPEG_AFTER & 0x7e0u))
#error XPan playback frame hook conflicts with another camera hook cache index.
#endif
/* DIGIC 4/5 patch_cache uses segment zero and address mask 0x7E0. Keep
 * playback distinct from both JPEG hooks and the native look transfer hook. */
#if ((PLAYBACK_CHECK & 0x7e0u) == (0xff42d1acu & 0x7e0u)) || \
    ((PLAYBACK_CHECK & 0x7e0u) == (JPEG_BEFORE & 0x7e0u)) || \
    ((PLAYBACK_CHECK & 0x7e0u) == (JPEG_AFTER & 0x7e0u))
#error XPan playback hook conflicts with a capture hook cache index.
#endif
#define THUMB_BYTES 174960
/* Two small auxiliary-source snapshots, not sensor frames. These provide the
 * missing evidence for Canon's thumbnail layout after native JPEG cropping. */
struct thumb_snapshot { uint32_t relative_us,source,copy_us,bytes; uint32_t words[THUMB_BYTES/4]; };
static struct thumb_snapshot *thumb_snapshots;
static unsigned thumb_count;
static unsigned playback_installed,playback_frame_installed;
static uint32_t playback_patch_error,playback_guard_address,playback_guard_actual;
static volatile unsigned playback_calls,playback_accepted;
static uint32_t playback_width,playback_height,playback_state;
static volatile unsigned playback_frame_calls,playback_frame_suppressed;
static uint32_t playback_frame_width,playback_frame_height;
static char playback_status[64]="Disabled";

static void playback_alignment_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    if (pc!=PLAYBACK_CHECK || !playback_installed) return;
    playback_calls++;
    playback_width=regs[1]; playback_height=regs[2]; playback_state=regs[5];
    /* GetJpegInfo has already stored the real parsed dimensions at 8CF5C/60
     * and checked destination capacity. R2 is scratch at this final alignment
     * test, unused after returning. The capacity comparison already used the real
     * dimensions; its flags are preserved. Change only the alignment operand for our one
     * exact size; leave parsed dimensions, file data and decoder arguments alone.
     * FF405468 already rounds decoder storage to 8/16-row block boundaries. */
    if (regs[1]==5184 && regs[2]==1914 && regs[5]==0x8cf60 &&
        *(volatile uint32_t *)0x8cf5c==5184 && *(volatile uint32_t *)0x8cf60==1914) {
        regs[2]=1920; playback_accepted++;
    }
    /* The library restores capacity-comparison flags, then executes the
     * original MOVLO R0,#5. BLO still rejects an insufficient buffer. On success,
     * the later TST/TSTEQ checks use scratch R2; all other images are native. */
}

static void playback_frame_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    if (pc!=PLAYBACK_FRAME_CHECK || !playback_frame_installed) return;
    playback_frame_calls++;
    playback_frame_width=*(volatile uint32_t *)0x8cf5c;
    playback_frame_height=*(volatile uint32_t *)0x8cf60;
    /* Canon's parsed dimensions identify this non-native size, not a camera
     * mode/property that could also match an ordinary 16:9 photo. R6 is the
     * verified DlgPlayMain state. The old aspect controls have already been
     * hidden at FF3B4074; leave them hidden via the normal CMP/BEQ return. */
    if (regs[6]==0x51924 && playback_frame_width==5184 &&
        (playback_frame_height==1914 || playback_frame_height==1920)) {
        regs[0]=0; playback_frame_suppressed++;
    }
    /* The relocated CMP sets flags from our result. Canon owns stack cleanup,
     * rendering and all other controls; no bitmap pixels or files are edited. */
}

const char *xpan_native_jpeg_playback_status(void) { return playback_status; }

int xpan_native_jpeg_playback_enable(void)
{
    if (playback_installed && playback_frame_installed) return 1;
    static const uint32_t guards[][2]={
        {0xff3ff044,0xe92d4070},{0xff3ff154,0xe585000c},{0xff3ff160,0xe5850010},
        {0xff3ff194,0x21500002},{PLAYBACK_CHECK,PLAYBACK_INSTRUCTION},
        {0xff3ff19c,0x3affffe8},{0xff3ff1a0,0xe3110007},{0xff3ff1a4,0x03120007},
        {0xff3ff1a8,0x13a00003},{0xff3ff1ac,0x01a00006},{0xff3ff33c,0x0008cf50},
        {0xff4054b0,0xe2800007},{0xff4054b4,0xe3c07007},
        {0xff4054c0,0xe280000f},{0xff4054c8,0xe3c0700f},
        {0xff3b4074,0xebffffce},{0xff3b4078,0xebff4f63},{0xff3b407c,0xe28d203c},
        {0xff3b4080,0xe28d1040},{0xff3b4084,0xebff5e88},
        {PLAYBACK_FRAME_CHECK,PLAYBACK_FRAME_INSTRUCTION},{0xff3b408c,0x0affffed},
        {0xff3b43d0,0x00051924},
    };
    for (unsigned i=0; i<COUNT(guards); i++) {
        uint32_t actual=*(volatile uint32_t *)guards[i][0];
        if (actual!=guards[i][1]) {
            playback_guard_address=guards[i][0]; playback_guard_actual=actual;
            snprintf(playback_status,sizeof(playback_status),"Playback ROM mismatch %08x",guards[i][0]);
            return 0;
        }
    }
    if (!playback_installed) {
        playback_patch_error=patch_hook_function(PLAYBACK_CHECK,PLAYBACK_INSTRUCTION,
            playback_alignment_hook,"XPan 1914 playback alignment");
        if (playback_patch_error) {
            snprintf(playback_status,sizeof(playback_status),"Playback patch error %08x",playback_patch_error);
            return 0;
        }
        playback_installed=1;
    }
    if (!playback_frame_installed) {
        playback_patch_error=patch_hook_function(PLAYBACK_FRAME_CHECK,PLAYBACK_FRAME_INSTRUCTION,
            playback_frame_hook,"XPan already-cropped playback frame");
        if (playback_patch_error) {
            snprintf(playback_status,sizeof(playback_status),"Playback frame error %08x",playback_patch_error);
            return 0;
        }
        playback_frame_installed=1;
    }
    /* Both hooks are installed manually at idle and retained until power-off.
     * A partial install is also retained safely and a retry installs only the
     * missing hook. Neither belongs to the normally removed capture hooks. */
    snprintf(playback_status,sizeof(playback_status),"Enabled until power-off");
    return 1;
}

int xpan_native_jpeg_playback_save(struct card_info *card)
{
    if (!card) return 0;
    char path[FIO_MAX_PATH_LENGTH],report[768];
    unsigned number;
    for (number=0; number<10000; number++) {
        uint32_t bytes;
        snprintf(path,sizeof(path),"%s:/PPLY%04d.LOG",card->drive_letter,number);
        if (FIO_GetFileSize(path,&bytes)) break;
    }
    if (number==10000) return 0;
    FILE *file=FIO_CreateFile(path);
    if (!file) return 0;
    int count=snprintf(report,sizeof(report),
        "EOS M XPan playback probe 3; CONFIG_QEMU=n\n"
        "Installed_until_power_off: %d\nPatch_error: %08x\nGuard_address: %08x\nGuard_actual: %08x\n"
        "Alignment_calls: %d\nExact_1914_accepted: %d\nLast_width: %d\nLast_height: %d\nLast_state: %08x\n"
        "Frame_hook_installed: %d\nFrame_calls: %d\nExact_frame_suppressed: %d\nFrame_last_width: %d\nFrame_last_height: %d\n"
        "Only scratch R2 before FF3FF198 is rounded for a guarded 5184x1914 alignment test.\n"
        "Parsed sizes, decoder arguments and files are unchanged by this playback hook.\n",
        (int)playback_installed,playback_patch_error,playback_guard_address,playback_guard_actual,
        (int)playback_calls,(int)playback_accepted,(int)playback_width,(int)playback_height,playback_state,
        (int)playback_frame_installed,(int)playback_frame_calls,(int)playback_frame_suppressed,
        (int)playback_frame_width,(int)playback_frame_height);
    int ok=count>0 && count<(int)sizeof(report) && FIO_WriteFile(file,report,count)==count;
    FIO_CloseFile(file);
    if (ok) snprintf(playback_status,sizeof(playback_status),"%s; PPLY%04d saved",
        (playback_installed && playback_frame_installed) ? "Enabled":"Incomplete/disabled",number);
    return ok;
}

int xpan_native_jpeg_log_number_available(struct card_info *card, unsigned number)
{
    char path[FIO_MAX_PATH_LENGTH]; uint32_t bytes;
    static const char *names[]={"NCRP%04d.LOG","NCRP%04d.BIN","NTHM%04d.BIN"};
    for (unsigned i=0; i<COUNT(names); i++) {
        char name[24]; snprintf(name,sizeof(name),names[i],number);
        snprintf(path,sizeof(path),"%s:/%s",card->drive_letter,name);
        if (!FIO_GetFileSize(path,&bytes)) return 0;
    }
    return 1;
}

static void observe_thumbnail_source(const struct jpeg_snapshot *shot)
{
    if (!crop_mode || thumb_count>=2 || shot->descriptor!=0x6ffbc ||
        shot->parameters[0]!=0xff91f040 || shot->parameters[2]!=0xff920424 ||
        (shot->parameters[0x38/4]>>16)!=160 || shot->parameters[0x3c/4]!=120 ||
        shot->parameters[0x40/4]!=0 || shot->parameters[0x58/4]!=0x40da4000 ||
        shot->parameters[0x5c/4]!=0) return;
    uint32_t entered=(uint32_t)get_us_clock();
    struct thumb_snapshot *copy=&thumb_snapshots[thumb_count];
    copy->relative_us=entered-started_us; copy->source=0x40da4000; copy->bytes=THUMB_BYTES;
    /* The observed thumbnail DMA consumes exactly 360*486 bytes here. Copy
     * only this fixed, RAM-only source after Canon's formatter, never RAW data.
     * No file access, JPEG decoding or formatting happens inside this hook. */
    const volatile uint32_t *source=(const volatile uint32_t *)0x40da4000;
    for (unsigned i=0; i<COUNT(copy->words); i++) copy->words[i]=source[i];
    copy->copy_us=(uint32_t)get_us_clock()-entered; thumb_count++;
}
struct crop_descriptor { uint32_t words[44],original; };
struct crop_workspace {
    uint32_t profile[PROFILE_WORDS];
    struct crop_descriptor copies[CROP_COPIES];
};
/* CPU-read descriptors/lists need no DMA allocation. Retain their small backing
 * store until power-off, including after incomplete instruction-hook cleanup. */
static struct crop_workspace *crop_workspace;
static unsigned profile_words;
/* Normalize Canon's cached/uncached aliases for the private crop descriptor. */
static uint32_t crop_probe_unalias(uint32_t p) { return p&~0x40000000u; }
/* JPEG_AFTER receives the private R4 descriptor installed by JPEG_BEFORE.
 * Match its owning workspace copy and original Canon descriptor, not the
 * original 0x70298 pointer that the crop hook deliberately replaces. */
static int mask_probe_xpan_crop_copy(uint32_t address,uint32_t *original)
{
    if (!crop_workspace || !address) return 0;
    uint32_t target=crop_probe_unalias(address);
    for (unsigned i=0;i<crop_applied && i<CROP_COPIES;i++) {
        struct crop_descriptor *copy=&crop_workspace->copies[i];
        if (crop_probe_unalias((uint32_t)copy->words)==target &&
            crop_probe_unalias(copy->original)==0x70298) {
            if (original) *original=crop_probe_unalias(copy->original);
            return 1;
        }
    }
    return 0;
}

static int prepare_crop_profile(unsigned mode)
{
    uint32_t address=mode==1 ? 0xff912bd0u:0xff9125a8u;
    unsigned pairs=mode==1 ? 101:96;
    uint32_t expected_hash=mode==1 ? 0x47cf4906u:0x6f7a98a1u,hash=2166136261u;
    const volatile uint32_t *source=(const volatile uint32_t *)address;
    profile_words=pairs*2;
    for (unsigned i=0; i<profile_words; i++) {
        uint32_t word=source[i];
        crop_workspace->profile[i]=word; hash=(hash^word)*16777619u;
    }
    /* Fingerprints include every pair and both sentinel words from the supplied
     * EOS M ROM. A different firmware/profile must leave Canon untouched. */
    if (hash!=expected_hash || source[profile_words-2]!=0xffffffffu ||
        source[profile_words-1]!=0xffffffffu) return 0;
    if (mode==1) return 1;
    unsigned changed=0;
    for (unsigned i=0; i<profile_words-2; i+=2) {
        uint32_t reg=crop_workspace->profile[i],value=crop_workspace->profile[i+1];
        switch (reg) {
            /* Canon's 3:2/16:9 profiles use these four 8-row pipeline counts
             * and the output-15 count. 1920/8-1=239; keep all width bits. */
            case 0xc0f1134c: case 0xc0f1138c: case 0xc0f1140c:
            case 0xc0f1158c: case 0xc0f2650c:
                if (value!=0x01af0008) return 0;
                crop_workspace->profile[i+1]=0x00ef0008; changed++; break;
            case 0xc0f26c0c:
                /* Input-1C uses 16-row groups: 1920/16-1=119. */
                if (value!=0x00d70008) return 0;
                crop_workspace->profile[i+1]=0x00770008; changed++; break;
            case 0xc0f13018:
                if (value!=0x0d7f023f) return 0;
                crop_workspace->profile[i+1]=0x077f023f; changed++; break;
        }
    }
    /* Keep 1920 processing rows for mode 3 as well: 1914 and 1920 occupy the
     * same JPEG block grid. Only the encoder's requested output height differs;
     * no unverified partial input/output DMA group is introduced. */
    return changed==7;
}

static void redirect_crop_descriptor(uint32_t *regs)
{
    /* Keep native 3:2 JPEG pixels while leaving Canon's LUT processing installed. */
    if (!crop_mode || !xpan_photo_is_panoramic() || regs[0]!=0x70298) return;
    const volatile uint32_t *source=(const volatile uint32_t *)regs[0];
    /* Test20 proves quality8 is the standalone JPEG and quality0 is the CR2
     * preview, reversing the old assumption. Both need the same centered frame.
     * Crop both as Test19 did; preserve the full Bayer RAW compression job. */
    /* Test17 proves this ordinary-JPEG, full-width photo path. Both calls (the
     * large preview and final JPEG) may be configured in one RAW+JPEG shot.
     * Precision 14 RAW compression, thumbnails and all other blocks are excluded. */
    if (source[0]!=0xff90f600 || source[1]!=0x26 || source[2]!=0xff9125a8 ||
        source[0x38/4]!=0x1440ffff || source[0x3c/4]!=3456 || source[0x40/4]!=0 ||
        (source[0x44/4]!=0 && source[0x44/4]!=8) ||
        source[0x48/4]!=0x1c || source[0x50/4]!=0x20000000 || source[0x5c/4]!=0 ||
        source[0x70/4]!=0 || source[0x78/4]!=0x16 || source[0x90/4]!=0x15) {
        crop_rejected++; crop_reason=1; return;
    }
    unsigned height=crop_mode==1 ? 2912:crop_mode==3 ? 1914:1920;
    unsigned top=(3456-height)/2,padded_height=(height+15)&~15u;
    uint32_t offset=top*10368u,physical=source[0x58/4]&~0x40000000u;
    /* Test18 same-shot RAW registration confirms pitch=10368 and centered top
     * rows 272/768. Mode 3 starts at 771 and supplies 1920 valid source rows;
     * the requested JPEG height is 1914. All padded rows remain inside 3456.
     * Validate against the native 2-byte/pixel pitch and RAM, not the misleading
     * half-height transfer-unit total from the generic EDMAC address model. */
    if ((source[0x58/4]&0xf0000000u)!=0x40000000u || (physical&127) ||
        physical<0x08000000 || physical>0x10000000u-10368u*3456u ||
        top+padded_height>3456 || offset+10368u*padded_height>10368u*3456u) {
        crop_rejected++; crop_reason=2; return;
    }
    if (crop_applied>=CROP_COPIES) { crop_rejected++; crop_reason=3; return; }
    struct crop_descriptor *copy=&crop_workspace->copies[crop_applied];
    for (unsigned i=0; i<COUNT(copy->words); i++) copy->words[i]=source[i];
    copy->original=regs[0];
    copy->words[2]=(uint32_t)crop_workspace->profile;
    copy->words[0x3c/4]=height;
    copy->words[0x5c/4]=offset;
    __asm__ __volatile__("" ::: "memory");
    /* The relocated MOV R4,R0 makes Canon read this private copy. Its normal
     * setup writes the profile and starts/finishes encoding via existing code.
     * No camera property or original 70298 field needs restoration. */
    regs[0]=(uint32_t)copy->words; crop_applied++;
}
#endif

static int descriptor_in_ram(uint32_t address)
{
    /* Canon already dereferences this argument in the hooked routine. Bound
     * our copy to one aligned, complete 176-byte block in EOS M's RAM window;
     * allow its cached and uncached aliases, never ROM or device addresses. */
    uint32_t ram=address&~0x40000000u;
    return !(address&3) && ram>=0x10000 && ram<=0x10000000-176;
}

static void jpeg_observe_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    if (!active) return;
    if (automatic_mode)
    {
        /* Crop Canon's normal photo jobs, including the RAW embedded preview.
         * Do not fill observer/thumbnail buffers or exhaust a one-shot quota. */
        if (!xpan_photo_enabled() || is_movie_mode() || pc!=JPEG_BEFORE || regs[0]!=0x70298) return;
        struct card_info *card=get_shooting_card();
        if (!card) return;
        if (card!=automatic_card || (unsigned)card->file_number!=automatic_file ||
            (unsigned)card->folder_number!=automatic_folder)
        {
            automatic_card=card; automatic_file=card->file_number;
            automatic_folder=card->folder_number;
            /* Canon serializes these setup jobs. Keep each photo's two copies
             * distinct and reuse the pool only at its next file identity. */
            crop_applied=crop_rejected=crop_reason=0;
        }
#ifdef CONFIG_XPAN
        /* Snapshot the color stage through this already installed callback.
         * No extra patch or image buffer is needed for JPEG-only diagnostics. */
        xpan_native_look_note_jpeg();
#endif
        redirect_crop_descriptor(regs);
        return;
    }
    calls++;
    if (in_hook) { reentered++; return; }
    if (pc!=JPEG_BEFORE && pc!=JPEG_AFTER) return;
    uint32_t address=pc==JPEG_BEFORE ? regs[0]:regs[4];
    /* Test16 proves that a descriptor allow-list hid other Canon jobs. The
     * photo developer constructs 70298 too (FF16FFA8..FF17042C). Observe every
     * RAM argument at this exact setup routine; dimensions/precision in the
     * log distinguish JPEG photos, thumbnails and lossless RAW configurations. */
    if (!descriptor_in_ram(address)) { invalid_address++; return; }
    if (snapshot_count>=MAX_SNAPSHOTS) { skipped++; return; }
    in_hook=1;
    uint32_t entered=(uint32_t)get_us_clock();
    struct jpeg_snapshot *shot=&snapshots[snapshot_count];
    shot->stage=pc==JPEG_BEFORE ? 0:1;
    shot->relative_us=entered-started_us; shot->caller_lr=regs[13];
    shot->descriptor=address;
    const volatile uint32_t *parameter=(const volatile uint32_t *)address;
    for (unsigned i=0; i<COUNT(shot->parameters); i++) shot->parameters[i]=parameter[i];
    static const unsigned channel_fields[]={0x48/4,0x60/4,0x78/4,0x90/4};
    static const uint32_t edmac_blocks[]={0xc0f04000,0xc0f26000,0xc0f30000};
    for (unsigned i=0; i<COUNT(channel_fields); i++) {
        uint32_t channel=shot->parameters[channel_fields[i]];
        /* Same bank arithmetic as src/edmac.c; no hardware register access. */
        shot->channel_bases[i]=channel<48 ? edmac_blocks[channel>>4]+((channel&15)<<8):0;
    }
    uint32_t base=*(volatile uint32_t *)0x4c338;
    uint32_t ram=base&~0x40000000u;
    shot->shadow_base=base;
    /* Canon combines the low 18 register bits with this base using OR. Only
     * accept an aligned, complete 256 KiB shadow region inside observed RAM. */
    shot->shadow_valid=!(base&0x3ffff) && ram>=0x10000 && ram<=0x08000000-0x40000;
    if (shot->shadow_valid) {
        for (unsigned i=0; i<CORE_REGISTERS; i++)
            shot->shadow[i]=*(volatile uint32_t *)(base|(shadow_registers[i]&0x3ffff));
        for (unsigned channel=0; channel<4; channel++) {
            uint32_t registers=shot->channel_bases[channel];
            for (unsigned i=0; i<10; i++)
                shot->shadow[CORE_REGISTERS+channel*10+i]=registers ?
                    *(volatile uint32_t *)(base|((registers+4+i*4)&0x3ffff)):0;
        }
    }
#ifdef CONFIG_XPAN
    /* Record the original descriptor before redirecting, and the actual private
     * descriptor plus hardware shadow at JPEG_AFTER. Snapshot storage exhaustion
     * skips the experiment as well, preserving evidence for every change. */
    if (pc==JPEG_BEFORE) { observe_thumbnail_source(shot); redirect_crop_descriptor(regs); }
#endif
    shot->hook_us=(uint32_t)get_us_clock()-entered;
    __asm__ __volatile__("" ::: "memory");
    snapshot_count++; in_hook=0;
    /* The patch library preserves flags and executes the relocated MOV/ADD.
     * R0 differs only in an explicitly enabled, guarded crop proof above. */
}

int xpan_native_jpeg_hooks_remain(void) { return installed!=0; }
const char *xpan_native_jpeg_status(void) { return jpeg_status; }
unsigned xpan_native_jpeg_crop_count(void) { return crop_applied; }

int xpan_native_jpeg_stop(int idle)
{
    active=0;
    /* An idle capture job plus the caller's 1 s grace period precedes removal.
     * A busy shot retains inactive hooks until reboot, as in the look proof. */
    if (!idle || in_hook) return !installed;
    if (installed&2) {
        int error=unpatch_memory(JPEG_AFTER); unpatch_error|=error;
        if (!error) installed&=~2u;
    }
    if (installed&1) {
        int error=unpatch_memory(JPEG_BEFORE); unpatch_error|=error;
        if (!error) installed&=~1u;
    }
    return !installed;
}

int xpan_native_jpeg_start(int requested_crop_mode)
{
    if (installed) return 0;
    active=0; snapshot_count=skipped=in_hook=0;
    calls=invalid_address=reentered=0;
    crop_mode=crop_applied=crop_rejected=crop_reason=0;
#ifdef CONFIG_XPAN
    thumb_count=0;
#endif
    patch_error=unpatch_error=0;
    failed_stage=guard_address=guard_expected=guard_actual=0;
    snprintf(jpeg_status,sizeof(jpeg_status),"Preparing JPEG observer");
    if (!automatic_mode && !snapshots) snapshots=malloc(MAX_SNAPSHOTS*sizeof(*snapshots));
    if (!automatic_mode && !snapshots) {
        patch_error=0xfffffffeu; failed_stage=1;
        snprintf(jpeg_status,sizeof(jpeg_status),"JPEG observer: no memory");
        return 0;
    }
    if (snapshots) memset(snapshots,0,MAX_SNAPSHOTS*sizeof(*snapshots));
#ifdef CONFIG_XPAN
    if (requested_crop_mode<0 || requested_crop_mode>3) return 0;
    crop_mode=(unsigned)requested_crop_mode;
    if (crop_mode) {
        if (!automatic_mode && !thumb_snapshots) thumb_snapshots=malloc(2*sizeof(*thumb_snapshots));
        if (!automatic_mode && !thumb_snapshots) {
            patch_error=0xfffffffcu; failed_stage=6;
            snprintf(jpeg_status,sizeof(jpeg_status),"No thumbnail snapshot memory"); return 0;
        }
        if (crop_mode==3 && !xpan_native_jpeg_playback_enable()) {
            patch_error=0xfffffffbu; failed_stage=7;
            snprintf(jpeg_status,sizeof(jpeg_status),"%s",playback_status); return 0;
        }
        if (!crop_workspace) crop_workspace=malloc(sizeof(*crop_workspace));
        if (!crop_workspace || !prepare_crop_profile(crop_mode)) {
            patch_error=0xfffffffdu; failed_stage=5;
            snprintf(jpeg_status,sizeof(jpeg_status),"Crop profile unavailable; unchanged");
            return 0;
        }
        memset(crop_workspace->copies,0,sizeof(crop_workspace->copies));
    }
#else
    if (requested_crop_mode) return 0;
#endif
    /* Check both hook instructions and their neighbours against the supplied
     * EOS M 2.0.2 ROM. Never install guessed entry points on another firmware. */
    static const uint32_t guards[][2]={
        {0xff4288ac,0xe92d41f0},{JPEG_BEFORE,0xe1a04000},{0xff4288b4,0xe1a03000},
        /* The supplied ROM has an unconditional BL (EB), not BLNE (1B).
         * V4's incorrect condition nibble rejected the correct firmware. */
        {0xff428f40,0xeb2f873e},{JPEG_AFTER,0xe28dd018},{0xff428f48,0xe8bd81f0},
    };
    for (unsigned i=0; i<COUNT(guards); i++) {
        uint32_t actual=*(volatile uint32_t *)guards[i][0];
        if (actual!=guards[i][1]) {
            patch_error=0xffffffffu; failed_stage=2;
            guard_address=guards[i][0]; guard_expected=guards[i][1]; guard_actual=actual;
            snprintf(jpeg_status,sizeof(jpeg_status),"JPEG map mismatch: %08x",guard_address);
            return 0;
        }
    }
    patch_error=patch_hook_function(JPEG_BEFORE,0xe1a04000,jpeg_observe_hook,"XPan JPEG setup observation");
    if (patch_error) {
        failed_stage=3;
        snprintf(jpeg_status,sizeof(jpeg_status),"JPEG entry patch: %08x",patch_error);
        return 0;
    }
    installed=1;
    patch_error=patch_hook_function(JPEG_AFTER,0xe28dd018,jpeg_observe_hook,"XPan JPEG configured observation");
    if (patch_error) {
        failed_stage=4; xpan_native_jpeg_stop(1);
        snprintf(jpeg_status,sizeof(jpeg_status),installed ? "JPEG cleanup failed; reboot":"JPEG exit patch: %08x",patch_error);
        return 0;
    }
    installed|=2;
    started_us=(uint32_t)get_us_clock(); active=1;
    snprintf(jpeg_status,sizeof(jpeg_status),"JPEG observer armed");
    return 1;
}

int xpan_native_jpeg_save(struct card_info *card, unsigned number)
{
    char path[FIO_MAX_PATH_LENGTH],text_path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/NCRP%04d.BIN",card->drive_letter,number);
    snprintf(text_path,sizeof(text_path),"%s:/NCRP%04d.LOG",card->drive_letter,number);
    uint32_t bytes;
    /* Keep earlier card captures intact even if NLUT and NCRP number sets differ. */
    if (!FIO_GetFileSize(path,&bytes) || !FIO_GetFileSize(text_path,&bytes)) return 0;
    FILE *file=FIO_CreateFile(path);
    if (!file) return 0;
    /* V4 retains V3's 436-byte records and extends the header to 64 bytes with
     * crop mode, redirection count, rejection count and rejection reason. */
    uint32_t header[]={0x434e5058,0x00345052,4,0x12345678,sizeof(struct jpeg_snapshot),
        snapshot_count,COUNT(shadow_registers),skipped,patch_error,unpatch_error,installed,44,
        crop_mode,crop_applied,crop_rejected,crop_reason};
    int ok=FIO_WriteFile(file,header,sizeof(header))==(int)sizeof(header) &&
        FIO_WriteFile(file,shadow_registers,sizeof(shadow_registers))==(int)sizeof(shadow_registers) &&
        (!snapshot_count || FIO_WriteFile(file,snapshots,snapshot_count*sizeof(*snapshots))==
         (int)(snapshot_count*sizeof(*snapshots)));
    FIO_CloseFile(file);
    if (!ok) return 0;
    /* Save an actionable error even if arming failed before the first shot.
     * Detailed guard values and capture coverage live in this text log. */
    file=FIO_CreateFile(text_path);
    if (!file) return 0;
    char report[768];
    unsigned manager_count=0,legacy_count=0,main_count=0,photo_sized=0;
    for (unsigned i=0; i<snapshot_count; i++) {
        if (snapshots[i].descriptor==0x72440) manager_count++;
        if (snapshots[i].descriptor==0x6ffbc) legacy_count++;
        if (snapshots[i].descriptor==0x70298) main_count++;
        if ((snapshots[i].parameters[0x38/4]>>16)>=1024 &&
            (snapshots[i].parameters[0x3c/4]&65535)>=1024 &&
            snapshots[i].parameters[0x40/4]==0) photo_sized++;
    }
    int count=snprintf(report,sizeof(report),
        "EOS M native JPEG observer revision 9 / crop proof 5; CONFIG_QEMU=n\n"
        "Failed_stage: %d (1=memory,2=ROM,3=entry,4=exit,5=profile,6=thumbnail memory,7=playback)\n"
        "Patch_error: %08x\nUnpatch_error: %08x\nHooks_remaining: %d\n"
        "Guard_address: %08x\nGuard_expected: %08x\nGuard_actual: %08x\n"
        "Hook_calls: %d\nInvalid_RAM_arguments: %d\nReentered: %d\n"
        "Snapshots: %d\nSkipped: %d\nManager_72440_snapshots: %d\n"
        "Legacy_6FFBC_snapshots: %d\nPhoto_developer_70298_snapshots: %d\n"
        "Photo_sized_JPEG_snapshots: %d\n",
        (int)failed_stage,patch_error,unpatch_error,(int)installed,
        guard_address,guard_expected,guard_actual,(int)calls,(int)invalid_address,(int)reentered,
        (int)snapshot_count,(int)skipped,(int)manager_count,(int)legacy_count,
        (int)main_count,(int)photo_sized);
    ok=count>0 && count<(int)sizeof(report) && FIO_WriteFile(file,report,count)==count;
    count=snprintf(report,sizeof(report),
        "Crop_mode: %d (0=observe,1=native16:9,2=5184x1920,3=5184x1914 exact experiment)\n"
        "Crop_redirected: %d\nCrop_rejected: %d\nCrop_reason: %d (1=request,2=source,3=capacity)\n"
        "Original Canon descriptors/pixels unchanged; optional private geometry is experimental.\n"
        "Mode 2: 1920 output and processing rows. JPEG and RAW preview are both cropped.\n",
        (int)crop_mode,(int)crop_applied,(int)crop_rejected,(int)crop_reason);
    ok=ok && count>0 && count<(int)sizeof(report) && FIO_WriteFile(file,report,count)==count;
#ifdef CONFIG_XPAN
    count=snprintf(report,sizeof(report),
        "Thumbnail_sources: %d\n"
        "Playback_hook_until_power_off: %d\nPlayback_alignment_calls: %d\nPlayback_1914_accepted: %d\n",
        (int)thumb_count,(int)playback_installed,
        (int)playback_calls,(int)playback_accepted);
    ok=ok && count>0 && count<(int)sizeof(report) && FIO_WriteFile(file,report,count)==count;
    for (unsigned i=0; ok && i<crop_applied; i++) {
        struct crop_descriptor *copy=&crop_workspace->copies[i];
        count=snprintf(report,sizeof(report),
            "Crop_copy_%d: %08x original=%08x profile=%08x height=%d offset=%d\n",
            (int)i,(uint32_t)copy->words,copy->original,copy->words[2],
            (int)copy->words[0x3c/4],(int)copy->words[0x5c/4]);
        ok=count>0 && count<(int)sizeof(report) && FIO_WriteFile(file,report,count)==count;
    }
#endif
    /* Include coverage in plain text so a missed route is visible without the
     * desktop decoder. All file writes happen after hooks have been stopped. */
    for (unsigned i=0; ok && i<snapshot_count; i++) {
        struct jpeg_snapshot *shot=&snapshots[i];
        count=snprintf(report,sizeof(report),
            "%d stage=%d descriptor=%08x size=%dx%d precision=%d caller=%08x hook_us=%d\n",
            (int)i,(int)shot->stage,shot->descriptor,(int)(shot->parameters[0x38/4]>>16),
            (int)(shot->parameters[0x3c/4]&65535),(int)shot->parameters[0x40/4],
            shot->caller_lr,(int)shot->hook_us);
        ok=count>0 && count<(int)sizeof(report) && FIO_WriteFile(file,report,count)==count;
    }
    FIO_CloseFile(file);
#ifdef CONFIG_XPAN
    if (ok && crop_mode) {
        /* Keep each auxiliary source beside its same-number capture logs. File
         * writes occur after capture hooks stop, never inside either callback. */
        snprintf(path,sizeof(path),"%s:/NTHM%04d.BIN",card->drive_letter,number);
        if (!FIO_GetFileSize(path,&bytes)) return 0;
        file=FIO_CreateFile(path);
        if (!file) return 0;
        uint32_t thumb_header[]={0x544e5058,0x00314d48,1,0x12345678,
            sizeof(struct thumb_snapshot),thumb_count,THUMB_BYTES,crop_mode};
        ok=FIO_WriteFile(file,thumb_header,sizeof(thumb_header))==(int)sizeof(thumb_header) &&
            (!thumb_count || FIO_WriteFile(file,thumb_snapshots,thumb_count*sizeof(*thumb_snapshots))==
             (int)(thumb_count*sizeof(*thumb_snapshots)));
        FIO_CloseFile(file);
    }
#endif
    return ok;
}
