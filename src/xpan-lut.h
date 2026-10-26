#ifndef XPAN_LUT_H
#define XPAN_LUT_H
#include <stddef.h>
#include <stdint.h>

/* Unsigned RGB16 with red varying fastest. The caller defines colour encoding:
 * the software developer uses display sRGB; the native path uses its versioned
 * Canon Neutral base. The sampler adds no gamma/gamut conversion. Caller owns
 * table storage; initialization and row processing allocate no memory. */
struct xpan_lut {
    const uint16_t *rgb;
    unsigned size;
};
int xpan_lut_init(struct xpan_lut *lut, const uint16_t *rgb,
                  unsigned size, size_t component_count);
/* RAW development feeds RGB16 rows here: demosaic/WB/color space must precede this stage.
 * No 8-bit intermediate is used. In-place is supported; strength 0..65535. */
int xpan_lut_apply_rgb16(const struct xpan_lut *lut, const uint16_t *input,
                        uint16_t *output, size_t pixels, unsigned strength);
/* Final baseline-JPEG input conversion only, after RGB16 LUT processing is complete. */
int xpan_lut_quantize_for_jpeg(const uint16_t *input, uint8_t *output, size_t pixels);
#endif
