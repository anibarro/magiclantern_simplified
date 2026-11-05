/* GPL-2.0-or-later. EOS M full-sensor Live View composite. */
#ifndef LIVECOMP_FULLRES_LV_H
#define LIVECOMP_FULLRES_LV_H
struct card_info;
/* Session modes (menu "Mode"). */
#define LC_MODE_COMPOSITE 0u   /* per-pixel maximum: Olympus-style Live Composite */
#define LC_MODE_BULB      1u   /* per-pixel sum: one long bulb exposure, built up live */
/* Result file format (menu "File format", LCM67). */
#define LC_FORMAT_DNG     0u
#define LC_FORMAT_DNG_MLV 1u   /* both, the MLV written first */
#define LC_FORMAT_MLV     2u   /* one-frame MLV only (extra LVBR/LVDK files stay DNG) */
/* On-screen info while a session runs (menu "Show ...", LCM69). */
#define LC_INFO_BATTERY   1u
#define LC_INFO_TEMP      2u
#define LC_INFO_TIME      4u   /* running time hh:mm:ss */
#define LC_INFO_FRAMES    8u   /* Live Composite frame count */
struct lc_frlv_options {
    unsigned fps_index;    /* lc_frr_timing entry = sub-exposure length */
    unsigned limit;        /* frames to merge before stopping; 0 = no limit */
    unsigned preview_only; /* 1: one frame shown on the LCD until SET, nothing saved */
    unsigned debug;        /* 1: LOG/CSV next to the DNG; 0: no log files at all */
    unsigned mode;         /* LC_MODE_COMPOSITE or LC_MODE_BULB */
    unsigned dark_frame;   /* 1: dark frames at the end (lens covered): Bulb N, Composite 1 */
    unsigned save_orig;    /* 1: also save LVBR/LVCR (unprocessed) and LVDK/LVCK (dark) */
    unsigned format;       /* LC_FORMAT_DNG, LC_FORMAT_DNG_MLV or LC_FORMAT_MLV */
    unsigned base_first;   /* 1: Live Composite takes the base frame, then waits for SET (LCM69) */
    unsigned info;         /* LC_INFO_* bits shown under the session line */
    unsigned battery_guard;/* 1: stop and save at the last (red) battery level (LCM71) */
};
/* Returns nonzero if DMA/hook ownership is uncertain; caller blocks retries. */
unsigned livecomp_frlv_run(struct card_info *card,const struct lc_frlv_options *options,
    const volatile unsigned *cancel,const char **status);
void livecomp_frlv_vsync(void);
/* Diagnostics: record a key event seen while a session runs (LOG). */
void livecomp_frlv_key_log(unsigned code,unsigned phase,unsigned result);
#endif
