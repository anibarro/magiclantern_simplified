/* Bounded normalized .cube reader for the native-table experiment. Loading is
 * done outside Canon's transfer hook; image pixels never enter this parser. */
#include "xpan-cube.h"
#include <string.h>

static int space(char c) { return c==' ' || c=='\t' || c=='\r'; }
static char lower(char c) { return c>='A' && c<='Z' ? c+('a'-'A'):c; }
static int equal(const char *a,const char *b)
{
    while (*a && *b && lower(*a)==lower(*b)) { a++; b++; }
    return !*a && !*b;
}
static int fail(struct xpan_cube_parser *p,unsigned code) { p->error=code; return 0; }

/* Read decimal/scientific notation directly into RGB16. Nine significant
 * decimal digits exceed its precision. Wider intermediates are only used at
 * load time. Range checks use the full token, so 1.0000000001 is not accepted
 * merely because it would round to 65535. unit is exact zero/one, or -1. */
static int unit_number(const char **text,uint16_t *value,int *unit)
{
    const char *s=*text;
    while (space(*s)) s++;
    int negative=0,decimal=0,digits=0,frac=0,significant=0,kept=0,one=0;
    uint32_t mantissa=0;
    if (*s=='-' || *s=='+') { negative=*s=='-'; s++; }
    while ((*s>='0' && *s<='9') || *s=='.') {
        if (*s=='.') { if (decimal) return 0; decimal=1; s++; continue; }
        int d=*s++-'0'; digits++;
        if (decimal) frac++;
        if (significant || d) {
            if (!significant) one=d==1;
            else if (d) one=0;
            significant++;
            if (kept<9) { mantissa=mantissa*10+d; kept++; }
        }
    }
    if (!digits) return 0;
    int exponent=0,exponent_negative=0;
    if (*s=='e' || *s=='E') {
        s++;
        if (*s=='+' || *s=='-') { exponent_negative=*s=='-'; s++; }
        if (*s<'0' || *s>'9') return 0;
        while (*s>='0' && *s<='9') {
            exponent=exponent*10+(*s++-'0');
            if (exponent>1000) return 0;
        }
        if (exponent_negative) exponent=-exponent;
    }
    if (*s && !space(*s)) return 0;
    while (space(*s)) s++;
    *text=s;
    if (!significant) { *value=0; *unit=0; return 1; }
    int order=significant+exponent-frac;
    if (negative || order>1 || (order==1 && !one)) return -1;
    if (order==1) { *value=65535; *unit=1; return 1; }
    int power=exponent-frac+significant-kept;
    *unit=-1;
    if (power < -14) { *value=0; return 1; }
    if (power>=0) return -1; /* A nonzero value below one needs a denominator. */
    uint64_t denominator=1;
    for (int i=0; i < -power; i++) denominator*=10;
    *value=(uint16_t)(((uint64_t)mantissa*65535+denominator/2)/denominator);
    return 1;
}

static int normalized_args(struct xpan_cube_parser *p,const char *s,unsigned flag,int count)
{
    if (p->headers&flag) return fail(p,2);
    for (int i=0; i<count; i++) {
        uint16_t value; int unit;
        if (unit_number(&s,&value,&unit)!=1) return fail(p,4);
        int expected=flag==2 ? 0:flag==4 ? 1:i;
        if (unit!=expected) return fail(p,4);
    }
    if (*s) return fail(p,4);
    p->headers|=flag;
    return 1;
}

static int line(struct xpan_cube_parser *p)
{
    p->line_number++;
    p->line[p->line_used]=0;
    char *s=p->line;
    if (p->line_number==1 && p->line_used>=3 &&
        (unsigned char)s[0]==0xef && (unsigned char)s[1]==0xbb && (unsigned char)s[2]==0xbf) s+=3;
    while (space(*s)) s++;
    for (char *c=s; *c; c++) if (*c=='#') { *c=0; break; }
    if (!*s) return 1;
    char *args=s;
    while (*args && !space(*args)) args++;
    if (*args) *args++=0;
    while (space(*args)) args++;
    if (equal(s,"TITLE")) return p->components ? fail(p,2):1;
    if (equal(s,"LUT_1D_SIZE") || equal(s,"LUT_1D_INPUT_RANGE")) return fail(p,8);
    if (equal(s,"LUT_3D_SIZE")) {
        if (p->components || p->size || !*args) return fail(p,2);
        unsigned size=0;
        while (*args>='0' && *args<='9') {
            size=size*10+(*args++-'0');
            if (size>XPAN_CUBE_MAX_SIZE) return fail(p,3);
        }
        while (space(*args)) args++;
        if (*args || size<2 || size*size*size*3>p->capacity) return fail(p,3);
        p->size=size; return 1;
    }
    if (equal(s,"DOMAIN_MIN")) return p->components ? fail(p,2):normalized_args(p,args,2,3);
    if (equal(s,"DOMAIN_MAX")) return p->components ? fail(p,2):normalized_args(p,args,4,3);
    if (equal(s,"LUT_3D_INPUT_RANGE")) return p->components ? fail(p,2):normalized_args(p,args,8,2);
    if (!p->size || p->components>=p->size*p->size*p->size*3) return fail(p,7);
    uint16_t rgb[3]; int unit;
    const char *first=s;
    int result=unit_number(&first,&rgb[0],&unit);
    if (result!=1 || *first) return fail(p,result<0 ? 6:5);
    const char *rest=args;
    for (int c=1; c<3; c++) {
        result=unit_number(&rest,&rgb[c],&unit);
        if (result!=1) return fail(p,result<0 ? 6:5);
    }
    if (*rest) return fail(p,5);
    unsigned point=p->components/3;
    for (unsigned c=0; c<3; c++) {
        unsigned coordinate=c==0 ? point%p->size:c==1 ? (point/p->size)%p->size:point/(p->size*p->size);
        unsigned expected=(coordinate*65535+(p->size-1)/2)/(p->size-1);
        if (rgb[c]!=expected) p->identity=0;
        p->rgb[p->components++]=rgb[c];
    }
    return 1;
}

void xpan_cube_init(struct xpan_cube_parser *p,uint16_t *rgb,unsigned capacity)
{
    memset(p,0,sizeof(*p)); p->rgb=rgb; p->capacity=capacity; p->identity=1;
}
int xpan_cube_feed(struct xpan_cube_parser *p,const char *data,size_t bytes)
{
    if (!p || !p->rgb || !data || p->error) return 0;
    for (size_t i=0; i<bytes; i++) {
        char c=data[i];
        if (!c) return fail(p,5);
        if (c=='\n') {
            if (!p->discard_comment && !line(p)) return 0;
            if (p->discard_comment) p->line_number++;
            p->line_used=p->discard_comment=0;
        } else if (!p->discard_comment) {
            /* A full-line comment may be arbitrarily long without occupying
             * line storage; data/header lines remain strictly bounded. */
            if (c=='#') {
                unsigned k=0; while (k<p->line_used && space(p->line[k])) k++;
                if (k==p->line_used) { p->discard_comment=1; continue; }
            }
            if (p->line_used>=sizeof(p->line)-1) return fail(p,1);
            p->line[p->line_used++]=c;
        }
    }
    return 1;
}
int xpan_cube_finish(struct xpan_cube_parser *p)
{
    if (!p || p->error) return 0;
    if (p->line_used && !p->discard_comment && !line(p)) return 0;
    p->line_used=0;
    if (!p->size || p->components!=p->size*p->size*p->size*3) return fail(p,7);
    return 1;
}
const char *xpan_cube_error(const struct xpan_cube_parser *p)
{
    static const char *errors[]={"Ready","Cube line too long","Invalid Cube header",
        "Cube grid must be 2..33","Cube domain must be 0..1","Invalid Cube number",
        "Cube RGB output must be 0..1","Wrong Cube entry count","1D/shaper Cube unsupported"};
    return p->error<sizeof(errors)/sizeof(errors[0]) ? errors[p->error]:"Invalid Cube";
}
