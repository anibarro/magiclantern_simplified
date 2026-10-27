/* GPL-2.0-or-later. EOS M full-sensor Live View composite. */
#ifndef LIVECOMP_FULLRES_LV_H
#define LIVECOMP_FULLRES_LV_H
struct card_info;
struct lc_frlv_options {
    unsigned fps_index;    /* lc_frr_timing entry = sub-exposure length */
    unsigned limit;        /* frames to merge before stopping; 0 = no limit */
    /* Exposure preview before the composite (NULL go: compose at once).
     * The worker sets *phase; the key handler sets *go (SET) to start the
     * composite, or the cancel flag (half-shutter) to leave the preview. */
    volatile unsigned *phase;
    const volatile unsigned *go;
};
enum { LC_FRLV_SETUP=0,LC_FRLV_PREVIEW=1,LC_FRLV_COMPOSE=2 };
/* Returns nonzero if DMA/hook ownership is uncertain; caller blocks retries. */
unsigned livecomp_frlv_run(struct card_info *card,const struct lc_frlv_options *options,
    const volatile unsigned *cancel,const char **status);
void livecomp_frlv_vsync(void);
/* Diagnostics: record a key event seen while a session runs (LOG). */
void livecomp_frlv_key_log(unsigned code,unsigned phase,unsigned result);
#endif
