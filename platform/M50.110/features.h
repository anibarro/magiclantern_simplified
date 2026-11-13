/* XPan panoramic photo mode (README-XPAN.md, README-XPAN-M50.md): shared code for supported bodies. */
#define CONFIG_XPAN_BODY

#define FEATURE_VRAM_RGBA

// Don't Click Me menu looks to be intended as a place
// for devs to put custom code in debug.c run_test(),
// and allowing triggering from a menu context.
#define FEATURE_DONT_CLICK_ME

#define FEATURE_SHOW_SHUTTER_COUNT

// working but incomplete, some allocators don't report
// anything yet as they're faked / not yet found
#define FEATURE_SHOW_FREE_MEMORY

// Task monitor off for the XPan M50 build: it hooks every task switch from
// boot_pre_init_task on, and failed starts stop between that point and
// Canon's init_task returning (README-XPAN-M50.md, boot log 12).
#undef CONFIG_TSKMON
#undef FEATURE_SHOW_TASKS
#undef FEATURE_SHOW_CPU_USAGE
#define FEATURE_SHOW_GUI_EVENTS

#define CONFIG_ADDITIONAL_VERSION
#define FEATURE_SCREENSHOT

// enable global draw
#define FEATURE_GLOBAL_DRAW
#define FEATURE_CROPMARKS

// XPan draws its photo HUD through flexinfo (src/xpan-photo-hud.inc)
#define FEATURE_FLEXINFO

// MMU remapping (ROM pages to RAM), as upstream. It was off from boot log 9
// (2026-11-08) while hunting the startup hang, which turned out to be the
// 128 GB exFAT card (README-XPAN-M50.md). Back on 2026-11-09 for XPan's
// colour-table hook (include/platform/mmu_patches.h, README "17-point
// grid"). CONFIG_RPC and CONFIG_MMU come from internals.h.
#define CONFIG_SGI_HANDLERS
#define CONFIG_MMU_REMAP

//~kitor: add bootdisk menu for easier testing on foreign camera :)
#define FEATURE_BOOTFLAG_MENU

// We can't yet rely on image capture.  Cam crashes due to null pointer,
// I think?  If it fails to AF lock, for example.
#define CONFIG_IMAGE_CAPTURE_NOT_WORKING

// Off, as in bilalfakhouri's M50 branches (tested on his M50). Upstream turned
// both on for seven bodies at once ("rom backup: enable on more cams"); the
// first XPan boot on an M50 with them on hung with the card LED lit.
#undef CONFIG_CRASH_LOG
#undef CONFIG_AUTOBACKUP_ROM

#undef CONFIG_PROP_REQUEST_CHANGE

/* Keep ML after a Canon format (src/debug.c; README-XPAN-M50.md "Keep ML after
 * a Canon format"): camera-tested (f50ef0122795) with the M50 is_dir fix. */
#define CONFIG_RESTORE_AFTER_FORMAT
