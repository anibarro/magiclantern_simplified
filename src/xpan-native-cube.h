#ifndef XPAN_NATIVE_CUBE_H
#define XPAN_NATIVE_CUBE_H
#include "xpan-native-table.h"
struct card_info;
/* Version the authoring base independently of parser/build revisions. No
 * automatic camera/gamma conversion is implied by this profile name. */
#define XPAN_CUBE_BASE_ID "EOSM_NEUTRAL_V1"
#define XPAN_CUBE_BASE_NAME "EOS M Neutral v1"
/* Menu advice only: do not use these authoring recommendations to veto capture. */
const char *xpan_native_cube_base_advice(void);
void xpan_native_cube_record_base(void);
/* Catalogue operations do not change the currently loaded capture table.
 * Loading/invalidation runs only at the workflow's safe reconfiguration point. */
enum xpan_cube_list_state {
    XPAN_CUBE_LIST_UNSCANNED, XPAN_CUBE_LIST_SCANNING,
    XPAN_CUBE_LIST_READY, XPAN_CUBE_LIST_EMPTY, XPAN_CUBE_LIST_UNAVAILABLE
};
int xpan_native_cube_select(struct card_info *card,int delta,int refresh);
unsigned xpan_native_cube_file_count(void);
enum xpan_cube_list_state xpan_native_cube_list_state(void);
void xpan_native_cube_invalidate(void);
const char *xpan_native_cube_filename(void);
const char *xpan_native_cube_filename_after(int delta);
const char *xpan_native_cube_status(void);
int xpan_native_cube_load(struct card_info *card,volatile int *cancel);
const struct xpan_lut *xpan_native_cube_table(void);
int xpan_native_cube_identity(void);
unsigned xpan_native_cube_revision(void);
int xpan_native_cube_save(struct card_info *card,unsigned number,
    const struct xpan_native_cube_stats *stats,unsigned strength,
    uint32_t hook_us,uint32_t matched,uint32_t flags,uint32_t source_hash,uint32_t output_hash);
/* Headroom tone base (Cineon log), ML/XPAN/<file>. */
/* v7: photo base calibrated for the full-HTP render (v6 CINEONH, v5 CINEON). */
#define XPAN_BASE_FILE "CINEON7.CUBE"          /* JPEG_OBWB -1 EV (highlight headroom +1) */
#define XPAN_BASE_FILE_2EV "CINEON7_2EV.CUBE"  /* JPEG_OBWB -2 EV (highlight headroom +2) */
/* Select the photo base file; a change forces a reload. */
void xpan_native_base_set_photo_file(const char *file);
int xpan_native_base_load(struct card_info *card,volatile int *cancel);
const struct xpan_lut *xpan_native_base_table(void);
const char *xpan_native_base_status(void);
/* Live View base (Canon render at normal gains -> same Cineon), 17^3 max. */
#define XPAN_BASE_LV_FILE "CINEONLV.CUBE"
const struct xpan_lut *xpan_native_base_lv_table(void);
const char *xpan_native_base_lv_status(void);
#endif
