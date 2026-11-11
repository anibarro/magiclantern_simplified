/**
 * Camera internals for EOS M50 1.1.0
 */

/** This camera has a DIGIC VIII chip */
#define CONFIG_DIGIC_VIII

// has inter-core RPC (so far this has always been dependent on SGI, 0xc)
#define CONFIG_RPC

// Cam has MMU (by itself, does nothing, see CONFIG_MMU_REMAP)
#define CONFIG_MMU

// Four remapped 64 kB ROM pages: XPan's hooks in E00E0000, Canon's creative
// filter grain driver in E0860000, the Live View filter map in E0770000 and
// the JPEG encoder size table in E0FC0000 (runtime patch, README-XPAN-M50.md
// "65:24 JPEG"), mapped with 4 kB small pages from the heap.
#define CONFIG_MMU_REMAP_PAGES 7   /* hooks E00E, E086, E077, E013, E042, E08D + the 65:24 encoder table E0FC (runtime patch) */
#define CONFIG_MMU_REMAP_SMALL_PAGES

/** Digic 8 does not have bitmap font in ROM, try to load it from card **/
#define CONFIG_NO_BFNT

/** disable SRM for now
 * in current state SRM_AllocateMemoryResourceFor1stJob makes camera crash
 * even if just one buffer is requrested.
 */
#define CONFIG_MEMORY_SRM_NOT_WORKING

/* has LV */
#define CONFIG_LIVEVIEW

/* hooking EFV_STATE ends with EvfCap crashes, requires investigation */
//#define CONFIG_STATE_OBJECT_HOOKS

#define CONFIG_MALLOC_STRUCT_V2

#define CONFIG_TASK_STRUCT_V2_SMP
#define CONFIG_TASK_ATTR_STRUCT_V5

/** Workaround for menu timeout in LiveView (README-XPAN-M50.md "ML menu
 * timeout"): Canon drops the GUI mode ML's menu sits on (0x63) after 6 s
 * (camera test 2026-11-11: 99 -> 0 at +6.0 s each time, display on), and ML
 * closes its menu; menu.c re-requests it every 2 s, as on the EOS M and 6D. */
#define CONFIG_MENU_TIMEOUT_FIX
