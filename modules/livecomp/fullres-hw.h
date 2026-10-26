/* GPL-2.0-or-later. Buffer layout for the full-res composite and the Canon
 * max-engine planes built from it (LCM22). Pure arithmetic: host-tested.
 * LCM21 lesson: the engine wrapper requires each band to start a whole
 * number of rows after the plane storage, so storage is set 4 rows (the
 * halo) before the pixels, not at the allocation start. */
#ifndef LIVECOMP_FULLRES_HW_H
#define LIVECOMP_FULLRES_HW_H
#include <stdint.h>
#include "native-max.h"
#define LC_FRLV_ALLOCATION 32488032u        /* size proven on camera (LCM9-13, 15-21) */
#define LC_FRLV_GUARD 64u
#define LC_FRLV_HALO (4u*9240u)
/* uncached allocation -> uncached pixel start (cap keeps a guard before it) */
static inline uint32_t lc_frlv_pixels(uint32_t alloc,int capture)
{
    return (alloc+LC_FRLV_HALO+(capture ? 2:1)*LC_FRLV_GUARD-1)&~(LC_FRLV_GUARD-1);
}
static inline void lc_frlv_plane(uint32_t alloc,uint32_t pixels,unsigned pitch,
    unsigned first_row,unsigned rows,struct lc_native_plane *p)
{
    uint32_t storage=pixels-LC_FRLV_HALO;
    p->storage=storage|0x40000000u;
    p->bytes=alloc+LC_FRLV_ALLOCATION-storage;
    p->pixels=(pixels+first_row*pitch)|0x40000000u;
    p->width=5280;p->height=rows;p->pitch=pitch;
}
/* Two engine bands: even row counts <= 1930 (1754 + 1752 for 3506 rows). */
static inline unsigned lc_frlv_band(unsigned height,unsigned band,unsigned *first)
{
    unsigned b0=(height/2+1)&~1u;
    *first=band ? b0:0;
    return band ? height-b0:b0;
}
#endif
