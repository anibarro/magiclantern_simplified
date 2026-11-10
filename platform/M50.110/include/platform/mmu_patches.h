#ifndef __PLATFORM_MMU_PATCHES_H__
#define __PLATFORM_MMU_PATCHES_H__

#include "patch.h"

#if CONFIG_FW_VERSION == 110 // ensure our hard-coded patch addresses are not broken
                             // by a FW upgrade

// Data patches this early on Digic 8 don't seem to work.
// It looks like ROM1 isn't properly mapped or initialised
// at that time, so accesses fail.
struct patch early_data_patches[] =
{

};

struct patch normal_data_patches[] =
{

};


struct function_hook_patch early_code_patches[] =
{

};

#ifdef CONFIG_XPAN
/* src/xpan-m50.c: same job as Canon's E00E73BC (load the still develop's
 * colour table, README-XPAN-M50.md "17-point grid"), with XPan's table when
 * Custom LUT is on. */
extern int xpan_m50_lucky_load(void *table, int a1, int a2, int a3);
/* src/xpan-m50.c: Canon's creative filter grain driver run; XPan's grain
 * instead of Canon's B/W engine, Canon's driver otherwise. README-XPAN-M50.md
 * "XPan's own grain in Canon's filter stage". */
extern int xpan_m50_cf_grain_run(void *job);
/* src/xpan-m50.c: Live View filter map, Canon's "no filter" value for XPan's
 * grain (4 kB small pages). README-XPAN-M50.md "Live View without Canon's
 * filter". */
extern int xpan_m50_lv_cfilter_map(int kind);
/* src/xpan-m50.c: display image layer probe (panel and EVF setters), read
 * only. README-XPAN-M50.md "Live View crop: the display's image layer". */
extern int xpan_m50_disp_panel(void *p, int a1, int a2, int a3, int s0, int s1);
extern int xpan_m50_disp_evf(void *p, int a1, int a2, int a3, int s0, int s1);
/* src/xpan-m50.c: Canon's touch -> AF frame converter, read only. README-XPAN-M50.md
 * "Touch AF and Canon's AF box". */
extern int xpan_m50_af_conv(void *frame, int r1, int r2, int r3, int s0, int s1, int s2, int s3, int s4, int s5, int s6);
/* src/xpan-m50.c: picture area centre row for Canon's AF frame code, moved
 * with the 65:24 band. Same 64 KB page as the converter. */
extern int xpan_m50_area_cy(void);
/* src/xpan-m50.c: AF box on-screen rectangle, row moved down with the band
 * in the Bottom layout. */
extern int xpan_m50_af_box(void *out, int r1, int r2, int r3, int s0, int s1, int s2);
#endif

struct function_hook_patch normal_code_patches[] =
{
#ifdef CONFIG_XPAN
    {
        .patch_addr = 0xe00e73bc, // still develop: load the Lucky colour table (size 0x1000)
        .orig_content = {0x2d, 0xe9, 0xfc, 0x41, 0x1f, 0x46, 0x15, 0x46}, // push.w {r2-r8,lr}; mov r7,r3; mov r5,r2
        .target_function_addr = (uint32_t)xpan_m50_lucky_load,
        .description = "XPan colour table"
    },
    {
        .patch_addr = 0xe086da10, // RoughMonochromeYuv driver run (job), from StartRoughMonoPath, CreativeFilterPath task
        .orig_content = {0x30, 0xb5, 0x04, 0x46, 0x97, 0xb0, 0x18, 0x21}, // push {r4,r5,lr}; mov r4,r0; sub sp,#0x5c; movs r1,#0x18
        .target_function_addr = (uint32_t)xpan_m50_cf_grain_run,
        .description = "XPan grain stage"
    },
    {
        .patch_addr = 0xe0776cc6, // LvCFilterCommon.c: filter kind -> Live View filter, only caller E048CB24
        .orig_content = {0x10, 0xb5, 0x08, 0x28, 0x04, 0x46, 0x0a, 0xd3}, // push {r4,lr}; cmp r0,#8; mov r4,r0; bcc E0776CE4
        .target_function_addr = (uint32_t)xpan_m50_lv_cfilter_map,
        .description = "XPan Live View filter map"
    },
    {
        .patch_addr = 0xe01389ac, // dispVramCtrl: panel image layer setter (p+8 = picture address)
        .orig_content = {0x2d, 0xe9, 0xf0, 0x43, 0x04, 0x46, 0x14, 0x30}, // push.w {r4-r8,sb,lr}; mov r4,r0; adds r0,#0x14
        .target_function_addr = (uint32_t)xpan_m50_disp_panel,
        .description = "XPan display probe (panel)"
    },
    {
        .patch_addr = 0xe01390e0, // dispVramCtrl: EVF image layer setter (same layout)
        .orig_content = {0x2d, 0xe9, 0xf0, 0x43, 0x04, 0x46, 0x14, 0x30}, // push.w {r4-r8,sb,lr}; mov r4,r0; adds r0,#0x14
        .target_function_addr = (uint32_t)xpan_m50_disp_evf,
        .description = "XPan display probe (EVF)"
    },
    {
        .patch_addr = 0xe0423406, // GmtWaku: touch -> AF frame converter (E04235CC/E04236A2 call it)
        .orig_content = {0x2d, 0xe9, 0xff, 0x4f, 0x00, 0x26, 0x85, 0xb0}, // push.w {r0-r11,lr}; movs r6,#0; sub sp,#0x14
        .target_function_addr = (uint32_t)xpan_m50_af_conv,
        .description = "XPan AF converter probe"
    },
    {
        .patch_addr = 0xe042667c, // GmtWaku picture area: centre row getter (sensor units)
        .orig_content = {0xfa, 0x48, 0x80, 0x6a, 0x01, 0x89, 0x40, 0x89}, // ldr r0,[pc,#0x3e8]; ldr r0,[r0,#0x28]; ldrh r1,[r0,#8]; ldrh r0,[r0,#0xa]
        .target_function_addr = (uint32_t)xpan_m50_area_cy,
        .description = "XPan AF picture area"
    },
    {
        .patch_addr = 0xe042268a, // GmtWaku: AF frame -> on-screen rectangle (out+6 = row relative to the area)
        .orig_content = {0x2d, 0xe9, 0xfc, 0x5f, 0x04, 0x46, 0x98, 0x46}, // push.w {r2-r11,ip,lr}; mov r4,r0; mov r8,r3
        .target_function_addr = (uint32_t)xpan_m50_af_box,
        .description = "XPan AF box (Bottom)"
    },
#endif
};

#endif // M50 FW_VERSION 110

#endif // __PLATFORM_MMU_PATCHES_H__
