/* GPL-2.0-or-later. EOS M full-sensor Live View composite. */
#ifndef LIVECOMP_FULLRES_LV_H
#define LIVECOMP_FULLRES_LV_H
struct card_info;
struct lc_frlv_options {
    unsigned fps_index;    /* lc_frr_timing entry = sub-exposure length */
    unsigned limit;        /* frames to merge before stopping; 0 = no limit */
};
/* Returns nonzero if DMA/hook ownership is uncertain; caller blocks retries. */
unsigned livecomp_frlv_run(struct card_info *card,const struct lc_frlv_options *options,
    const volatile unsigned *cancel,const char **status);
void livecomp_frlv_vsync(void);
#endif
