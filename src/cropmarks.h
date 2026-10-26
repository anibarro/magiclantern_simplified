#ifndef _cropmarks_h_
#define _cropmarks_h_
/**
 * @brief Set custom movie cropmarks in LiveView
 *
 * @param x X coordinate
 * @param y Y coordinate
 * @param w Width
 * @param h Height
 * @addtogroup cropmarks
 */
void set_movie_cropmarks(int x, int y, int w, int h);
/**
 * @brief Reset LiveView movie cropmarks
 */
void reset_movie_cropmarks();

#ifdef CONFIG_EOSM
/* XPan overlay state and separately configured photo-capture crop helpers. */
enum xpan_photo_aspect { XPAN_ASPECT_65_24 = 0, XPAN_ASPECT_3_2 = 1 };
int xpan_photo_aspect_ratio(void);
int xpan_photo_is_panoramic(void);
/* Compute 65:24 rectangle geometry; this helper does not modify pixels or image metadata. */
int xpan_photo_get_65_24_rect(int width, int height, int *x, int *y, int *crop_width, int *crop_height);
/* Centered full-width preview source rectangle; height approximates 65:24 to even rows. */
int xpan_photo_get_preview_rect(int width, int height, int *x, int *y, int *crop_width, int *crop_height);
/* Return the overlay/frame rectangle for drawing and HUD layout; never crops image pixels. */
int xpan_photo_get_liveview_frame_rect(int *x, int *y, int *crop_width, int *crop_height);
/* Return the top/bottom position for the transformed LCD frame, independent of overlay geometry. */
int xpan_photo_get_preview_frame_y(int width, int height);
/* Return an inset image area for monitoring overlays; never uses the HUD matte. */
int xpan_photo_get_monitor_rect(int *x, int *y, int *width, int *height);
int xpan_photo_ui_active(void);
/* Whether the custom XPan bitmap HUD should be drawn over the image. */
int xpan_photo_hud_is_enabled(void);
/* Enable the separate LCD preview transform; does not affect the guide or saved-image data. */
int xpan_photo_preview_crop_enabled(void);
/* Return the last LCD preview transform failure code, or zero after a successful frame. */
int xpan_photo_preview_failure_code(void);
/* True after the display-filter task has built a complete centered LCD frame. */
int xpan_photo_preview_frame_active(void);
/* Last measured LCD publication rate in tenths of a frame per second. */
unsigned xpan_photo_preview_fps_x10(void);
/* Raw EDMAC cursor seen when EOS M rejects a source address, or zero if matched. */
unsigned xpan_photo_preview_unmatched_cursor(void);
void xpan_photo_keep_3_2_aspect(void);
int xpan_photo_dng_copy_enabled(void);
int xpan_photo_cr2_crop_enabled(void);
/* Show the last DNG worker state directly in the XPan menu. */
enum xpan_dng_copy_status
{
    XPAN_DNG_COPY_READY = 0,
    XPAN_DNG_COPY_WAIT_REVIEW,
    XPAN_DNG_COPY_WAIT_RAW,
    XPAN_DNG_COPY_SAVED,
    XPAN_DNG_COPY_FAILED,
    XPAN_DNG_COPY_NO_RAW,
    XPAN_DNG_COPY_UNKNOWN_FORMAT,
    XPAN_DNG_COPY_FAILED_GEOMETRY,
    XPAN_DNG_COPY_FAILED_CARD_SPACE,
    XPAN_DNG_COPY_FAILED_MEMORY,
    XPAN_DNG_COPY_FAILED_BUFFER,
    XPAN_DNG_COPY_FAILED_WRITER,
};
void xpan_photo_set_dng_copy_status(enum xpan_dng_copy_status status);
#endif

void crop_set_dirty(int redraw_timeout);

#endif
