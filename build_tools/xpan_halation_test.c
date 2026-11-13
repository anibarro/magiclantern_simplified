/* Host test of src/xpan-halation.inc (README-XPAN-M50.md "Halation").
 *
 *   xpan_halation_test selftest
 *       synthetic 6000 x 4000 night scene in Canon's YCbCr 4:2:2 words:
 *       checks the glow is red-orange outside a lamp, the lamp core and far
 *       pixels unchanged, and an image without highlights untouched; run by
 *       build_xpan.sh.
 *   xpan_halation_test file IN.uyvy OUT.uyvy WIDTH ROWS STRENGTH
 *       the camera's phases on a raw dump (words U Y0 V Y1, U/V signed),
 *       for comparing with build_tools/xpan_halation_proto.py.
 *
 * The phases run in the camera's order, rows split in two halves as the two
 * stage workers split them (on a multiple of 32 rows). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#define MIN(a,b) ((a)<(b) ? (a):(b))
#include "../src/xpan-halation.inc"

static unsigned run(uint32_t *img, uint32_t *out, unsigned width, unsigned rows, int pct, unsigned *white)
{
    static struct hal_tables t;
    static uint16_t acc[HAL_STRIDE*HAL_MH], tail[HAL_TSTRIDE*HAL_TH], ri[HAL_MW+1], rt[HAL_TW+1];
    struct hal_job j={0};
    j.acc=acc; j.tail=tail; j.t=&t;
    hal_tables_init(&t);
    hal_set_strength(&j,pct);
    if (!hal_fits(width,rows)) { fprintf(stderr,"size does not fit\n"); exit(2); }
    hal_job_size(&j,width,rows);
    unsigned half=(rows/2)&~31u, pitch=width/2;
    memset(t.hist,0,sizeof(t.hist));
    for (unsigned y=0;y<rows;y+=8) hal_hist_row(t.hist[y>=half],img+y*pitch,width);
    hal_set_white(&j);
    memset(acc,0,sizeof(acc));
    for (unsigned y=0;y<rows;y++) hal_acc_row(&j,img+y*pitch,y);
    hal_blur(&j);
    unsigned changed=0;
    for (unsigned y=0;y<rows;y++) { hal_row_prep(&j,y,ri,rt); changed+=hal_row(&j,out+y*pitch,img+y*pitch,ri,rt); }
    *white=j.white;
    return changed;
}

static uint32_t word(int y0, int y1, int u, int v) { return (uint32_t)(uint8_t)u|(uint32_t)y0<<8|(uint32_t)(uint8_t)v<<16|(uint32_t)y1<<24; }
static void rgb_of(uint32_t w, int p, int *r, int *g, int *b)
{
    int u=(int8_t)(w&255), v=(int8_t)((w>>16)&255), y=p ? (int)(w>>24) : (int)((w>>8)&255);
    *r=y+(int)lround(1.402*v); *g=y-(int)lround(0.344136*u+0.714136*v); *b=y+(int)lround(1.772*u);
}

static int selftest(void)
{
    const unsigned W=6000,H=4000,P=W/2;
    uint32_t *img=malloc(W*H*2), *out=malloc(W*H*2);
    int fails=0;
    /* 0. the camera's allocations (xpan-m50.c hal_poll), each under 512 KB */
    {
        unsigned sz[4]={HAL_STRIDE*HAL_MH*2u,HAL_TSTRIDE*HAL_TH*2u,(unsigned)sizeof(struct hal_tables),
                        2u*(2u*(HAL_MW+8)+2u*(HAL_TW+8)+4u*(HAL_MAX_W/2))+64u};
        for (int i=0;i<4;i++) if (sz[i]+32u>=512u*1024u) { printf("FAIL: allocation %d is %u bytes\n",i,sz[i]); fails++; }
        printf("ok: halation allocations %u + %u + %u + %u bytes\n",sz[0],sz[1],sz[2],sz[3]);
    }
    /* 1. dark scene with a mid-grey card, nothing near white: untouched */
    for (unsigned i=0;i<P*H;i++) img[i]=word(28,28,0,0);
    for (unsigned y=1500;y<2500;y++) for (unsigned x=1500/2;x<2500/2;x++) img[y*P+x]=word(150,150,0,0);
    unsigned white, ch=run(img,out,W,H,100,&white);
    if (ch || memcmp(img,out,W*H*2)) { printf("FAIL: no highlights, %u words changed\n",ch); fails++; }
    else printf("ok: no highlights: image unchanged (white %u)\n",white);
    /* 2. a lamp (radius 40 px, Y 222) */
    for (unsigned i=0;i<P*H;i++) img[i]=word(28,28,0,0);
    for (int y=900;y<1100;y++) for (int x=2900/2;x<3100/2;x++) {
        int dx=2*x+1-3000, dy=y-1000;
        if (dx*dx+dy*dy<40*40) img[y*P+x]=word(222,222,0,0);
    }
    ch=run(img,out,W,H,100,&white);
    int r,g,b,r0,g0,b0;
    rgb_of(img[1000*P+3060/2],0,&r0,&g0,&b0);
    rgb_of(out[1000*P+3060/2],0,&r,&g,&b);          /* 60 px right of the centre: 20 px outside */
    printf("rim (20 px out): rgb %d %d %d -> %d %d %d; white %u, %u words changed\n",r0,g0,b0,r,g,b,white,ch);
    if (!(r-r0>=20 && r-r0>g-g0 && g-g0>=b-b0 && b-b0>=0)) { printf("FAIL: rim not red-orange\n"); fails++; }
    rgb_of(out[1000*P+3000/2],0,&r,&g,&b);
    if (abs(r-222)>2 || abs(g-222)>2 || abs(b-222)>2) { printf("FAIL: lamp core changed: %d %d %d\n",r,g,b); fails++; }
    else printf("ok: lamp core unchanged (%d %d %d)\n",r,g,b);
    rgb_of(out[3000*P+500/2],0,&r,&g,&b);
    if (out[3000*P+500/2]!=img[3000*P+500/2]) { printf("FAIL: far pixel changed\n"); fails++; }
    else printf("ok: far pixel unchanged\n");
    if (white!=222) { printf("FAIL: white %u, expected 222 (lamp)\n",white); fails++; }
    free(img); free(out);
    printf(fails ? "HALATION TEST FAILED\n" : "HALATION TEST OK\n");
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc==2 && !strcmp(argv[1],"selftest")) return selftest();
    if (argc==7 && !strcmp(argv[1],"file")) {
        unsigned W=(unsigned)atoi(argv[4]), H=(unsigned)atoi(argv[5]);
        uint32_t *img=malloc((size_t)W*H*2), *out=malloc((size_t)W*H*2);
        FILE *f=fopen(argv[2],"rb");
        if (!f || fread(img,2,(size_t)W*H,f)!=(size_t)W*H) { fprintf(stderr,"read %s\n",argv[2]); return 2; }
        fclose(f);
        unsigned white, ch=run(img,out,W,H,atoi(argv[6]),&white);
        f=fopen(argv[3],"wb"); fwrite(out,2,(size_t)W*H,f); fclose(f);
        printf("white %u, %u of %u words changed (%.1f%%)\n",white,ch,W*H/2,100.0*ch/(W*H/2));
        return 0;
    }
    fprintf(stderr,"usage: %s selftest | file IN OUT WIDTH ROWS STRENGTH\n",argv[0]);
    return 2;
}
