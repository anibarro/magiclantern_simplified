#ifndef XPAN_MODE_H
#define XPAN_MODE_H
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
int xpan_photo_get_hud_y(void);
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
void xpan_native_look_note_jpeg(void);
/* Read-only UI snapshot; never opens files or rebuilds a table. */
void xpan_native_look_hud_label(char *name, unsigned name_size, char *state, unsigned state_size);
#endif
#endif
