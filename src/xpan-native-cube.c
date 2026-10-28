/* Manual SD Cube selection/loading and post-shot diagnostics. The parser and
 * RGB16 table are CPU-only; only the final native4096 bytes go to Canon DMA. */
#include "dryos.h"
#include "fileprefix.h"
#include "xpan-cube.h"
#include "xpan-native-cube.h"
#include "lens.h"
#include "picstyle.h"
#include "config.h"

#define MAX_CUBES 16
#define CUBE_NAME_BYTES 48
#define MAX_CUBE_FILE_BYTES (4u*1024u*1024u)
static char cube_names[MAX_CUBES][CUBE_NAME_BYTES];
static unsigned cube_count,cube_index;
static enum xpan_cube_list_state list_state=XPAN_CUBE_LIST_UNSCANNED;
/* Persist name identity, so directory ordering changes do not select a different look. */
static CONFIG_INT("xpan.cube.name",cube_saved_name,0);
static unsigned cube_name_hash(const char *s)
{
    unsigned h=2166136261u;
    while (*s) h=(h^(unsigned char)*s++)*16777619u;
    return h;
}
static uint16_t *cube_rgb;
static struct xpan_lut cube;
static int cube_valid,cube_identity;
static unsigned cube_revision;
static struct card_info *cached_card;
static char cached_name[CUBE_NAME_BYTES];
static uint32_t cube_hash,cube_bytes,cube_load_ms,cube_reused;
static char cube_status[96]="Select a Cube for EOS M Neutral v1";
/* Record setup in the menu worker, never call Canon custom-function getters
 * from the transfer hook. Values describe arming time, not a RAW decoder preset. */
static struct {
    int sharpness,contrast,saturation,tone,alo,htp,matches;
    unsigned wb,kelvin;
    int wb_gm,wb_ba;
} base_at_arm;

const char *xpan_native_cube_base_advice(void)
{
    if (lens_info.raw_picstyle!=0x84) return "Set Canon style to Neutral";
    if (picstyle_get_current_contrast()!=0 ||
        picstyle_get_current_saturation()!=0 || picstyle_get_current_color_tone()!=0)
        return "Set Neutral Contrast/Sat/Tone to 0";
    if (get_alo()!=ALO_OFF) return "Set Canon ALO to OFF";
    if (get_htp()!=0) return "Set Canon HTP to OFF";
    return NULL;
}

void xpan_native_cube_record_base(void)
{
    base_at_arm.sharpness=picstyle_get_current_sharpness();
    base_at_arm.contrast=picstyle_get_current_contrast();
    base_at_arm.saturation=picstyle_get_current_saturation();
    base_at_arm.tone=picstyle_get_current_color_tone();
    base_at_arm.alo=get_alo(); base_at_arm.htp=get_htp();
    /* Matching the recommended reference setup is logged, not enforced. This
     * keeps previously working ALO/HTP/custom-adjustment captures available. */
    base_at_arm.matches=lens_info.raw_picstyle==0x84 &&
        !base_at_arm.contrast && !base_at_arm.saturation && !base_at_arm.tone &&
        base_at_arm.alo==ALO_OFF && !base_at_arm.htp;
    base_at_arm.wb=lens_info.wb_mode; base_at_arm.kelvin=lens_info.kelvin;
    base_at_arm.wb_gm=lens_info.wbs_gm; base_at_arm.wb_ba=lens_info.wbs_ba;
}

const char *xpan_native_cube_filename(void)
{
    if (list_state==XPAN_CUBE_LIST_SCANNING) return "Scanning...";
    if (cube_count) return cube_names[cube_index];
    return list_state==XPAN_CUBE_LIST_EMPTY ? "No .cube files":
        list_state==XPAN_CUBE_LIST_UNAVAILABLE ? "LUT folder unavailable":"Not scanned yet";
}
/* Name the queued wheel/menu steps will select, from the RAM catalogue only:
 * the HUD shows it before the worker has picked the request up. */
const char *xpan_native_cube_filename_after(int delta)
{
    if (list_state!=XPAN_CUBE_LIST_READY || !cube_count) return xpan_native_cube_filename();
    int n=(int)cube_count;
    return cube_names[(((int)cube_index+delta%n)%n+n)%n];
}
unsigned xpan_native_cube_file_count(void) { return cube_count; }
enum xpan_cube_list_state xpan_native_cube_list_state(void) { return list_state; }
void xpan_native_cube_invalidate(void) { cube_valid=0; }
const char *xpan_native_cube_status(void) { return cube_status; }
const struct xpan_lut *xpan_native_cube_table(void) { return cube_valid ? &cube:NULL; }
int xpan_native_cube_identity(void) { return cube_valid && cube_identity; }
unsigned xpan_native_cube_revision(void) { return cube_valid ? cube_revision:0; }

int xpan_native_cube_select(struct card_info *card,int delta,int refresh)
{
    if (!card) {
        list_state=XPAN_CUBE_LIST_UNAVAILABLE;
        snprintf(cube_status,sizeof(cube_status),"No ML card"); return 0;
    }
    if (!cube_count || refresh) {
        char previous[CUBE_NAME_BYTES],path[FIO_MAX_PATH_LENGTH];
        snprintf(previous,sizeof(previous),"%s",cube_count ? cube_names[cube_index]:"IDENTITY.CUBE");
        /* Browsing must not invalidate a table a Canon transfer may be using.
         * cached_name still lets the later loader detect a changed selection. */
        list_state=XPAN_CUBE_LIST_SCANNING;
        cube_count=cube_index=0;
        snprintf(path,sizeof(path),"%s:/ML/XPAN/LUTS/",card->drive_letter);
        struct fio_file file;
        struct fio_dirent *dir=FIO_FindFirstEx(path,&file);
        if (!dir || IS_ERROR(dir)) {
            list_state=XPAN_CUBE_LIST_UNAVAILABLE;
            snprintf(cube_status,sizeof(cube_status),"Cannot read %s",path); return 0;
        }
        unsigned visited=0;
        do {
            /* Bound both directory work and persistent list storage. */
            if (++visited>256 || cube_count>=MAX_CUBES) break;
            unsigned length=strlen(file.name);
            if ((file.mode&ATTR_DIRECTORY) || length<6 || length>=CUBE_NAME_BYTES ||
                strcasecmp(file.name+length-5,".cube") || file.name[0]=='.') continue;
            int valid=1;
            for (unsigned i=0; i<length; i++)
                if ((unsigned char)file.name[i]<32 || (unsigned char)file.name[i]>126 ||
                    file.name[i]=='/' || file.name[i]=='\\' || file.name[i]==':') valid=0;
            if (!valid) continue;
            snprintf(cube_names[cube_count++],CUBE_NAME_BYTES,"%s",file.name);
        } while (!FIO_FindNextEx(dir,&file));
        FIO_FindClose(dir);
        for (unsigned i=0; i<cube_count; i++) for (unsigned j=i+1; j<cube_count; j++)
            if (strcasecmp(cube_names[i],cube_names[j])>0) {
                char temp[CUBE_NAME_BYTES];
                memcpy(temp,cube_names[i],sizeof(temp));
                memcpy(cube_names[i],cube_names[j],sizeof(temp));
                memcpy(cube_names[j],temp,sizeof(temp));
            }
        for (unsigned i=0; i<cube_count; i++)
            if ((cube_saved_name && cube_name_hash(cube_names[i])==(unsigned)cube_saved_name) ||
                (!cube_saved_name && !strcasecmp(previous,cube_names[i]))) cube_index=i;
    } else if (delta) {
        cube_index=(cube_index+cube_count+(delta>0 ? 1:-1))%cube_count;
    }
    list_state=cube_count ? XPAN_CUBE_LIST_READY:XPAN_CUBE_LIST_EMPTY;
    if (cube_count) cube_saved_name=cube_name_hash(cube_names[cube_index]);
    snprintf(cube_status,sizeof(cube_status),cube_count ? "Cube: %s":"No .cube files (max16)",
        cube_count ? cube_names[cube_index]:"");
    return cube_count!=0;
}

int xpan_native_cube_load(struct card_info *card,volatile int *cancel)
{
    if (!cube_count && !xpan_native_cube_select(card,0,1)) return 0;
    const char *name=cube_names[cube_index];
    if (cube_valid && cached_card==card && !strcmp(cached_name,name)) { cube_reused=1; return 1; }
    cube_valid=cube_reused=cube_identity=0;
    uint32_t started=get_ms_clock();
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/ML/XPAN/LUTS/%s",card->drive_letter,name);
    if (FIO_GetFileSize(path,&cube_bytes) || !cube_bytes || cube_bytes>MAX_CUBE_FILE_BYTES) {
        snprintf(cube_status,sizeof(cube_status),"Cube missing or larger than4MiB"); return 0;
    }
    /* Allocate once on explicit use. Retain CPU-only RGB storage until power-off
     * so incomplete hook cleanup can never cause a lifetime race. */
    if (!cube_rgb) cube_rgb=malloc(XPAN_CUBE_MAX_COMPONENTS*sizeof(*cube_rgb));
    if (!cube_rgb) { snprintf(cube_status,sizeof(cube_status),"Not enough memory for Cube"); return 0; }
    FILE *file=FIO_OpenFile(path,O_RDONLY|O_SYNC);
    if (!file) { snprintf(cube_status,sizeof(cube_status),"Could not open Cube"); return 0; }
    struct xpan_cube_parser parser;
    xpan_cube_init(&parser,cube_rgb,XPAN_CUBE_MAX_COMPONENTS);
    char buffer[4096]; uint32_t consumed=0;
    cube_hash=2166136261u;
    int ok=1;
    while (consumed<cube_bytes) {
        if (ml_shutdown_requested || (cancel && *cancel) || get_ml_card()!=card) {
            snprintf(cube_status,sizeof(cube_status),"Cube load cancelled"); ok=0; break;
        }
        if ((uint32_t)(get_ms_clock()-started)>10000) {
            snprintf(cube_status,sizeof(cube_status),"Cube load exceeded10 seconds"); ok=0; break;
        }
        unsigned bytes=MIN((uint32_t)sizeof(buffer),cube_bytes-consumed);
        if (FIO_ReadFile(file,buffer,bytes)!=(int)bytes) {
            snprintf(cube_status,sizeof(cube_status),"Short Cube read"); ok=0; break;
        }
        for (unsigned i=0; i<bytes; i++) cube_hash=(cube_hash^(unsigned char)buffer[i])*16777619u;
        consumed+=bytes;
        if (!xpan_cube_feed(&parser,buffer,bytes)) { ok=0; break; }
    }
    FIO_CloseFile(file);
    if (ok) ok=xpan_cube_finish(&parser);
    if (parser.error) snprintf(cube_status,sizeof(cube_status),"Line%d: %s",
        (int)parser.line_number,xpan_cube_error(&parser));
    if (!ok) return 0;
    if (!xpan_lut_init(&cube,cube_rgb,parser.size,parser.components)) {
        snprintf(cube_status,sizeof(cube_status),"Invalid Cube buffer"); return 0;
    }
    cube_identity=parser.identity; cube_valid=1; cached_card=card;
    /* Identifies loaded RGB contents even when the same buffer/name is reused. */
    if (!++cube_revision) cube_revision=1;
    snprintf(cached_name,sizeof(cached_name),"%s",name);
    cube_load_ms=get_ms_clock()-started;
    snprintf(cube_status,sizeof(cube_status),"Loaded%d grid%s",(int)cube.size,
        cube_identity ? " (identity)":" (experimental RGB)");
    return 1;
}

int xpan_native_cube_save(struct card_info *card,unsigned number,
    const struct xpan_native_cube_stats *stats,unsigned strength,
    uint32_t hook_us,uint32_t matched,uint32_t flags,uint32_t source_hash,uint32_t output_hash)
{
    if (!card) return 0;
    char path[FIO_MAX_PATH_LENGTH],report[2048];
    snprintf(path,sizeof(path),"%s:/CUBE%04d.LOG",card->drive_letter,number);
    FILE *file=FIO_CreateFile(path);
    if (!file) return 0;
    /* ASCII integer fields match the camera's limited printf implementation. */
    int count=snprintf(report,sizeof(report),
        "EOS M native Cube proof 21; CONFIG_QEMU=n\nFile: %s\nGrid: %d\n"
        "Base_ID: " XPAN_CUBE_BASE_ID "\n"
        "Base_policy: Tone-setting advice only; original Neutral format guard retained\n"
        "Recommended_base_match_at_arm: %d\n"
        "Input_profile: Canon Neutral RGB; native transfer, not Log\n"
        "Output_profile: sRGB camera setting required; not verified in firmware\n"
        "Gamma_handling: No additional transfer; native gamma not measured as standard sRGB\n"
        "Automatic_colour_adaptation: None\n"
        "Setup_at_arm_S_C_Sat_T_ALO_HTP: %d %d %d %d %d %d\n"
        "Setup_at_arm_WB_Kelvin_GM_BA: %d %d %d %d\n"
        "Bytes: %d\nFile_FNV1a: %08x\nLoad_ms: %d\nCached: %d\n"
        "RGB16_capacity_bytes: %d\nStrength_Q16: %d\nIdentity_detected: %d\nIdentity_bypass: %d\n"
        "Prepared_entries: %d\nInput_clipped_components: %d\nWorst_input_clip_Q16: %d\n"
        "Output_range_rejections: %d\nHook_us: %d\nMatched: %d\nFormat_flags: %08x\n"
        "Actual_Canon_hash: %08x\nOutput_hash: %08x\n"
        "Native output4096 bytes/1000 colour entries; approximation of source Cube.\n"
        "Loaded outside hook; only native table nodes processed, no image pixels.\n",
        cached_name,(int)cube.size,base_at_arm.matches,base_at_arm.sharpness,base_at_arm.contrast,
        base_at_arm.saturation,base_at_arm.tone,base_at_arm.alo,base_at_arm.htp,
        (int)base_at_arm.wb,(int)base_at_arm.kelvin,base_at_arm.wb_gm,base_at_arm.wb_ba,
        (int)cube_bytes,cube_hash,(int)cube_load_ms,(int)cube_reused,
        (int)(XPAN_CUBE_MAX_COMPONENTS*sizeof(*cube_rgb)),(int)strength,cube_identity,
        (int)stats->identity_bypass,(int)stats->prepared_entries,(int)stats->input_clipped_components,
        (int)stats->worst_input_clip_q16,(int)stats->output_clipped_components,
        (int)hook_us,(int)matched,flags,source_hash,output_hash);
    int ok=count>0 && count<(int)sizeof(report) && FIO_WriteFile(file,report,count)==count;
    FIO_CloseFile(file); return ok;
}

/* Tone bases for the headroom path, loaded from ML/XPAN so a recalibration
 * needs no rebuild. Slot 0: photos (Canon render at JPEG_OBWB -2 EV -> Cineon).
 * Slot 1: Live View (Canon render at normal gains -> the same Cineon), so the
 * LCD preview of a Cineon LUT matches the saved JPEG. */
static struct base_slot {
    const char *file; unsigned capacity;
    uint16_t *rgb; struct xpan_lut lut; int valid;
    struct card_info *card; uint32_t size; char status[48];
} base_slots[2]={
    { .file=XPAN_BASE_FILE, .capacity=33u*33u*33u*3u },
    { .file=XPAN_BASE_LV_FILE, .capacity=17u*17u*17u*3u },
};
static int base_slot_load(struct base_slot *b,struct card_info *card,volatile int *cancel)
{
    char path[FIO_MAX_PATH_LENGTH]; uint32_t bytes=0;
    snprintf(path,sizeof(path),"%s:/ML/XPAN/%s",card->drive_letter,b->file);
    if (FIO_GetFileSize(path,&bytes) || !bytes || bytes>MAX_CUBE_FILE_BYTES) {
        b->valid=0; snprintf(b->status,sizeof(b->status),"%s missing",b->file); return 0;
    }
    if (b->valid && b->card==card && b->size==bytes) return 1;
    b->valid=0;
    if (!b->rgb) b->rgb=malloc(b->capacity*sizeof(*b->rgb));
    if (!b->rgb) { snprintf(b->status,sizeof(b->status),"No memory for %s",b->file); return 0; }
    FILE *file=FIO_OpenFile(path,O_RDONLY|O_SYNC);
    if (!file) { snprintf(b->status,sizeof(b->status),"Cannot open %s",b->file); return 0; }
    struct xpan_cube_parser parser;
    xpan_cube_init(&parser,b->rgb,b->capacity);
    char buffer[4096]; uint32_t consumed=0; int ok=1;
    while (consumed<bytes) {
        if (ml_shutdown_requested || (cancel && *cancel) || get_ml_card()!=card) { ok=0; break; }
        unsigned n=MIN((uint32_t)sizeof(buffer),bytes-consumed);
        if (FIO_ReadFile(file,buffer,n)!=(int)n) { ok=0; break; }
        consumed+=n;
        if (!xpan_cube_feed(&parser,buffer,n)) { ok=0; break; }
    }
    FIO_CloseFile(file);
    if (ok) ok=xpan_cube_finish(&parser);
    if (!ok || !xpan_lut_init(&b->lut,b->rgb,parser.size,parser.components)) {
        snprintf(b->status,sizeof(b->status),"%s invalid (line %d)",b->file,(int)parser.line_number); return 0;
    }
    b->valid=1; b->card=card; b->size=bytes;
    snprintf(b->status,sizeof(b->status),"%s %d grid",b->file,(int)b->lut.size);
    return 1;
}
void xpan_native_base_set_photo_file(const char *file)
{
    if (file && strcmp(base_slots[0].file,file)) { base_slots[0].file=file; base_slots[0].valid=0; }
}
int xpan_native_base_load(struct card_info *card,volatile int *cancel)
{
    int ok=base_slot_load(&base_slots[0],card,cancel);
    base_slot_load(&base_slots[1],card,cancel); /* optional: LCD falls back to Neutral */
    return ok;
}
const struct xpan_lut *xpan_native_base_table(void) { return base_slots[0].valid ? &base_slots[0].lut:NULL; }
const struct xpan_lut *xpan_native_base_lv_table(void) { return base_slots[1].valid ? &base_slots[1].lut:NULL; }
const char *xpan_native_base_status(void) { return base_slots[0].status; }
const char *xpan_native_base_lv_status(void) { return base_slots[1].status; }
