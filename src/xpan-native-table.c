/* Small native color-table transforms. Canon still evaluates the resulting
 * lookup for every image pixel during RAW development, before JPEG encoding. */
#include "xpan-native-table.h"

static unsigned table_index(unsigned r, unsigned g, unsigned b)
{
    return 5*(g%5+5*(b%5))+(b>4 ? 513:0)+(g>4 ? 257:0)+r+(r>4 ? 124:0);
}
static int signed_chroma(uint32_t value)
{
    value&=1023;
    return value&512 ? (int)value-1024:(int)value;
}

uint32_t xpan_native_table_validate(const uint32_t *table)
{
    uint32_t flags=0;
    for (unsigned r=0; r<10; r++) for (unsigned g=0; g<10; g++) for (unsigned b=0; b<10; b++) {
        uint32_t word=table[table_index(r,g,b)];
        unsigned y=(word>>20)&1023;
        int cr=signed_chroma(word), cb=signed_chroma(word>>10);
        if (word&0xc0000000u) flags|=XPAN_NATIVE_TABLE_TOP_BITS;
        if (y>512) flags|=XPAN_NATIVE_TABLE_Y_RANGE;
        if (cr<-256 || cr>256 || cb<-256 || cb>256) flags|=XPAN_NATIVE_TABLE_CHROMA_RANGE;
    }
    /* Every captured Neutral variant has a monotone, chroma-free gray axis with
     * black 0 and white 512. Do not assume its intermediate tone curve repeats. */
    unsigned previous=0;
    for (unsigned n=0; n<10; n++) {
        uint32_t word=table[table_index(n,n,n)];
        unsigned y=(word>>20)&1023;
        if (word&0xfffff) flags|=XPAN_NATIVE_TABLE_GRAY_CHROMA;
        if (y<previous) flags|=XPAN_NATIVE_TABLE_GRAY_ORDER;
        if ((!n && y) || (n>=8 && y!=512)) flags|=XPAN_NATIVE_TABLE_ENDPOINTS;
        previous=y;
    }
    uint32_t red=table[table_index(8,0,0)], green=table[table_index(0,8,0)], blue=table[table_index(0,0,8)];
    /* The observed red/green/blue primary signs distinguish the packed chroma
     * fields from an unrelated bounded buffer. Unknown domains remain blocked. */
    if (signed_chroma(red)<=0 || signed_chroma(red>>10)>=0 ||
        signed_chroma(green)>=0 || signed_chroma(green>>10)>=0 ||
        signed_chroma(blue)>=0 || signed_chroma(blue>>10)<=0) flags|=XPAN_NATIVE_TABLE_PRIMARIES;
    return flags;
}

int xpan_native_table_prepare(uint32_t *output, const uint32_t *source,
                              int look, uint32_t *format_flags)
{
    if (!output || !source || !format_flags || look<0 || look>2) return 0;
    *format_flags=xpan_native_table_validate(source);
    if (look && *format_flags) return 0;
    /* Copy all words, preserving Canon's 24 other slots for every recipe. */
    for (unsigned i=0; i<1024; i++) output[i]=source[i];
    if (!look) return 1;
    for (unsigned r=0; r<10; r++) for (unsigned g=0; g<10; g++) for (unsigned b=0; b<10; b++) {
        unsigned index=table_index(r,g,b);
        uint32_t word=source[index], y=(word>>20)&1023;
        int cr=signed_chroma(word), cb=signed_chroma(word>>10);
        if (look==2) {
            /* Matte monochrome acts on this shot's native tone curve, not on a
             * stale reference table: lift black, soften white, remove chroma. */
            y=16+((y*472+256)>>9); cr=cb=0;
        } else {
            /* Warm tint fades at black/white. Fixed-point shifts avoid divisions
             * and quantize only to the native table's existing packed format. */
            unsigned weight=y<512-y ? y:512-y;
            cr+=(int)((20*weight+128)>>8); cb-=(int)((12*weight+128)>>8);
            if (cr>511) cr=511;
            if (cb<-512) cb=-512;
        }
        output[index]=(y<<20)|(((uint32_t)cb&1023)<<10)|((uint32_t)cr&1023);
    }
    return 1;
}


static int round_signed_512(int value)
{
    return value>=0 ? (value+256)/512:-((-value+256)/512);
}
static unsigned divide_65535_rounded(unsigned value)
{
    /* Exact bounded division for values <=33553920, avoiding a division for
     * every component. Folding the high word leaves at most one carry. */
    value+=32767;
    unsigned quotient=value>>16,remainder=(value&65535)+quotient;
    return quotient+(remainder>=65535);
}
static int native_component(int numerator)
{
    return numerator>=0 ? (int)divide_65535_rounded((unsigned)numerator*2):
        -(int)divide_65535_rounded((unsigned)-numerator*2);
}

int xpan_native_table_prepare_cube(uint32_t *output,const uint32_t *source,
    const struct xpan_lut *cube,unsigned strength,int identity,const struct xpan_lut *base,
    struct xpan_native_cube_stats *stats,uint32_t *format_flags)
{
    if (!output || !source || !cube || !cube->rgb || !stats || !format_flags ||
        cube->size<2 || cube->size>33 || strength>65535) return 0;
    *stats=(struct xpan_native_cube_stats){0};
    *format_flags=xpan_native_table_validate(source);
    if (*format_flags) return 0;
    for (unsigned i=0; i<1024; i++) output[i]=source[i];
    /* A quantized identity Cube, or zero strength, preserves the actual Canon
     * table byte-for-byte. This is the reference for subsequent colour probes. */
    if ((identity || !strength) && !base) { stats->identity_bypass=1; return 1; }
    if (identity) strength=0; /* base only */
    for (unsigned r=0; r<10; r++) for (unsigned g=0; g<10; g++) for (unsigned b=0; b<10; b++) {
        unsigned index=table_index(r,g,b);
        uint32_t word=source[index];
        int y=(word>>20)&1023,cr=signed_chroma(word),cb=signed_chroma(word>>10);
        /* Inverse of [[77,150,29],[128,-107,-21],[-43,-85,128]]/256,
         * in Q16. The integer forward matrix and signed rounding reproduce
         * every ROM fallback node. Its physical colour space is still uncalibrated.
         * Source validation bounds each accumulator well within signed32. */
        int rgb_q16[3]={round_signed_512(65536*y+91715*cr+199*cb),
            round_signed_512(65536*y-47003*cr-22559*cb),
            round_signed_512(65536*y-402*cr+116158*cb)};
        uint16_t input[3],graded[3];
        for (unsigned c=0; c<3; c++) {
            int value=rgb_q16[c];
            if (value<0 || value>65536) {
                unsigned clipped=value<0 ? (unsigned)-value:(unsigned)(value-65536);
                stats->input_clipped_components++;
                if (clipped>stats->worst_input_clip_q16) stats->worst_input_clip_q16=clipped;
                value=value<0 ? 0:65536;
            }
            input[c]=((uint32_t)value*65535+32768u)>>16;
        }
        /* Headroom base: invert Canon's -2 EV render and encode (Cineon). */
        if (base && !xpan_lut_apply_rgb16(base,input,input,1,65535)) return 0;
        /* Only one native table node is sampled, never a sensor/image pixel. */
        if (!xpan_lut_apply_rgb16(cube,input,graded,1,strength)) return 0;
        int out_y=native_component(77*graded[0]+150*graded[1]+29*graded[2]);
        int out_cr=native_component(128*graded[0]-107*graded[1]-21*graded[2]);
        int out_cb=native_component(-43*graded[0]-85*graded[1]+128*graded[2]);
        if (out_y<0 || out_y>512 || out_cr < -256 || out_cr>256 || out_cb < -256 || out_cb>256) {
            /* Fail the whole redirection instead of wrapping overflow into
             * packed fields or silently changing an unverified output range. */
            stats->output_clipped_components++;
            *format_flags|=XPAN_NATIVE_TABLE_CUBE_OUTPUT; return 0;
        }
        output[index]=((uint32_t)out_y<<20)|(((uint32_t)out_cb&1023)<<10)|((uint32_t)out_cr&1023);
        stats->prepared_entries++;
    }
    return 1;
}

/* Returns 1 on success, or a negative code naming the failed check. */
int xpan_native_obwb_headroom(uint32_t *output,uint32_t source,unsigned stops)
{
    static const unsigned gains[4]={0x54,0x64,0x74,0x84}; /* G1,R,B,G2 */
    uint32_t physical=source&~0x40000000u;
    if (!output || !stops || stops>2 || (source&3) || physical<0x10000 ||
        physical>0x10000000-XPAN_OBWB_BYTES) return -1;
    const volatile uint32_t *in=(const volatile uint32_t *)source;
    for (unsigned i=0; i<XPAN_OBWB_BYTES/4; i++) output[i]=in[i];
    for (unsigned i=0; i<XPAN_OBWB_BYTES/4; i++) if (output[i]!=in[i]) return -2;
    uint16_t *h=(uint16_t *)output;
    /* White level 15675 identifies the layout decoded in Test58/68. */
    if (h[0xf4/2]<8000 || h[0xf4/2]>16383) return -3;
    for (unsigned k=0; k<4; k++) {
        unsigned word=h[gains[k]/2];
        if (word&0xc000) return -4;
        unsigned value=(word&0xfff)<<((word>>12)&3);
        if (value<256 || value>8192) return -5;
        /* Linear pre-gain: Test68 shows an exact EV shift of the whole curve. */
        value=(value+(1u<<(stops-1)))>>stops;
        unsigned shift=0;
        while (value>0x7ff) { value>>=1; shift++; }
        h[gains[k]/2]=(uint16_t)((shift<<12)|value);
    }
    return 1;
}

/* Shadow room for the 10-step hardware table (v6). Angel +0x80..+0xB7 is the
 * Neutral luma row, applied after Canon's gamma and before the 3D table. The
 * HTP row is Canon's own monotonic curve = Neutral +1 EV with a highlight
 * shoulder (Test64/65), so deep shadows reach higher table inputs. Only the
 * exact Neutral contrast-0 layout (+0x40..+0xB7, Test64 baseline) is changed. */
static const uint16_t angel_neutral_40_b8[60]={
 0x0f80,0x01bb,0x0045,0x0380,0x0066,0x0245,0x0159,0x0000,0x013c,0x01b5,0x0059,0x223a,0x01b2,0x009c,0x323a,0x01b9,
 0x00cc,0x30c8,0x01ea,0x00ec,0x03e9,0x308f,0x01f5,0x00f3,0x0592,0x3045,0x01fc,0x00fe,0x06a2,0x302c,0x01fd,0x0102,
 0x0000,0x0086,0x0253,0x0760,0x0000,0x11ef,0x0783,0x0070,0x10fe,0x07e0,0x009c,0x31fe,0x07ce,0x00c8,0x310e,0x07e2,
 0x00e8,0x03de,0x30a9,0x07eb,0x00f1,0x050d,0x304d,0x07f9,0x00fa,0x0410,0x0410,0x0710};
/* Canon HTP set (Test64 IMG_9294): chroma-vs-level +0x40..+0x7F and luma row
 * +0x80..+0xB7; Canon lowers highlight chroma with the +1 EV luma so red
 * cannot clip before the table (v6 used the luma row alone and clipped). */
static const uint16_t angel_htp_40_b8[60]={
 0x8f80,0x0118,0x10dc,0x034c,0x0033,0x0488,0x215a,0x0000,0x0270,0x1170,0x0059,0x011d,0x01b1,0x009c,0x1119,0x01bb,
 0x00cc,0x3165,0x01ce,0x00ec,0x027f,0x30bd,0x01e4,0x00fb,0x0372,0x3055,0x01f7,0x0103,0x04b1,0x302c,0x01fd,0x0108,
 0x0001,0x003e,0x04c3,0x053b,0x0000,0x0224,0x06d4,0x006a,0x0103,0x07b0,0x009c,0x21af,0x07a4,0x00c6,0x3186,0x07c5,
 0x00e0,0x02ba,0x3084,0x07f3,0x00f2,0x0452,0x3049,0x07fd,0x00fb,0x0410,0x0410,0x0608};

volatile int xpan_native_angel_diff=-1;

int xpan_native_angel_shadow(uint32_t *output,uint32_t source)
{
    uint32_t physical=source&~0x40000000u;
    if (!output || (source&3) || physical<0x10000 || physical>0x10000000-XPAN_ANGEL_BYTES) return -1;
    const volatile uint32_t *in=(const volatile uint32_t *)source;
    for (unsigned i=0; i<XPAN_ANGEL_BYTES/4; i++) output[i]=in[i];
    for (unsigned i=0; i<XPAN_ANGEL_BYTES/4; i++) if (output[i]!=in[i]) return -2;
    uint16_t *h=(uint16_t *)output;
    /* v10: the calibrated tone set is imposed on every shot. Canon writes a
     * different luma row at ISO 100 (v9 camera test: row check -3 at ISO 100,
     * applied at ISO 400), so the row is no longer a precondition. The buffer
     * is Canon's own Angel block (front +0x2C); replacing its tone rows with
     * the exact Test64 HTP set makes the render identical at every ISO. The
     * count of differing luma words is kept only as a diagnostic. */
    unsigned diff=0;
    for (unsigned i=32; i<60; i++) diff+=h[0x40/2+i]!=angel_neutral_40_b8[i];
    xpan_native_angel_diff=diff;
    for (unsigned i=0; i<60; i++) h[0x40/2+i]=angel_htp_40_b8[i];
    h[0xda/2]=0x0003; h[0xde/2]=0x4b87; /* HTP gamma-block pair (Test64) */
    return 1;
}
