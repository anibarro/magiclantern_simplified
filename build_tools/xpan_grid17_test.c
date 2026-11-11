/* Host check for the 17-point colour table code (build_xpan.sh, M50):
 *   xpan_grid17_test ROM0.BIN
 * Uses Canon's own 10-point default table from the ROM (E0B18330). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/xpan-native-table.h"

static unsigned index10(unsigned r, unsigned g, unsigned b)
{
    return 5*(g%5+5*(b%5))+(b>4 ? 513:0)+(g>4 ? 257:0)+r+(r>4 ? 124:0);
}
int main(int argc, char **argv)
{
    static uint32_t canon[1024], out[XPAN_GRID17_WORDS];
    static uint16_t ident[2*2*2*3];
    static unsigned char seen[XPAN_GRID17_WORDS];
    FILE *f=argc>1 ? fopen(argv[1],"rb") : 0;
    if (!f || fseek(f,0xB18330,SEEK_SET) || fread(canon,4,1024,f)!=1024) { puts("grid17: cannot read Canon's table from the ROM"); return 1; }
    fclose(f);
    /* The ROM table is the identity, nodes 64 apart: node 8 is white (512) and
     * node 9 a guard beyond it (576). Picture style tables hold white there
     * (XPan's format check): give the guard nodes their node-8 neighbour. */
    if ((canon[index10(8,8,8)]>>20)!=512 || (canon[index10(9,9,9)]>>20)!=576 || (canon[index10(4,4,4)]>>20)!=256) {
        puts("grid17: Canon's ROM table is not the 64-step identity"); return 1;
    }
    for (unsigned r=0;r<10;r++) for (unsigned g=0;g<10;g++) for (unsigned b=0;b<10;b++)
        if (r==9 || g==9 || b==9) canon[index10(r,g,b)]=canon[index10(r>8 ? 8:r,g>8 ? 8:g,b>8 ? 8:b)];
    if (xpan_native_table_validate(canon)) { puts("grid17: Canon's ROM table fails the format check"); return 1; }
    for (unsigned i=0;i<8;i++) { ident[i*3]=i&1 ? 65535:0; ident[i*3+1]=i&2 ? 65535:0; ident[i*3+2]=i&4 ? 65535:0; }
    struct xpan_lut cube;
    if (!xpan_lut_init(&cube,ident,2,24)) return 1;
    for (unsigned layout=XPAN_GRID17_RGB; layout<=XPAN_GRID17_PARITY; layout++) {
        memset(seen,0,sizeof(seen));
        for (unsigned r=0;r<17;r++) for (unsigned g=0;g<17;g++) for (unsigned b=0;b<17;b++) {
            unsigned i=xpan_native17_index(layout,r,g,b);
            if (i>=XPAN_GRID17_WORDS || seen[i]++) { printf("grid17: layout %u is not a bijection at %u,%u,%u\n",layout,r,g,b); return 1; }
        }
        struct xpan_native_cube_stats stats; uint32_t flags=0;
        memset(out,0xff,sizeof(out));
        if (!xpan_native_table_prepare_cube17(out,canon,&cube,65535,1,0,layout,&stats,&flags) || !stats.identity_bypass) {
            printf("grid17: layout %u identity conversion failed (flags %x)\n",layout,(unsigned)flags); return 1;
        }
        /* Even nodes are Canon's nodes; grey stays grey and rises to white. */
        unsigned prev=0;
        for (unsigned r=0;r<17;r+=2) for (unsigned g=0;g<17;g+=2) for (unsigned b=0;b<17;b+=2)
            if (out[xpan_native17_index(layout,r,g,b)]!=canon[index10(r/2,g/2,b/2)]) { printf("grid17: layout %u even node differs\n",layout); return 1; }
        for (unsigned n=0;n<17;n++) {
            uint32_t w=out[xpan_native17_index(layout,n,n,n)];
            if ((w&0xC00FFFFF) || (w>>20)<prev) { printf("grid17: layout %u grey axis broken at %u\n",layout,n); return 1; }
            prev=w>>20;
        }
        if (prev!=512) { printf("grid17: layout %u white is %u\n",layout,prev); return 1; }
        /* Through the grading path (strength 100 %, identity Cube not flagged):
         * every node within 1 step of the resampled table. */
        static uint32_t graded[XPAN_GRID17_WORDS];
        if (!xpan_native_table_prepare_cube17(graded,canon,&cube,65535,0,0,layout,&stats,&flags) || stats.prepared_entries!=XPAN_GRID17_WORDS) {
            printf("grid17: layout %u grading failed (flags %x)\n",layout,(unsigned)flags); return 1;
        }
        for (unsigned i=0;i<XPAN_GRID17_WORDS;i++) for (unsigned s=0;s<30;s+=10) {
            int a=(out[i]>>s)&1023, b=(graded[i]>>s)&1023, d=a-b;
            if (d<0) d=-d;
            if (d>1 && d<1023) { printf("grid17: layout %u word %u differs by %d\n",layout,i,d); return 1; }
        }
    }
    puts("grid17: 4 layouts are bijections; even nodes equal Canon's; grey axis and grading as expected");
    return 0;
}
