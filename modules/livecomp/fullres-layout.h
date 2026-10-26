/* GPL-2.0-or-later. Synthetic full-width benchmark, NOT measured sensor geometry.
 * The pinned crop_rec mode claims 5208x3478 active pixels. Budget the surrounding
 * RAW raster separately; verify its actual geometry before enabling readout.
 * Two input slots + in-place accumulator + core fallback = four large buffers.
 * An LCD snapshot must be reduced, never a fifth full-sized buffer. */
#ifndef LIVECOMP_FULLRES_LAYOUT_H
#define LIVECOMP_FULLRES_LAYOUT_H
#include <stdint.h>
#define LC_FR_WIDTH 5280u
#define LC_FR_HEIGHT 3508u
#define LC_FR_PITCH (LC_FR_WIDTH*14u/8u)
#define LC_FR_HALO_ROWS 4u
#define LC_FR_GUARD 64u
#define LC_FR_BYTES (LC_FR_PITCH*LC_FR_HEIGHT)
#define LC_FR_STORAGE (LC_FR_PITCH*(LC_FR_HEIGHT+2*LC_FR_HALO_ROWS))
#define LC_FR_ALLOCATION (LC_FR_STORAGE+3*LC_FR_GUARD)
#define LC_FR_BUFFERS 4u
#define LC_FR_BAND_ROWS 1754u
#define LC_FR_CASES 6u
static const unsigned lc_fr_case_rows[LC_FR_CASES]={16,16,64,256,1754,1754};
/* Compare 16-row layouts before increasing the transfer size. */
static inline int lc_fr_case_inplace(unsigned test) { return test==1 || test==5; }
static inline unsigned lc_fr_output_row(unsigned test)
{ return !lc_fr_case_inplace(test) ? lc_fr_case_rows[test]+8:LC_FR_HALO_ROWS; }

/* Deterministic, independent inputs exercise every RAW14 value, both maximum
 * branches and row/word boundaries. No captured or Canon-owned data is used. */
static inline uint16_t lc_fr_pattern(uint32_t sample,unsigned input)
{
    return (uint16_t)((sample*(input ? 109u:37u)+(input ? 173u:13u))&16383u);
}
static inline uint32_t lc_fr_pixel_offset(unsigned row)
{
    return (LC_FR_HALO_ROWS+row)*LC_FR_PITCH;
}
#endif
