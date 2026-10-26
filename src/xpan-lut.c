/* RGB16 tetrahedral sampler, shared by the native small-table Cube composer
 * and the older software experiment. This unit performs no RAW development. */
#include "xpan-lut.h"
#include <string.h>

int xpan_lut_init(struct xpan_lut *lut, const uint16_t *rgb,
                  unsigned size, size_t component_count)
{
    if (!lut) return 0;
    /* An invalid import cannot leave a previously valid table active by accident. */
    memset(lut, 0, sizeof(*lut));
    if (!rgb || size < 2 || size > 33 || component_count != (size_t)size * size * size * 3)
        return 0;
    lut->rgb = rgb;
    lut->size = size;
    return 1;
}

static void interpolate(const struct xpan_lut *lut, unsigned base,
                        uint32_t fr, uint32_t fg, uint32_t fb, uint16_t output[3])
{
    const unsigned rs = 3, gs = lut->size*3, bs = lut->size*lut->size*3;
    unsigned vertex[4] = {base, 0, 0, base+rs+gs+bs};
    uint32_t weight[4];
    /* Tetrahedral weights sum to 65536; all table samples and results retain 16 bits. */
    if (fr >= fg) {
        if (fg >= fb) {
            vertex[1]=base+rs; vertex[2]=vertex[1]+gs;
            weight[0]=65536-fr; weight[1]=fr-fg; weight[2]=fg-fb; weight[3]=fb;
        } else if (fr >= fb) {
            vertex[1]=base+rs; vertex[2]=vertex[1]+bs;
            weight[0]=65536-fr; weight[1]=fr-fb; weight[2]=fb-fg; weight[3]=fg;
        } else {
            vertex[1]=base+bs; vertex[2]=vertex[1]+rs;
            weight[0]=65536-fb; weight[1]=fb-fr; weight[2]=fr-fg; weight[3]=fg;
        }
    } else {
        if (fr >= fb) {
            vertex[1]=base+gs; vertex[2]=vertex[1]+rs;
            weight[0]=65536-fg; weight[1]=fg-fr; weight[2]=fr-fb; weight[3]=fb;
        } else if (fg >= fb) {
            vertex[1]=base+gs; vertex[2]=vertex[1]+bs;
            weight[0]=65536-fg; weight[1]=fg-fb; weight[2]=fb-fr; weight[3]=fr;
        } else {
            vertex[1]=base+bs; vertex[2]=vertex[1]+gs;
            weight[0]=65536-fb; weight[1]=fb-fg; weight[2]=fg-fr; weight[3]=fr;
        }
    }
    for (unsigned c=0; c<3; c++) {
        /* Max sum plus rounding is 65535*65536+32768, within uint32_t. */
        uint32_t value=32768;
        for (unsigned corner=0; corner<4; corner++)
            value += lut->rgb[vertex[corner]+c]*weight[corner];
        output[c]=value>>16;
    }
}

static void coordinate16(unsigned sample, unsigned size, unsigned *cell, uint32_t *fraction)
{
    unsigned scaled=sample*(size-1);
    /* Exact quotient/remainder by 65535 for the bounded 2..33 grid. Fold the
     * upper 16 bits into the lower word, then carry once. This removes divisions
     * from every RAW-derived RGB sample without reducing input precision. */
    unsigned quotient=scaled>>16, remainder=(scaled&65535)+quotient;
    if (remainder>=65535) { quotient++; remainder-=65535; }
    *cell=quotient;
    /* round(remainder*65536/65535) is remainder plus one above its midpoint. */
    *fraction=remainder+(remainder>=32768);
    if (*cell == size-1) {
        *cell=size-2;
        *fraction=65536;
    }
}

int xpan_lut_apply_rgb16(const struct xpan_lut *lut, const uint16_t *input,
                        uint16_t *output, size_t pixels, unsigned strength)
{
    if (!lut || !lut->rgb || lut->size<2 || lut->size>33 ||
        !input || !output || strength>65535 || pixels>(size_t)-1/6)
        return 0;
    if (!strength) {
        if (output!=input) memmove(output,input,pixels*6);
        return 1;
    }
    for (size_t pixel=0; pixel<pixels; pixel++) {
        unsigned r,g,b;
        uint32_t fr,fg,fb;
        uint16_t graded[3];
        coordinate16(input[0],lut->size,&r,&fr);
        coordinate16(input[1],lut->size,&g,&fg);
        coordinate16(input[2],lut->size,&b,&fb);
        interpolate(lut,((b*lut->size+g)*lut->size+r)*3,fr,fg,fb,graded);
        for (unsigned c=0; c<3; c++) {
            /* Blend before any final JPEG quantization; sum remains within uint32_t. */
            output[c]=strength==65535 ? graded[c] :
                (input[c]*(65535u-strength)+graded[c]*strength+32767)/65535;
        }
        input+=3; output+=3;
    }
    return 1;
}

int xpan_lut_quantize_for_jpeg(const uint16_t *input, uint8_t *output, size_t pixels)
{
    if (!input || !output || pixels>(size_t)-1/6) return 0;
    /* Keep reduction explicit at the final encoder boundary, never inside the RAW/LUT path. */
    for (size_t i=0; i<pixels*3; i++) output[i]=((uint32_t)input[i]+128)/257;
    return 1;
}
