#ifndef XPAN_LIVE_NATIVE_H
#define XPAN_LIVE_NATIVE_H
#include "xpan-lut.h"
/* Hardware LCD preview; Canon remains the transfer owner. */
void xpan_live_native_init(unsigned observer_hooks);
void xpan_live_native_pause(void);
const char *xpan_live_native_status(void);
/* base: optional LCD tone base composed under the Cube (Cineon preview). */
int xpan_live_native_prepare(const struct xpan_lut *cube,unsigned revision,
    unsigned strength,int identity,const struct xpan_lut *base,volatile int *cancel);
void xpan_live_native_transfer(uint32_t *regs,uint32_t *stack,uint32_t pc);
int xpan_live_native_log(char *text,unsigned size);
/* Compact transition revision; the existing idle worker owns SD writes. */
unsigned xpan_live_native_log_revision(void);
int xpan_native_look_live_requested(void);
int xpan_native_look_movie_requested(void);
int xpan_movie_lut_wanted(void);
/* Movie LUT diagnostics for the menu: seen transfers and redirections. */
void xpan_live_native_movie_diag(char *text,unsigned size);
#endif
