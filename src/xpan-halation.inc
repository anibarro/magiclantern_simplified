/* XPan halation (README-XPAN-M50.md "Halation"), pure C: no camera calls,
 * so build_tools/xpan_halation_test.c runs exactly this code on the host
 * against build_tools/xpan_halation_proto.py.
 *
 * Film: light from a strong highlight crosses the emulsion, reflects off the
 * base and exposes the red-sensitive layer again from behind: an orange-red
 * rim around lamps, windows and bright lines, the highlight itself white.
 *
 * Image: Canon's developed YCbCr 4:2:2 after the LUT (words U Y0 V Y1, U and
 * V signed, JPEG full range), the same rows XPan's Film grain works on.
 *   1. hal_hist_row: histogram of every 8th row -> the image's white
 *      (99.99th percentile, at least HAL_WHITE_MIN: XPan Cineon's white is
 *      about Y 222, Canon's 255).
 *   2. hal_set_white: key tables, smoothstep from HAL_THR levels under the
 *      white to the white, squared.
 *   3. hal_acc_row: sum of key(Y) per 16 x 16 cell.
 *   4. hal_blur: cell means -> the inner map (1/16) and the tail map (1/32,
 *      2 x 2 means), Gaussian blurs (sigma 0.25 % and 1.2 % of the width),
 *      stored gamma-coded in 8 bits (fine steps near zero) in place.
 *   5. hal_row: per word, the glow from both maps (bilinear), minus the
 *      pixel's own key (only outside the highlight), screen blend in linear
 *      light with the tints; words with no glow are copied unchanged.
 * Integers only per pixel; expf/log2f only for the tables and kernels. */

#define HAL_CELL        16u
#define HAL_MAX_W       6016u
#define HAL_MAX_ROWS    4000u
#define HAL_MW          (HAL_MAX_W/HAL_CELL)                  /* 376 */
#define HAL_MH          ((HAL_MAX_ROWS+HAL_CELL-1)/HAL_CELL)  /* 250 */
#define HAL_STRIDE      ((HAL_MW+15u)&~15u)                   /* 384 cells: rows start on 32-byte lines */
#define HAL_TW          ((HAL_MW+1u)/2u)
#define HAL_TH          ((HAL_MH+1u)/2u)
#define HAL_TSTRIDE     ((HAL_TW+15u)&~15u)                   /* 192 */
#define HAL_THR         30
#define HAL_WHITE_MIN   210
#define HAL_KMAX        12                                    /* kernel radius cap */
#define HAL_TMP         (HAL_MW+2u*HAL_KMAX+8u)

struct hal_tables {
    uint16_t lin[256];          /* display -> linear 0..65535 (gamma 2.2) */
    uint16_t dec[4096];         /* gamma-coded glow (8.4 bits) -> linear 0..65535 */
    uint8_t inv[4096];          /* linear >> 4 -> display 0..255 */
    uint8_t key8[256];          /* key 0..255 (map sums) */
    uint16_t key16[256];        /* key 0..65535 (pixel) */
    uint32_t hist[2][256];      /* per worker */
    int32_t tmp[HAL_TMP];       /* blur line */
};
struct hal_job {
    unsigned width, rows, mw, mh, tw, th, white;
    uint16_t *acc;              /* HAL_STRIDE x HAL_MH: key sums -> inner glow -> coded bytes */
    uint16_t *tail;             /* HAL_TSTRIDE x HAL_TH: tail glow -> coded bytes */
    struct hal_tables *t;
    int ai[3], at[3];           /* tint x share x strength, 1/4096 */
};

static float hal_pow(float x, float a) { return x<=0 ? 0 : expf(a*0.69314718f*log2f(x)); }

/* Once (XPan's task). */
static void hal_tables_init(struct hal_tables *t)
{
    for (int i=0;i<256;i++) t->lin[i]=(uint16_t)(65535.0f*hal_pow(i/255.0f,2.2f)+0.5f);
    for (int i=0;i<4096;i++) {
        float d=65535.0f*hal_pow(MIN(i,4080)/4080.0f,2.2f)+0.5f;
        t->dec[i]=(uint16_t)MIN(d,65535.0f);
        t->inv[i]=(uint8_t)(255.0f*hal_pow(i/4095.0f,1/2.2f)+0.5f);
    }
}
/* Strength 0..100 % (XPan's task). Tints in linear light: inner orange-red
 * (1, 0.30, 0.10) 65 %, tail deeper red (1, 0.12, 0.03) 35 % (prototype).
 * 70 % = the prototype's strength 1.0 (user, 2026-11-13: stronger; 100 %
 * = 1.43). */
static void hal_set_strength(struct hal_job *j, int pct)
{
    static const int in[3]={1000,300,100}, out[3]={1000,120,30};
    for (int c=0;c<3;c++) {
        j->ai[c]=4096*pct*65/70*in[c]/100000;      /* 100 %: 3803, 1141, 380 */
        j->at[c]=4096*pct*35/70*out[c]/100000;     /* 100 %: 2048, 245, 61 */
    }
}

static void hal_job_size(struct hal_job *j, unsigned width, unsigned rows)
{
    j->width=width; j->rows=rows;
    j->mw=width/HAL_CELL; j->mh=(rows+HAL_CELL-1)/HAL_CELL;
    j->tw=(j->mw+1)/2; j->th=(j->mh+1)/2;
}
static int hal_fits(unsigned width, unsigned rows)
{
    return width>=64 && width<=HAL_MAX_W && !(width%HAL_CELL) && rows>=HAL_CELL && rows<=HAL_MAX_ROWS;
}

/* Phase 1: one row of the histogram (rows with (y & 7) == 0). */
static void hal_hist_row(uint32_t *hist, const uint32_t *src, unsigned width)
{
    for (unsigned i=0;i<width/2;i++) { uint32_t w=src[i]; hist[(w>>8)&255]++; hist[w>>24]++; }
}
/* Phase 2: the white and the key tables. */
static void hal_set_white(struct hal_job *j)
{
    struct hal_tables *t=j->t;
    uint32_t total=0;
    for (int i=0;i<256;i++) total+=t->hist[0][i]+t->hist[1][i];
    uint32_t above=0, lim=total/10000u;
    int white=255;
    for (;white>0;white--) { above+=t->hist[0][white]+t->hist[1][white]; if (above>lim) break; }
    if (white<HAL_WHITE_MIN) white=HAL_WHITE_MIN;
    j->white=(unsigned)white;
    for (int y=0;y<256;y++) {
        float u=(float)(y-(white-HAL_THR))/HAL_THR;
        u=u<0 ? 0 : u>1 ? 1 : u;
        float s=u*u*(3-2*u); s*=s;
        t->key8[y]=(uint8_t)(255.0f*s+0.5f);
        t->key16[y]=(uint16_t)(t->key8[y]*257u);
    }
}
/* Phase 3: add one row's keys to its cell row (cells of 8 words). */
static void hal_acc_row(const struct hal_job *j, const uint32_t *src, unsigned y)
{
    const uint8_t *k=j->t->key8;
    uint16_t *a=j->acc+(y/HAL_CELL)*HAL_STRIDE;
    for (unsigned cx=0;cx<j->mw;cx++) {
        const uint32_t *s=src+cx*8u;
        unsigned sum=0;
        for (unsigned i=0;i<8;i++) { uint32_t w=s[i]; sum+=k[(w>>8)&255]+k[w>>24]; }
        a[cx]=(uint16_t)(a[cx]+sum);
    }
}

/* Gaussian kernel, weights summing to 65536; returns the radius. */
static int hal_kernel(int32_t *kw, float sigma)
{
    int r=(int)(3*sigma+0.999f);
    if (r<1) r=1;
    if (r>HAL_KMAX) r=HAL_KMAX;
    float w[HAL_KMAX+1], sum=0;
    for (int i=0;i<=r;i++) { w[i]=expf(-(float)(i*i)/(2*sigma*sigma)); sum+=i ? 2*w[i] : w[i]; }
    int32_t tot=0;
    for (int i=1;i<=r;i++) { kw[i]=(int32_t)(65536.0f*w[i]/sum+0.5f); tot+=2*kw[i]; }
    kw[0]=65536-tot;
    return r;
}
/* One line (n values, step apart) blurred in place, edges repeated. */
static void hal_blur_line(uint16_t *v, unsigned n, unsigned step, const int32_t *kw, int r, int32_t *tmp)
{
    for (int i=0;i<(int)n+2*r;i++) { int k=i-r; k=k<0 ? 0 : k>=(int)n ? (int)n-1 : k; tmp[i]=v[(unsigned)k*step]; }
    for (unsigned i=0;i<n;i++) {
        /* values <= 65535, weights sum to 65536: the total fits 32 bits */
        const int32_t *p=tmp+i+r;
        uint32_t acc=(uint32_t)p[0]*(uint32_t)kw[0];
        for (int k=1;k<=r;k++) acc+=(uint32_t)(p[k]+p[-k])*(uint32_t)kw[k];
        acc=(acc+32768u)>>16;
        v[i*step]=(uint16_t)MIN(acc,65535u);
    }
}
static void hal_blur_map(uint16_t *m, unsigned w, unsigned h, unsigned stride, float sigma, int32_t *tmp)
{
    int32_t kw[HAL_KMAX+1];
    int r=hal_kernel(kw,sigma);
    for (unsigned y=0;y<h;y++) hal_blur_line(m+y*stride,w,1,kw,r,tmp);
    for (unsigned x=0;x<w;x++) hal_blur_line(m+x,h,stride,kw,r,tmp);
}
/* uint16 linear -> gamma-coded byte, in place (byte i over the uint16 i/2,
 * already read). */
static void hal_encode(uint16_t *m, unsigned h, unsigned stride, const uint8_t *inv)
{
    uint8_t *b=(uint8_t *)m;
    for (unsigned i=0;i<h*stride;i++) b[i]=inv[m[i]>>4];
}
/* Phase 4: cell means, tail map, blurs, coding. */
static void hal_blur(struct hal_job *j)
{
    struct hal_tables *t=j->t;
    for (unsigned cy=0;cy<j->mh;cy++) {
        unsigned rows=MIN(HAL_CELL,j->rows-cy*HAL_CELL), count=HAL_CELL*rows*255u;
        uint16_t *a=j->acc+cy*HAL_STRIDE;
        for (unsigned cx=0;cx<j->mw;cx++) a[cx]=(uint16_t)(((uint32_t)a[cx]*65535u+count/2)/count);
        for (unsigned cx=j->mw;cx<HAL_STRIDE;cx++) a[cx]=0;
    }
    for (unsigned ty=0;ty<j->th;ty++) {
        uint16_t *o=j->tail+ty*HAL_TSTRIDE;
        for (unsigned tx=0;tx<j->tw;tx++) {
            unsigned s=0,n=0;
            for (unsigned dy=0;dy<2;dy++) for (unsigned dx=0;dx<2;dx++) {
                unsigned cy=ty*2+dy, cx=tx*2+dx;
                if (cy<j->mh && cx<j->mw) { s+=j->acc[cy*HAL_STRIDE+cx]; n++; }
            }
            o[tx]=(uint16_t)(n ? (s+n/2)/n : 0);
        }
        for (unsigned tx=j->tw;tx<HAL_TSTRIDE;tx++) o[tx]=0;
    }
    hal_blur_map(j->acc,j->mw,j->mh,HAL_STRIDE,0.0025f*j->width/HAL_CELL,t->tmp);
    hal_blur_map(j->tail,j->tw,j->th,HAL_TSTRIDE,0.012f*j->width/(2*HAL_CELL),t->tmp);
    hal_encode(j->acc,j->mh,HAL_STRIDE,t->inv);
    hal_encode(j->tail,j->th,HAL_TSTRIDE,t->inv);
}

/* Bilinear position: centre p (in 1/256 cell) -> cell c0, c1, weight f. */
static inline void hal_pos(int p, unsigned n, unsigned *c0, unsigned *c1, unsigned *f)
{
    if (p<0) { *c0=*c1=0; *f=0; return; }
    unsigned c=(unsigned)p>>8;
    if (c>=n-1) { *c0=*c1=n-1; *f=0; return; }
    *c0=c; *c1=c+1; *f=(unsigned)p&255u;
}
/* Phase 5, per row: the two map rows for row y (8.8 coded), n+1 values. */
static void hal_row_prep(const struct hal_job *j, unsigned y, uint16_t *ri, uint16_t *rt)
{
    const uint8_t *im=(const uint8_t *)j->acc, *tm=(const uint8_t *)j->tail;
    unsigned c0,c1,f;
    hal_pos((int)(2*y+1)*8-128,j->mh,&c0,&c1,&f);
    for (unsigned x=0;x<j->mw;x++) ri[x]=(uint16_t)(im[c0*HAL_STRIDE+x]*(256u-f)+im[c1*HAL_STRIDE+x]*f);
    hal_pos((int)(2*y+1)*4-128,j->th,&c0,&c1,&f);
    for (unsigned x=0;x<j->tw;x++) rt[x]=(uint16_t)(tm[c0*HAL_TSTRIDE+x]*(256u-f)+tm[c1*HAL_TSTRIDE+x]*f);
}
static inline int hal_clamp8(int v) { return v<0 ? 0 : v>255 ? 255 : v; }
/* Phase 5: one row, src -> dst (may not overlap). Returns words changed. */
static unsigned hal_row(const struct hal_job *j, uint32_t *dst, const uint32_t *src, const uint16_t *ri, const uint16_t *rt)
{
    const struct hal_tables *t=j->t;
    const int ai0=j->ai[0],ai1=j->ai[1],ai2=j->ai[2],at0=j->at[0],at1=j->at[1],at2=j->at[2];
    unsigned changed=0;
    for (unsigned i=0;i<j->width/2;i++) {
        uint32_t w=src[i];
        unsigned c0,c1,f;
        hal_pos((int)(32*i)-112,j->mw,&c0,&c1,&f);
        unsigned ci=(ri[c0]*(256u-f)+ri[c1]*f)>>12;
        hal_pos((int)(16*i)-120,j->tw,&c0,&c1,&f);
        unsigned ct=(rt[c0]*(256u-f)+rt[c1]*f)>>12;
        if (!ci && !ct) { dst[i]=w; continue; }
        int gi=t->dec[ci], gt=t->dec[ct];
        int u=(int8_t)(w&255u), v=(int8_t)((w>>16)&255u);
        int yv[2]={(int)((w>>8)&255u),(int)(w>>24)};
        int dr_ru=(91881*v+32768)>>16, dg_uv=(22554*u+46802*v+32768)>>16, db_bu=(116130*u+32768)>>16;
        int sr=0,sg=0,sb=0,dy[2]={0,0};
        for (int p=0;p<2;p++) {
            int y=yv[p], k=t->key16[y];
            int ei=gi>k ? gi-k : 0, et=gt>k ? gt-k : 0;
            if (!ei && !et) continue;
            int gr=(ei*ai0+et*at0)>>12, gg=(ei*ai1+et*at1)>>12, gb=(ei*ai2+et*at2)>>12;
            gr=MIN(gr,65535); gg=MIN(gg,65535); gb=MIN(gb,65535);
            int r=hal_clamp8(y+dr_ru), g=hal_clamp8(y-dg_uv), b=hal_clamp8(y+db_bu);
            uint32_t L;
            L=t->lin[r]; L+=((65535u-L)*(uint32_t)gr)>>16; int r2=t->inv[L>>4];
            L=t->lin[g]; L+=((65535u-L)*(uint32_t)gg)>>16; int g2=t->inv[L>>4];
            L=t->lin[b]; L+=((65535u-L)*(uint32_t)gb)>>16; int b2=t->inv[L>>4];
            int er=r2-r, eg=g2-g, eb=b2-b;
            dy[p]=(19595*er+38470*eg+7471*eb+32768)>>16;
            sr+=er; sg+=eg; sb+=eb;
        }
        if (!sr && !sg && !sb && !dy[0] && !dy[1]) { dst[i]=w; continue; }
        int y0=hal_clamp8(yv[0]+dy[0]), y1=hal_clamp8(yv[1]+dy[1]);
        int du=(-11059*sr-21709*sg+32768*sb+65536)>>17, dv=(32768*sr-27439*sg-5329*sb+65536)>>17;
        int u2=u+du, v2=v+dv;
        u2=u2<-128 ? -128 : u2>127 ? 127 : u2;
        v2=v2<-128 ? -128 : v2>127 ? 127 : v2;
        dst[i]=((uint32_t)(uint8_t)u2)|((uint32_t)y0<<8)|((uint32_t)(uint8_t)v2<<16)|((uint32_t)y1<<24);
        changed++;
    }
    return changed;
}
