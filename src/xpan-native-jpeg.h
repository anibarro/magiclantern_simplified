#ifndef XPAN_NATIVE_JPEG_H
#define XPAN_NATIVE_JPEG_H
/* Optional, manually scoped observation of Canon's normal JPEG crop setup. */
struct card_info;
/* Modes: 0 observe, 1 native 16:9, 2 5184x1920 at top768 (the XPan photo
 * mode), 3 retired 1914-row experiment. Mode 3 kept the 1920-row processing grid
 * but Canon split the job at 1914/2, repeating rows 957-959 at output row 960. */
int xpan_native_jpeg_start(int crop_mode);
/* Persistent mode keeps guarded crop hooks installed; no per-shot observers. */
void xpan_native_jpeg_set_automatic(void);
int xpan_native_jpeg_stop(int idle);
int xpan_native_jpeg_save(struct card_info *card, unsigned number);
int xpan_native_jpeg_hooks_remain(void);
/* Keep the actual setup failure visible; reboot is useful only for retained hooks. */
const char *xpan_native_jpeg_status(void);
unsigned xpan_native_jpeg_crop_count(void);
#ifdef CONFIG_XPAN
/* Manual playback compatibility is separate from per-shot capture hooks. */
int xpan_native_jpeg_playback_enable(void);
const char *xpan_native_jpeg_playback_status(void);
int xpan_native_jpeg_playback_save(struct card_info *card);
int xpan_native_jpeg_log_number_available(struct card_info *card, unsigned number);
#endif
#endif
