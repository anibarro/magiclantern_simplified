/* GPL-2.0-or-later. EOS M full-sensor Live View readout gate (LCM15). */
#ifndef LIVECOMP_FULLRES_LV_H
#define LIVECOMP_FULLRES_LV_H
struct card_info;
struct lc_frlv_options {
    unsigned fps_index,expo_index,inputs;
    unsigned continuous;   /* 1: full-res composite (arm every frame, merge, preview) */
    unsigned limit;        /* continuous: frames to merge before stopping */
};
/* Returns nonzero if DMA/hook ownership is uncertain; caller blocks retries. */
unsigned livecomp_frlv_run(struct card_info *card,const struct lc_frlv_options *options,
    const volatile unsigned *cancel,const char **status);
void livecomp_frlv_vsync(void);
#endif
