#ifndef XPAN_NATIVE_TABLE_H
#define XPAN_NATIVE_TABLE_H
#include <stdint.h>
#include "xpan-lut.h"

/* Observed EOS M Neutral hardware domain. These are table fields, not an RGB16
 * pixel-buffer ABI. Flags explain why an unknown table is left unchanged. */
enum xpan_native_table_error {
    XPAN_NATIVE_TABLE_TOP_BITS=1,
    XPAN_NATIVE_TABLE_Y_RANGE=2,
    XPAN_NATIVE_TABLE_CHROMA_RANGE=4,
    XPAN_NATIVE_TABLE_GRAY_CHROMA=8,
    XPAN_NATIVE_TABLE_GRAY_ORDER=16,
    XPAN_NATIVE_TABLE_ENDPOINTS=32,
    XPAN_NATIVE_TABLE_PRIMARIES=64,
    XPAN_NATIVE_TABLE_NOT_CHECKED=128,
    XPAN_NATIVE_TABLE_CUBE_OUTPUT=256
};
uint32_t xpan_native_table_validate(const uint32_t *table);
/* Look 0 copies any source exactly; 1/2 grade only validated native tables.
 * Both arrays have 1024 words. No allocation, image access, I/O or floating point. */
int xpan_native_table_prepare(uint32_t *output, const uint32_t *source,
                              int look, uint32_t *format_flags);
/* Cube composition changes only the 1000 nodes, using the provisional EOS M
 * fallback RGB matrix. It does not establish an sRGB/gamma calibration. */
struct xpan_native_cube_stats {
    uint32_t input_clipped_components,worst_input_clip_q16,output_clipped_components;
    uint32_t prepared_entries,identity_bypass;
};
/* base: optional tone base Cube composed before the creative Cube (even for
 * an identity Cube); the caller lowers JPEG_OBWB to match the base. */
int xpan_native_table_prepare_cube(uint32_t *output,const uint32_t *source,
    const struct xpan_lut *cube,unsigned strength,int identity,const struct xpan_lut *base,
    struct xpan_native_cube_stats *stats,uint32_t *format_flags);
/* Private JPEG_OBWB copy (392 bytes) with the four CFA WB gains lowered by
 * 'stops'. Returns 1, or <=0 (output unused) for an unexpected layout/value. */
#define XPAN_OBWB_BYTES 392
int xpan_native_obwb_headroom(uint32_t *output,uint32_t source,unsigned stops);
/* Private Angel copy (2016 bytes) with Canon's HTP luma row in place of the
 * Neutral row, for more shadow range in the table. 1 ok, <=0 rejected. */
#define XPAN_ANGEL_BYTES 2016
int xpan_native_angel_shadow(uint32_t *output,uint32_t source);
extern volatile int xpan_native_angel_diff;
#endif
