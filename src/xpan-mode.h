#ifndef XPAN_MODE_H
#define XPAN_MODE_H
/* Body geometry. Full-resolution Canon image (CR2 IFD0 / Large JPEG), the
 * sensor width in tenths of a millimetre (XPan equivalent lens), and Canon's
 * own 16:9, 4:3 and 1:1 MakerNote crop records at full resolution. */
#if defined(CONFIG_6D)
#define XPAN_SENSOR_WIDTH 5472
#define XPAN_SENSOR_HEIGHT 3648
#define XPAN_SENSOR_WIDTH_MM10 358
#define XPAN_ASPECT_16_9_HEIGHT 3078
#define XPAN_ASPECT_16_9_TOP 285
#define XPAN_ASPECT_16_9_NATIVE_HEIGHT 3072  /* 16:9 JPEG raster */
#define XPAN_ASPECT_4_3_WIDTH 4864
#define XPAN_ASPECT_4_3_LEFT 304
#define XPAN_ASPECT_1_1_LEFT 912
#elif defined(CONFIG_M50)
/* EOS M50: 6000x4000 L image, 22.3 mm wide APS-C sensor. Canon's 16:9 L JPEG
 * is 6000x3368, 4:3 is 5328x4000, 1:1 is 4000x4000. The crop records are
 * derived from those sizes like the EOS M's, not yet read from an M50 CR3
 * (the record writer only runs in QEMU builds). */
#define XPAN_SENSOR_WIDTH 6000
#define XPAN_SENSOR_HEIGHT 4000
#define XPAN_SENSOR_WIDTH_MM10 223
#define XPAN_ASPECT_16_9_HEIGHT 3376
#define XPAN_ASPECT_16_9_TOP 312
#define XPAN_ASPECT_16_9_NATIVE_HEIGHT 3368
#define XPAN_ASPECT_4_3_WIDTH 5328
#define XPAN_ASPECT_4_3_LEFT 336
#define XPAN_ASPECT_1_1_LEFT 1000
#else /* EOS M */
#define XPAN_SENSOR_WIDTH 5184
#define XPAN_SENSOR_HEIGHT 3456
#define XPAN_SENSOR_WIDTH_MM10 223
#define XPAN_ASPECT_16_9_HEIGHT 2916
#define XPAN_ASPECT_16_9_TOP 270
#define XPAN_ASPECT_16_9_NATIVE_HEIGHT 2912
#define XPAN_ASPECT_4_3_WIDTH 4608
#define XPAN_ASPECT_4_3_LEFT 288
#define XPAN_ASPECT_1_1_LEFT 864
#endif
/* The saved mode switch gates framing, metadata and automatic photo hooks. */
int xpan_photo_enabled(void);
/* User-facing option controls only the bitmap HUD, not framing or capture. */
int xpan_photo_hud_is_enabled(void);
/* The ratio is independent of the XPan master switch and remains persistent. */
int xpan_photo_aspect_ratio(void);
int xpan_photo_is_panoramic(void);
int xpan_photo_debug_enabled(void);   /* XPan > Debug info */
/* Readback must confirm a supported format before automatic capture is ready. */
int xpan_photo_ensure_capture_quality(void);
/* The 214-row matte belongs only to the panoramic HUD; 3:2 uses compact overlays. */
#define XPAN_PHOTO_HUD_HEIGHT 214
/* Frame alignment Split (65:24 only): the panel's first row (logo, lens,
 * mode, clock, card, battery) becomes a bar of this height above the
 * picture; the rest of the panel stays below it. */
#define XPAN_PHOTO_SPLIT_BAR 60
/* Rows 0..213 of the panel; in Split, rows 0..59 are drawn at the top-bar y. */
int xpan_photo_get_hud_y(void);
int xpan_photo_get_hud_top_y(void);
/* Idle log snapshot for diagnosing a missing photo HUD without menu input. */
void xpan_photo_hud_debug_snapshot(unsigned *draw_calls, int *last_y, unsigned *clipped);
/* Native Canon selection flags, observed without intercepting wheel events. */
enum xpan_wheel_target {
    XPAN_WHEEL_ISO=1, XPAN_WHEEL_APERTURE=2,
    XPAN_WHEEL_SHUTTER=4, XPAN_WHEEL_COMPENSATION=8
};
unsigned xpan_photo_wheel_targets(void);
unsigned xpan_photo_wheel_state(void);
#ifdef CONFIG_XPAN
void xpan_native_look_add_menu(void);
const char *xpan_native_look_status(void);
int xpan_native_look_adjusting(void);
void xpan_native_look_mode_changed(void);
/* Called by the existing LCD task; creates at most one checked worker. */
void xpan_native_look_start_worker(void);
/* Read-only snapshot from the existing JPEG setup callback; no SD I/O. */
void xpan_native_look_note_jpeg(uint32_t jpeg_job);
/* Read-only UI snapshot; never opens files or rebuilds a table. */
void xpan_native_look_hud_label(char *name, unsigned name_size, char *state, unsigned state_size);
/* Name (without extension) of the LUT Canon applied to the photo being
 * saved; 0 when no LUT was applied. Safe in the JPEG capture hook. */
int xpan_native_look_photo_lut(char *name, unsigned size);
#endif
#endif
