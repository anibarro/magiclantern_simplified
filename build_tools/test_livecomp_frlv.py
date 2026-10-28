#!/usr/bin/env python3
"""Host checks for the full-sensor readout and composite (not hardware evidence).

1. fullres-regs.h: reference register values, NRZI, blanking, list rewriting.
2. fullres-lv.c compiled against stub headers and a simulated camera:
   hooks, x5 switch, our RAW slurp, merge, DNG, restore and fault paths,
   in Live Composite (maximum) and Live Bulb (sum) mode.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MOD = ROOT / 'modules/livecomp'

REGS_TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "fullres-regs.h"
#include <stdlib.h>
#include "raw14-max.h"
#include "raw14-add.h"
int main(void) {
    { /* Live Bulb add == reference: acc + (frame - black), clamped to
       * [0,white], saturated samples stay; every sample, random data. */
        enum { B=8000 }; static uint16_t a[7*B],b[7*B],ref[7*B];
        srand(5);
        for (int i=0;i<7*B;i++) { a[i]=(uint16_t)rand();b[i]=(uint16_t)rand(); }
        memcpy(ref,a,sizeof(a));
        for (unsigned n=0;n<B;n++) {
            uint16_t x[8],y[8];lc_raw14_unpack(ref+7*n,x);lc_raw14_unpack(b+7*n,y);
            for (int p=0;p<8;p++) {
                if (x[p]>=15000) continue;
                int v=x[p]+y[p]-2048;x[p]=(uint16_t)(v<0 ? 0:v>15000 ? 15000:v);
            }
            lc_raw14_pack(ref+7*n,x);
        }
        lc_raw14_add(a,b,B,2048,15000);
        assert(!memcmp(a,ref,sizeof(a)));
    }
    { /* LCM55 word kernel == reference kernel, bit for bit: random words,
       * realistic values with edges (0, black, white, 16383), several
       * black/white levels incl. white > 16383 */
        enum { P=4000 }; static uint32_t a[7*P],b[7*P],c[7*P];
        static const unsigned edge[]={0,1,2047,2048,2049,14999,15000,15001,16382,16383};
        static const unsigned bl[]={2048,2047,2063,512},wh[]={15000,16383,14000,20000};
        srand(13);
        for (int round=0;round<16;round++) {
            for (unsigned n=0;n<2*P;n++) {
                uint16_t x[8],y[8];
                for (int p=0;p<8;p++) {
                    x[p]=(uint16_t)(round&1 ? edge[rand()%10]:rand()%5 ? 2000u+(unsigned)rand()%400u:(unsigned)rand()&16383u);
                    y[p]=(uint16_t)(round&2 ? edge[rand()%10]:rand()%5 ? 2000u+(unsigned)rand()%400u:(unsigned)rand()&16383u);
                }
                lc_raw14_pack((uint16_t *)a+7*n,x);lc_raw14_pack((uint16_t *)b+7*n,y);
            }
            if (round>=12) for (int i=0;i<7*P;i++) { a[i]=((uint32_t)rand()<<16)^rand();b[i]=((uint32_t)rand()<<16)^rand(); }
            memcpy(c,a,sizeof(a));
            lc_raw14_add((uint16_t *)c,(const uint16_t *)b,2*P,bl[round&3],wh[(round>>2)&3]);
            lc_raw14_add_sel(a,b,P,bl[round&3],wh[(round>>2)&3]);
            assert(!memcmp(a,c,sizeof(a)));
            /* LCM59 dark subtraction: word kernel == reference, same edges */
            lc_raw14_sub((uint16_t *)c,(const uint16_t *)b,2*P,bl[round&3],wh[(round>>2)&3]);
            lc_raw14_sub_sel(a,b,P,bl[round&3],wh[(round>>2)&3]);
            assert(!memcmp(a,c,sizeof(a)));
        }
    }
    { /* Dark subtraction physics: 10 light frames of black+100+20 (20 = dark
       * signal) minus 10 dark frames of black+20 leave black+1000; a
       * saturated sample stays saturated. */
        enum { B=64 }; static uint16_t acc[7*B],f[7*B],d[7*B];uint16_t s[8];
        for (unsigned n=0;n<B;n++) { for (int p=0;p<8;p++) s[p]=2048+120;lc_raw14_pack(f+7*n,s);
                                     for (int p=0;p<8;p++) s[p]=2048+20;lc_raw14_pack(d+7*n,s); }
        memcpy(acc,f,sizeof(f));
        for (int k=1;k<10;k++) lc_raw14_add(acc,f,B,2048,15000);
        for (int k=0;k<10;k++) lc_raw14_sub_sel((uint32_t *)(void *)acc,(const uint32_t *)(const void *)d,B/2,2048,15000);
        for (unsigned n=0;n<B;n++) { lc_raw14_unpack(acc+7*n,s);for (int p=0;p<8;p++) assert(s[p]==2048+1000); }
        lc_raw14_unpack(acc,s);s[3]=15000;lc_raw14_pack(acc,s);
        lc_raw14_sub_sel((uint32_t *)(void *)acc,(const uint32_t *)(const void *)d,B/2,2048,15000);
        lc_raw14_unpack(acc,s);assert(s[3]==15000 && s[2]==2048+980);
    }
    { /* Bulb physics: 10 frames of black+100 sum to black+1000; noise around
       * black does not drift the sum; bright light clips at white. */
        enum { B=64 }; static uint16_t acc[7*B],f[7*B];uint16_t s[8];
        for (unsigned n=0;n<B;n++) { for (int p=0;p<8;p++) s[p]=2048+100;lc_raw14_pack(f+7*n,s); }
        memcpy(acc,f,sizeof(f));
        for (int k=1;k<10;k++) lc_raw14_add(acc,f,B,2048,15000);
        for (unsigned n=0;n<B;n++) { lc_raw14_unpack(acc+7*n,s);for (int p=0;p<8;p++) assert(s[p]==2048+1000); }
        for (unsigned n=0;n<B;n++) { for (int p=0;p<8;p++) s[p]=(uint16_t)(p&1 ? 2048+40:2048-40);lc_raw14_pack(f+7*n,s); }
        memcpy(acc,f,sizeof(f));
        for (int k=1;k<100;k++) { lc_raw14_add(acc,f,B,2048,15000); }
        for (unsigned n=0;n<B;n++) { lc_raw14_unpack(acc+7*n,s);for (int p=0;p<8;p++) assert(s[p]==(p&1 ? 2048+4000:0)); }
        for (unsigned n=0;n<B;n++) { for (int p=0;p<8;p++) s[p]=9000;lc_raw14_pack(f+7*n,s); }
        memcpy(acc,f,sizeof(f));
        for (int k=1;k<4;k++) lc_raw14_add(acc,f,B,2048,15000);
        for (unsigned n=0;n<B;n++) { lc_raw14_unpack(acc+7*n,s);for (int p=0;p<8;p++) assert(s[p]==15000); }
    }
    { /* select max == reference max, every sample, incl. ties/edges */
        enum { P=10000 }; static uint32_t a[7*P],b[7*P],c[7*P];
        srand(11);
        for (int round=0;round<4;round++) {
            for (int i=0;i<7*P;i++) { b[i]=((uint32_t)rand()<<16)^rand(); if (!round) a[i]=c[i]=((uint32_t)rand()<<16)^rand(); }
            if (round==2) for (int i=0;i<7*P;i++) b[i]=a[i]^(rand()&rand()&rand()&0xFFFF);
            if (round==3) memcpy(b,a,sizeof(a));
            lc_raw14_max((uint16_t *)c,(const uint16_t *)b,2*P);
            unsigned ch=lc_raw14_max_sel(a,b,P);
            assert(!memcmp(a,c,sizeof(a)) && (round<3 || ch==0));
        }
    }
    struct lc_frr_config c; struct lc_frr_stats st; memset(&st,0,sizeof(st));
    lc_frr_configure(&c,LC_FRR_FPS_DEFAULT);
    assert(c.raw_h==0x538 && c.raw_v==0xDB2 && c.timer_a==0x56B && c.timer_b==57635 && c.blanking==10);
    for (unsigned i=0;i<LC_FRR_FPS_CHOICES;i++) {   /* menu order, 2 s ... 60 s */
        static const unsigned ms[]={2500,3000,4000,5000,6000,8000,10000,13000,15000,20000,25000,30000,40000,50000,60000};
        assert(LC_FRR_FPS_CHOICES==sizeof(ms)/sizeof(*ms) && lc_frr_timing[i].period_ms==ms[i]);
        /* timer A = k x 1388 - 1: the reference line time stretched k times */
        assert((lc_frr_timing[i].timer_a+1)%1388==0 && lc_frr_timing[i].timer_a<=0xFFFF);
        lc_frr_configure(&c,i);assert(c.blanking==10);          /* 100% exposure, no gap */
    }
    assert(LC_FRR_TESTED<=LC_FRR_FPS_CHOICES && LC_FRR_FPS_DEFAULT<LC_FRR_TESTED);
    /* LCB3: 2 s test shutter, k=1, outside the menu list */
    { const struct lc_frr_timing *t=lc_frr_timing_of(LC_FRR_FPS_2S);double period=(double)(t->timer_a+1)*(t->timer_b+1)/32e6;
      assert(t->period_ms==2000 && t->timer_a==0x56B && period>1.999 && period<2.001 && lc_frr_timing_of(99)==&lc_frr_timing[LC_FRR_FPS_DEFAULT]); }
    /* Reference EOS M CROP_Full_Res constants (crop_rec 5f0cda8), 3 fps. */
    c.timer_a=0x56B;c.timer_b=0x1E03;c.blanking=lc_frr_blanking(0x1E03,100);
    /* period = (A+1)(B+1) / 32 MHz, within 0.1% of the menu value */
    for (unsigned i=0;i<LC_FRR_FPS_CHOICES;i++) {
        const struct lc_frr_timing *t=&lc_frr_timing[i];
        double period=(double)(t->timer_a+1)*(t->timer_b+1)/32e6,want=t->period_ms/1000.0;
        assert(period>want*0.999 && period<want*1.001 && t->timer_b<=0xFFFF && t->timer_a>=LC_FRR_TIMER_A);
    }
    for (uint32_t v=0;v<0x10000;v+=7) assert(lc_frr_nrzi_decode(lc_frr_nrzi_encode(v))==v);
    assert(lc_frr_blanking(0x1E03,100)==10 && lc_frr_blanking(0x1E03,50)==0x1E04-0x1E04/2);
    assert(lc_frr_blanking(100,0)==99);
    /* CMOS: only 5 and 7 replaced; terminator kept; overflow refused. */
    uint16_t in[]={0x1123,0x5555,0x2001,0x7abc,0xFFFF},out[8];
    assert(lc_frr_cmos(in,out,8,&st) && out[0]==0x1123 && out[1]==0x5000 && out[2]==0x2001 &&
           out[3]==0x73A0 && out[4]==0xFFFF && in[1]==0x5555 && st.cmos_changes==2);
    assert(!lc_frr_cmos(in,out,3,&st) && st.overflow==1);
    /* ADTG: present registers only, chip-select mask respected, blanking NRZI. */
    uint32_t a[]={0x805F0123,0x81720000,0x12340005,0x82F90000,0xFFFFFFFF},ao[8];
    st.overflow=0;
    assert(lc_frr_adtg(2,a,ao,8,&c,&st));
    assert((ao[0]&0xFFFF)==(lc_frr_nrzi_encode(10)&0xFFFF) && (ao[0]>>16)==0x805F);
    assert((ao[1]&0xFFFF)==(lc_frr_nrzi_encode(0xDB3)&0xFFFF));
    assert(ao[2]==0x12340005 && (ao[3]&0xFFFF)==(lc_frr_nrzi_encode(0x1E03)&0xFFFF) && ao[4]==0xFFFFFFFF);
    assert(st.blanking_seen==1 && (st.orig_blanking&0xFFFF)==lc_frr_nrzi_decode(0x123));
    uint32_t ao2[8];assert(lc_frr_adtg(1,a,ao2,8,&c,&st) && ao2[0]==a[0]); /* cs 1 not in mask 6 */
    /* ENGIO in place, originals recorded once. */
    uint32_t e[]={0xC0F06804,0x4540298,0xC0F06014,0x5DC,0xC0F06008,0x2CB02CB,0xC0F11111,7,
                  0xC0F0713C,1,0xC0F07150,2,0xC0F06828,3,0xFFFFFFFF};
    lc_frr_engio(e,64,&c,&st);
    assert(e[1]==0x0DB20538 && e[3]==0x1E03 && e[5]==0x056B056B && e[7]==7 &&
           e[9]==0xDB3 && e[11]==0xDB2-0x3A && e[13]==0x538+0x32);
    assert(st.orig_6804==0x4540298 && (st.orig_timer_b&0xFFFF)==0x5DC);
    uint32_t v=0;assert(lc_frr_drv(0xC0F38024,&v,&c) && v==((0xDB1u<<16)+0x538-0x11));
    v=5;assert(!lc_frr_drv(0xC0F12345,&v,&c) && v==5);
    unsigned w,h;lc_frr_geometry(0x1C0012,0x0DB20538,&w,&h);
    assert(w==(0x538-0x12)*4 && h==0xDB2-0x1C+1);
    printf("regs ok\n");return 0;
}
'''

# Minimal stand-ins for the ML headers used by fullres-lv.c. The simulated
# camera reacts to set_lv_zoom by calling the installed hooks with Canon-like
# register lists, and to our RAW slurp by filling the capture buffer.
STUB = r'''
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define FAST
#define COUNT(a) (sizeof(a)/sizeof((a)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MEM(a) sim_mem((uint32_t)(a))
#define FIO_MAX_PATH_LENGTH 64
#define UILOCK_SHUTTER 0x10
int is_camera(const char *m,const char *f);
struct card_info { const char *drive_letter; };
struct raw_info { int white_level; uint32_t cfa_pattern; int32_t color_matrix1[18]; int calibration_illuminant1; };
struct lens_info_t { int raw_iso,aperture,focal_len,job_state,wb_mode,kelvin; unsigned WBGain_R,WBGain_G,WBGain_B; int raw_aperture,raw_aperture_min,raw_aperture_max; };
extern struct raw_info raw_info; extern struct lens_info_t lens_info;
extern int lv,LV_PAUSED,RECORDING,lv_dispsize,ml_shutdown_requested,icu_uilock;
extern uint32_t raw_write_chan;
typedef void (*patch_hook_function_cbr)(uint32_t *regs,uint32_t *stack,uint32_t pc);
uint32_t sim_mem(uint32_t a);
int is_movie_mode(void); unsigned get_ms_clock(void); unsigned get_us_clock(void);
void msleep(int ms); void sync_caches(void); void *fio_malloc(size_t n); void fio_free(void *p);
int patch_hook_function(uintptr_t addr,uint32_t orig,patch_hook_function_cbr cb,const char *d);
int unpatch_memory(uintptr_t addr); void set_lv_zoom(int z);
uint32_t shamem_read(uint32_t a); void EngDrvOut(uint32_t r,uint32_t v); void EngDrvOutLV(uint32_t r,uint32_t v);
int edmac_get_state(uint32_t ch); uint32_t edmac_get_pointer(uint32_t ch);
uint32_t edmac_get_address(uint32_t ch);
struct edmac_info { unsigned off1a,off1b,off2a,off2b,off3,xa,xb,ya,yb,xn,yn; };
#define EDMAC_2_BYTES_PER_TRANSFER 0x20000000
void RegisterEDmacCompleteCBR(int ch,void (*cbr)(void*),void *ctx);
void RegisterEDmacAbortCBR(int ch,void (*cbr)(void*),void *ctx);
void RegisterEDmacPopCBR(int ch,void (*cbr)(void*),void *ctx);
void ConnectWriteEDmac(unsigned ch,unsigned where);
void SetEDmac(unsigned ch,void *addr,struct edmac_info *info,int flags);
void StartEDmac(unsigned ch,int flags);
struct vram_info { uint8_t *vram; int width,pitch,height; };
struct vram_info *get_yuv422_vram(void);
void NotifyBox(int ms,const char *fmt,...);
int raw_lv_is_enabled(void); int fps_get_current_x1000(void); struct card_info *get_ml_card(void);
void powersave_prohibit(void); void powersave_permit(void); void gui_uilock(int l);
int raw2iso(int r);
void AbortEDmac(unsigned ch); int get_global_draw_setting(void);
#define FONT(f,fg,bg) ((f)|((fg)<<8)|((bg)<<16))
#define FONT_MED 1
#define FONT_SMALL 2
#define FONT_LARGE 3
#define COLOR_WHITE 1
#define COLOR_BLACK 2
void bmp_printf(uint32_t font,int x,int y,const char *fmt,...); void clrscr(void);
void canon_gui_disable_front_buffer(void); void canon_gui_enable_front_buffer(int r); void redraw(void);
int lens_set_rawiso(int iso); int lens_set_rawaperture(int ap); void NotifyBoxHide(void);
static inline uint32_t cli(void){ return 0; } static inline void sei(uint32_t x){ (void)x; }
FILE *FIO_CreateFile(const char *p); int FIO_WriteFile(FILE *f,const void *b,int n);
#define O_RDONLY 0
FILE *FIO_OpenFile(const char *p,unsigned mode); int FIO_ReadFile(FILE *f,void *b,int n);
void FIO_CloseFile(FILE *f); int FIO_GetFileSize(const char *p,uint32_t *s);
int FIO_CreateDirectory(const char *p); int is_dir(const char *p);
'''

SIM = r'''
#include <assert.h>
#include <stdarg.h>
#include "stub.h"
#include "camera-profile.h"
#include "fullres-lv.h"
#include "fullres-regs.h"
struct lc_camera_profile livecomp_camera={0,0x12u,0,0x1d,4,5,{0,0,0},0x54,0x6f,0x79}; /* host: no alias bit, no LCD */
struct raw_info raw_info={15000,0x02010100,{6602,10000,-841,10000,-939,10000,-4472,10000,12458,10000,2247,10000,-975,10000,2039,10000,6148,10000},21};
struct lens_info_t lens_info={72,40,22,0,1,5200,1024,1024,1024,40,27,80};
int lv=1,LV_PAUSED,RECORDING,lv_dispsize=1,ml_shutdown_requested,icu_uilock,battery_level_bars=3;
uint32_t raw_write_chan=0x12;
static struct card_info card={"B"};
/* Simulated camera state */
static unsigned now_us,zoom=1,pdm_zoom=1,raw_user,fps_now=29970,next_vsync_us;
static uint32_t reg6800=0x10012,reg6804=0x4540298,reg6014=0x5DC;
static patch_hook_function_cbr hooks[8];static uint32_t sites[8];static int nhooks,patch_fail_at=-1;
static int unpatched,stall_after_zoom,no_window,dma_stuck,single_alloc,locked,partial_frames;
static int csv_written;
static unsigned sim_period_us=333333;
static unsigned vd_since_zoom,canon_abort_vd,canon_aborts,warm_slurps;static int slurp_warm;   /* LCM62 */
static uint8_t *slurp_dst;static int slurp_w,slurp_h,slurp_armed,slurp_frames;
static unsigned allocs,frees,dma_state;
uint32_t sim_mem(uint32_t a)
{
    if (a==0x2998C) return 0xE92D41F0; if (a==0x2986C) return 0xE92D43F8;
    if (a==0xFF2C19AC) return 0xE51FC15C; if (a==0xFF2C1694) return 0xE92D4010;
    if (a==0x1CA60) return 0xE92D4070;                  /* AbortEDmac: STMFD */
    if (a==0x892E8+0x10) return pdm_zoom<<16;
    return 0;
}
int is_movie_mode(void){return 1;} unsigned get_ms_clock(void){return now_us/1000;} unsigned get_us_clock(void){return now_us;}
void sync_caches(void){}
void *fio_malloc(size_t n){ if (n>1000000 && single_alloc && allocs>=1) return NULL; allocs++; return calloc(1,n); }
void fio_free(void *p){ frees++; free(p); }
int patch_hook_function(uintptr_t addr,uint32_t orig,patch_hook_function_cbr cb,const char *d)
{ (void)d; if (nhooks==patch_fail_at) return 1; assert(sim_mem(addr)==orig && nhooks<8); sites[nhooks]=addr; hooks[nhooks++]=cb; return 0; }
int unpatch_memory(uintptr_t addr){ for (int i=0;i<nhooks;i++) if (sites[i]==addr) { unpatched++; return 0; } return 1; }
static patch_hook_function_cbr hook_at(uint32_t a){ for (int i=0;i<nhooks;i++) if (sites[i]==a) return hooks[i]; return 0; }
static uint32_t *pdm_word(void){ static uint32_t w; return &w; }
uint32_t shamem_read(uint32_t a){ if (a==0xC0F06800) return reg6800; if (a==0xC0F06804) return reg6804;
    if (a==0xC0F06014) return reg6014; return 0; }
void EngDrvOut(uint32_t r,uint32_t v){ (void)r;(void)v; }
void EngDrvOutLV(uint32_t r,uint32_t v){ if (r==0xC0F06014) reg6014=v; }
int edmac_get_state(uint32_t ch){ (void)ch; return dma_state; }
static int overrun_sim;
static unsigned last_written;
uint32_t edmac_get_pointer(uint32_t ch){ (void)ch; if (!slurp_dst) return 0;
    return (uint32_t)(uintptr_t)slurp_dst+(slurp_armed ? (overrun_sim ? 9240u:0u):last_written); }
static struct vram_info vram={0,720,1440,480};
static int gui_off,notify_while_hidden;
void NotifyBox(int ms,const char *fmt,...){ (void)ms;(void)fmt; if (gui_off) notify_while_hidden++; }
uint32_t edmac_get_address(uint32_t ch){ (void)ch; return 0; }
struct vram_info *get_yuv422_vram(void){ return &vram; }
static void (*sim_cb)(void*);static void *sim_cb_ctx;
void RegisterEDmacCompleteCBR(int ch,void (*cbr)(void*),void *ctx){ (void)ch; sim_cb=cbr; sim_cb_ctx=ctx; }
void RegisterEDmacAbortCBR(int ch,void (*cbr)(void*),void *ctx){ (void)ch;(void)cbr;(void)ctx; }
void RegisterEDmacPopCBR(int ch,void (*cbr)(void*),void *ctx){ (void)ch;(void)cbr;(void)ctx; }
void ConnectWriteEDmac(unsigned ch,unsigned where){ (void)ch;(void)where; }
static uint8_t *pending_dst;static int pending_w,pending_h;
void SetEDmac(unsigned ch,void *addr,struct edmac_info *info,int flags){ (void)ch;(void)flags; pending_dst=addr; pending_w=info->xb; pending_h=info->yb+1; }
void StartEDmac(unsigned ch,int flags){ (void)ch;(void)flags; slurp_warm=zoom==5 && reg6014==0x1E03; slurp_dst=pending_dst; slurp_w=pending_w; slurp_h=pending_h; slurp_armed=1; }
int raw_lv_is_enabled(void){ return raw_user; } int fps_get_current_x1000(void){ return fps_now; }
struct card_info *get_ml_card(void){ return &card; }
void powersave_prohibit(void){} void powersave_permit(void){} void gui_uilock(int l){ locked=l; }
int raw2iso(int r){ return r; }
static int hud_draws,hud_clears;static char hud_text[128];
static int cover_seen,ending_seen,skip_dark_press,tactical_seen;static volatile unsigned *press_ptr;
void bmp_printf(uint32_t font,int x,int y,const char *fmt,...){ (void)font;(void)x;
  { static char t[128];va_list ap;va_start(ap,fmt);vsnprintf(t,sizeof(t),fmt,ap);va_end(ap);   /* session line (y=20) */
    if (strstr(t,"COVER THE LENS")) { cover_seen++; if (skip_dark_press && press_ptr && cover_seen==3) (*press_ptr)++; }
    if (strstr(t,"Ending the current frame")) ending_seen++;
    if (strstr(t,"Tactical delay... first frame in")) tactical_seen++; }
  if (y<20) { va_list ap;va_start(ap,fmt);vsnprintf(hud_text,sizeof(hud_text),fmt,ap);va_end(ap);hud_draws++; } }
void clrscr(void){ hud_clears++; }
static unsigned gui_offs;
void canon_gui_disable_front_buffer(void){ gui_off=1;gui_offs++; } void canon_gui_enable_front_buffer(int r){ (void)r; gui_off=0; } void redraw(void){}
int lens_set_rawiso(int iso){ lens_info.raw_iso=iso; return 1; }
int lens_set_rawaperture(int ap){ lens_info.raw_aperture=ap; lens_info.aperture=ap; return 1; }
static int aborts,gd_setting,abort_fails;
void AbortEDmac(unsigned ch){ (void)ch; aborts++; if (abort_fails) return; dma_state=0; slurp_armed=0; }
int get_global_draw_setting(void){ return gd_setting; }
static int file_token; static unsigned dng_bytes,log_files,dng_files;
int is_camera(const char *m,const char *f){ return !strcmp(m,"EOSM") && !strcmp(f,"2.0.2"); }
static int dng_created,cur_dng,cur_log;static char last_log[8192],last_log_path[64];static unsigned last_log_n;
static int dirs_created,dir_fail,dng_in_live,log_in_live;static char last_dir[64];
static uint8_t *lvbr_data;static unsigned lvbr_n,lvbr_pos,lvbr_files,lvdk_files;static int cur_lvbr,lvbr_reads;
#define LVBR_MAX 40000000u
FILE *FIO_OpenFile(const char *p,unsigned mode){ (void)mode; if (!(strstr(p,"LVBR") || strstr(p,"LVCR")) || !lvbr_n) return 0; lvbr_pos=0;lvbr_reads++; return (FILE *)&file_token; }
int FIO_ReadFile(FILE *f,void *b,int n){ (void)f; unsigned k=lvbr_n-lvbr_pos<(unsigned)n ? lvbr_n-lvbr_pos:(unsigned)n; memcpy(b,lvbr_data+lvbr_pos,k);lvbr_pos+=k;return (int)k; }
FILE *FIO_CreateFile(const char *p){ cur_lvbr=strstr(p,"LVBR") || strstr(p,"LVCR"); if (cur_lvbr) { lvbr_n=0;lvbr_files++; if (!lvbr_data) lvbr_data=malloc(LVBR_MAX); } if (strstr(p,"LVDK") || strstr(p,"LVCK")) lvdk_files++; if (strstr(p,".DNG") && !strncmp(p,"B:/DCIM/LIVE/LVFR",17)) dng_in_live=1; if (strstr(p,".LOG") && !strncmp(p,"B:/DCIM/LIVE/",13)) log_in_live=1; if (strstr(p,".CSV")) csv_written=1; cur_log=!!strstr(p,".LOG"); if (cur_log) { log_files++;last_log_n=0;last_log[0]=0;snprintf(last_log_path,sizeof(last_log_path),"%s",p); } cur_dng=!!strstr(p,".DNG"); if (cur_dng) { dng_created=1;dng_files++; } return (FILE *)&file_token; }
int FIO_WriteFile(FILE *f,const void *b,int n){ (void)f; if (cur_dng) dng_bytes+=n; if (cur_lvbr && lvbr_n+n<=LVBR_MAX) { memcpy(lvbr_data+lvbr_n,b,n);lvbr_n+=n; }
  if (cur_log && last_log_n+n<sizeof(last_log)) { memcpy(last_log+last_log_n,b,n);last_log_n+=n;last_log[last_log_n]=0; } return n; }
void FIO_CloseFile(FILE *f){ (void)f; }
static unsigned dng_size_ok;
int FIO_CreateDirectory(const char *p){ dirs_created++;snprintf(last_dir,sizeof(last_dir),"%s",p);return dir_fail ? -1:0; }
int is_dir(const char *p){ return !dir_fail && !strcmp(p,"B:/DCIM/LIVE"); }
int FIO_GetFileSize(const char *p,uint32_t *s){ if (strstr(p,".DNG") && dng_created) { *s=dng_size_ok; return 0; } return 1; }
/* Canon-like reconfiguration through the installed hooks. */
static void canon_reconfigure(unsigned z)
{
    pdm_zoom=z;
    static uint16_t cmos[4];static uint32_t adtg[3],engio[7];
    const uint16_t c0[]={0x1123,0x5280,0x7aa9,0xFFFF};memcpy(cmos,c0,sizeof(c0));
    const uint32_t a0[]={0x805F0100,0x81720000,0xFFFFFFFF};memcpy(adtg,a0,sizeof(a0));
    const uint32_t e0[]={0xC0F06800,0x10012,0xC0F06804,z==5 ? 0x4540298u:0x2D80298u,0xC0F06014,0x5DC,0xFFFFFFFF};
    memcpy(engio,e0,sizeof(e0));
    uint32_t regs[4]={0};
    patch_hook_function_cbr h;
    if ((h=hook_at(0x2998C))) { regs[0]=(uint32_t)(uintptr_t)cmos; h(regs,0,0); }
    if ((h=hook_at(0x2986C))) { regs[0]=6;regs[1]=(uint32_t)(uintptr_t)adtg; h(regs,0,0); }
    if ((h=hook_at(0xFF2C19AC))) { regs[0]=(uint32_t)(uintptr_t)engio; h(regs,0,0); }
    reg6800=engio[1]; reg6804=no_window ? 0x4540298u:engio[3]; reg6014=engio[5];
}
static unsigned zoom5_us,first_frame_us;
static uint32_t exit_timer_b;static unsigned cancel_at_us;
void set_lv_zoom(int z){ if (z==5) vd_since_zoom=0; if (z==1) exit_timer_b=reg6014; zoom=z; lv_dispsize=z; canon_reconfigure(z); if (z==5) { zoom5_us=now_us;first_frame_us=0; } }
/* Pointers in this simulation are host addresses; keep the alias bit clear. */
static unsigned cancel_at,battery_at,lose_mode_at,hold_cancel_vsyncs,hold_vsyncs;static volatile unsigned *cancel_ptr;
static void frame(void)
{
    if (cancel_at && cancel_ptr && slurp_frames>=(int)cancel_at) *cancel_ptr=1;
    if (cancel_at_us && cancel_ptr && now_us>=cancel_at_us && !*cancel_ptr) *cancel_ptr=1;
    /* Preview only: SET some vsyncs after the frame was captured. */
    if (hold_cancel_vsyncs && slurp_frames>=1 && !slurp_armed && zoom==5 && ++hold_vsyncs>=hold_cancel_vsyncs && cancel_ptr) *cancel_ptr=1;
    if (battery_at && slurp_frames>=(int)battery_at) battery_level_bars=0;
    /* Canon leaves the full-res timing: window back to the x1 value, the
     * armed 32 MB slurp never completes (until aborted). */
    if (lose_mode_at && slurp_frames>=(int)lose_mode_at) { reg6804=0x2D80298;dma_stuck=1; }
    if (stall_after_zoom && zoom==5) return;
    /* LCM62: Canon aborts channel 0x12 at the start of the readout of the
     * frame ending at VD canon_abort_vd after x5 (6 rows written). */
    if (zoom==5) vd_since_zoom++;
    if (canon_abort_vd && vd_since_zoom==canon_abort_vd && slurp_armed) {
        patch_hook_function_cbr h=hook_at(0x1CA60);
        uint32_t regs[14]={0};regs[0]=0x12;regs[13]=0xFF0ABCDE;
        if (h) h(regs,0,0);
        memset(slurp_dst,0x22,1000);last_written=55440;canon_aborts++;
        slurp_armed=0;dma_state=0;livecomp_frlv_vsync();return;
    }
    if (slurp_armed && partial_frames && !slurp_warm) { partial_frames--; memset(slurp_dst,0x22,1000); last_written=1000; }
    else if (slurp_armed && !dma_stuck) {
        last_written=slurp_w*slurp_h;
        for (int i=0;i<slurp_w*slurp_h;i+=4096) slurp_dst[i]=(uint8_t)(slurp_frames*7+i);
        memset(slurp_dst+slurp_w*slurp_h-8,0x11,8);
        /* LCM62: warm-up slurps (armed at the 3 fps entry timing) counted apart */
        if (slurp_warm) warm_slurps++;
        else { slurp_frames++; if (slurp_frames==1) first_frame_us=now_us; }
        if (sim_cb) sim_cb(sim_cb_ctx);
    }
    dma_state=dma_stuck && (slurp_armed || dma_state);
    slurp_armed=0;
    livecomp_frlv_vsync();
}
static unsigned sim_slow=1;   /* scales simulated CPU time (msleep) */
/* x5 runs at the 3 fps entry timing (timer B 0x1E03) until the switch.
 * Timers written in the vsync callback apply to the frame that starts there
 * (sim_late_latch: only to the one after it). */
static int sim_late_latch;
static unsigned sim_period(void){ return zoom==5 ? (reg6014==0x1E03 ? 333333u:sim_period_us):33333u; }
void msleep(int ms)
{
    unsigned end=now_us+ms*1000*sim_slow;
    while (next_vsync_us<=end) {
        unsigned before=sim_period();
        now_us=next_vsync_us; frame();
        next_vsync_us+=sim_late_latch ? before:sim_period();
    }
    now_us=end;
}
static void reset(void)
{
    now_us=next_vsync_us=0;zoom=pdm_zoom=1;lv_dispsize=1;raw_user=0;fps_now=29970;
    reg6800=0x10012;reg6804=0x4540298;nhooks=0;patch_fail_at=-1;unpatched=0;stall_after_zoom=0;
    no_window=0;dma_stuck=0;partial_frames=0;overrun_sim=0;csv_written=0;single_alloc=0;slurp_armed=0;slurp_frames=0;allocs=frees=0;dma_state=0;
    dng_bytes=0;dng_size_ok=0;locked=0;dng_created=0;
    dirs_created=dir_fail=dng_in_live=log_in_live=0;log_files=0;
    cover_seen=ending_seen=skip_dark_press=tactical_seen=0;exit_timer_b=0;cancel_at_us=0;dng_files=0;
    lvbr_n=lvbr_files=lvdk_files=0;lvbr_reads=0;canon_abort_vd=0;canon_aborts=0;warm_slurps=0;
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    volatile unsigned cancel=0;const char *status="";
    struct lc_frlv_options o={0,6,0,1,LC_MODE_COMPOSITE,0,0,0,0};   /* 2.5 s shutter, stop after 6 frames, Debug ON, composite */
    sim_period_us=2500000;
    /* Happy path: composite of 6 frames, geometry from registers, restore,
     * unhook, DNG + LOG + CSV, every buffer freed. */
    reset();
    unsigned w=(0x538-0x12)*4,h=0xDB2-1+1,bytes=w*14/8*h;
    dng_size_ok=2048+bytes;
    unsigned unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("happy: %s unsafe=%u frames=%d unpatched=%d zoom=%u\n",status,unsafe,slurp_frames,unpatched,zoom);
    assert(!unsafe && slurp_frames>=6 && unpatched==5 && zoom==1 && lv_dispsize==1);
    assert(slurp_w==(int)(w*14/8) && slurp_h==(int)h && allocs==frees);
    assert(strstr(status,"saved: 6 frames") && dng_bytes==2048+bytes && locked==0 && csv_written);
    assert(!strstr(last_log,"HW_"));               /* no engine code in the product */
    /* Output folder DCIM/LIVE (created), LOG next to the DNG with Debug. */
    assert(dirs_created==2 && dng_in_live && log_in_live && strstr(last_log,"DNG: B:/DCIM/LIVE/LVFR0000.DNG"));
    /* Fast start (LCM50): x5 entered at 3 fps, shutter switched in at a
     * vsync, so the first usable frame is done one shutter + ~1 s after the
     * zoom; the exit switch back to 3 fps is logged; Canon's zoom overlay
     * hidden during the session and restored. */
    printf("first frame %u ms after zoom\n",(first_frame_us-zoom5_us)/1000);
    assert(first_frame_us-zoom5_us<2500000u+1200000u+12u*333333u);   /* LCM62: + 12 warm-up frames */
    assert(strstr(last_log,"Timer_switch_after_zoom_ms_exit_switched_short_skipped: "));
    { const char *k=strstr(last_log,"exit_switched_short_skipped: ");unsigned sw,ex,sk;
      assert(sscanf(k,"exit_switched_short_skipped: %u,%u,%u",&sw,&ex,&sk)==3 && ex==1 && sw<1500+12*333);   /* LCM62: after the 12-frame warm-up */printf("short frames skipped: %u\n",sk); }
    assert(gui_offs==1 && gui_off==0 && !notify_while_hidden);   /* NotifyBox redraws Canon's zoom box */
    /* Start-up: armed right after the transitional frame (no full period
     * lost waiting for the next vsync), i.e. 40-100 ms after a vsync. */
    { const char *k=strstr(last_log,"first_arm_ms: ");unsigned po,cl,ml,da,fa;
      assert(k && sscanf(k,"first_arm_ms: %u,%u,%u,%u,%u",&po,&cl,&ml,&da,&fa)==5);
      printf("first arm %u ms after vsync\n",fa);assert(fa>=40 && fa<100); }
    /* The 2.5 s timers (A 0x56B, B 57635 = 0xE123) are what the hooks apply. */
    assert(strstr(last_log,"Shutter_ms_timerA_timerB_blanking: 2500,56b,e123,"));
    /* Live Bulb: the same session with the add kernel. Own file prefix
     * (LVBL), exposure = frames x shutter in the LOG/DNG and the status. */
    reset();o.mode=LC_MODE_BULB;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("bulb: %s\n",status);
    assert(!unsafe && strstr(status,"Bulb saved: 6 frames, 15 s") && dng_bytes==2048+bytes && allocs==frees &&
           zoom==1 && unpatched==5 && locked==0);
    assert(strstr(last_log,"Full-res live bulb") && strstr(last_log,"DNG: B:/DCIM/LIVE/LVBL0000.DNG") &&
           strstr(last_log,"Mode_total_exposure_ms_acc_black_too_slow: bulb,15000,") && !strstr(status,"longer frames") && strstr(last_log,"strips: add,"));
    /* Live Bulb with partial frames: only the delivered rows are added. */
    reset();partial_frames=2;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("bulb partial: %s\n",status);
    assert(!unsafe && strstr(status,"saved") && zoom==1);
    /* LCM56: partial frames are not added in Live Bulb; the count stays exact. */
    assert(strstr(last_log,"Column_fix_rms_x10_us_bulb_partials_skipped: ") && strstr(last_log,",2\nColumn_fix_img") &&
           strstr(status,"Bulb saved: 6 frames, 15 s"));
    /* Live Bulb, no limit, SET at frame 9; and every shutter choice. */
    reset();o.limit=0;cancel_at=9;cancel_ptr=&cancel;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("bulb no limit: %s\n",status);
    assert(!unsafe && strstr(status,"Bulb saved") && zoom==1);
    cancel=0;cancel_at=0;o.limit=6;
    /* LCM62: Canon's abort of channel 0x12 where the camera does it (the
     * frame ending at VD 8 after x5) falls on a throwaway warm-up frame:
     * no partial, all 6 frames whole; the trace has Canon's call. */
    reset();canon_abort_vd=8;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("Canon abort at VD 8: %s (Canon aborts %u, tactical delay lines %d)\n",status,canon_aborts,tactical_seen);
    assert(tactical_seen>=3);                          /* LCM63: the warm-up says what it is doing */
    assert(!unsafe && canon_aborts==1 && strstr(status,"Bulb saved: 6 frames, 15 s") && zoom==1 && allocs==frees && unpatched==5);
    assert(strstr(last_log,"Column_fix_rms_x10_us_bulb_partials_skipped: ") && strstr(last_log,",0\nColumn_fix_img"));
    { const char *k=strstr(last_log,"Abort_hook_instr_hooked_calls");assert(k && strstr(k,"e92d4070,1,1 7:ff0abcde:") && strstr(k,":0:1\n"));
      k=strstr(last_log,"Warmup_vds_slurps_done_ms_aborts: 12,");unsigned vds,sl,dn,ms,ab;
      assert(k && sscanf(strchr(k,':')+1," %u,%u,%u,%u,%u",&vds,&sl,&dn,&ms,&ab)==5 && ab==1 && sl>=8);
      printf("warm-up: %u slurps, %u ms\n",sl,ms); }
    /* If Canon aborted later (after the warm-up) the trace shows it and the
     * bulb skips that frame as before. */
    reset();canon_abort_vd=12+4;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("Canon abort at VD 16: %s\n",status);
    assert(!unsafe && canon_aborts==1 && strstr(status,"Bulb saved: 6 frames") && zoom==1 && allocs==frees);
    assert(strstr(last_log,"_partials_skipped: ") && strstr(last_log,",1\nColumn_fix_img") && strstr(last_log,",1,1 15:ff0abcde:"));
    /* LCM59 dark frames: countdown, as many frames as the bulb at the session
     * shutter, each subtracted as it arrives; back to 3 fps before leaving
     * x5. (The kernels' exactness is checked above; the simulated frames
     * are not dark, so only the flow is checked here.) */
    reset();o.dark_frame=1;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("bulb + dark: %s (countdown lines %d, DNGs %u)\n",status,cover_seen,dng_files);
    assert(!unsafe && strstr(status,"Bulb saved: 6 frames, 15 s, dark subtracted") && zoom==1 && allocs==frees && unpatched==5 && locked==0);
    assert(cover_seen>=9 && dng_files==1 && lvbr_files==0 && lvdk_files==0 && exit_timer_b==0x1E03);
    { const char *k=strstr(last_log,"Dark_want_");unsigned v[8];
      assert(k && sscanf(strchr(k,':')+1," %u,%u,%u,%u,%u,%u,%u,%u",&v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7])==8);
      printf("dark: state %u, %u frames, %u ms from the last light frame\n",v[2],v[3],v[6]);
      assert(v[0]==1 && v[1]==0 && v[2]==3 && v[3]==6 && v[4]==0 && v[5]==0 && v[7]==0);
      assert(v[6]>=10000+6*2500 && v[6]<=10000+8*2500+3000); }
    /* Save unprocessed + dark: LVBR first, darks summed apart (LVDK), LVBR
     * read back and the dark sum subtracted: three DNGs. */
    reset();o.save_orig=1;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("bulb + dark + unprocessed: %s (DNGs %u)\n",status,dng_files);
    assert(!unsafe && strstr(status,"Bulb saved: 6 frames, 15 s, dark subtracted") && zoom==1 && allocs==frees);
    assert(dng_files==3 && lvbr_files==1 && lvdk_files==1 && lvbr_reads==1 && lvbr_n==2048+bytes);
    assert(strstr(last_log,"_end_wait_ms: 1,1,3,6,0,0,") && strstr(last_log,",1,1,1,1,"));   /* sep, saved, saved, read */
    /* Save unprocessed without dark frames: LVBR + LVBL. */
    reset();o.dark_frame=0;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("bulb + unprocessed: %s (DNGs %u)\n",status,dng_files);
    assert(!unsafe && strstr(status,"Bulb saved: 6 frames, 15 s") && dng_files==2 && lvbr_files==1 && lvdk_files==0 && allocs==frees);
    o.save_orig=0;o.dark_frame=1;
    /* SET during the countdown: no dark frames, the bulb is saved as usual. */
    reset();skip_dark_press=1;press_ptr=&cancel;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("bulb + dark skipped: %s\n",status);
    assert(!unsafe && strstr(status,"Bulb saved: 6 frames, 15 s") && !strstr(status,"dark") && zoom==1 && allocs==frees);
    assert(dng_files==1 && exit_timer_b==0x1E03 && strstr(last_log,"_end_wait_ms: 1,0,1,0,"));
    cancel=0;press_ptr=0;o.dark_frame=0;
    /* LVBL0014: SET before the first full 60 s frame (the camera latches
     * timers one frame late: the first frame is short and skipped). LCM59:
     * that is a single-frame bulb; then wait for the late-latched long
     * frame before leaving x5 (LCM57: FE_RESTORE; LCM58: no frame). */
    reset();o.limit=0;o.fps_index=LC_FRR_FPS_CHOICES-1;sim_period_us=lc_frr_timing[o.fps_index].period_ms*1000u;
    sim_late_latch=1;cancel_at_us=500000;cancel_ptr=&cancel;dng_size_ok=2048+bytes;   /* SET at the 667 ms vsync */
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("SET before the first 60 s frame: %s (then 3 fps: timer B %x)\n",status,exit_timer_b);
    assert(!unsafe && zoom==1 && allocs==frees && unpatched==5 && exit_timer_b==0x1E03);
    assert(strstr(status,"Bulb saved: 1 frames, 60 s") && strstr(last_log,"Stage_error_unsafe: 8,0,0"));
    cancel=0;cancel_at_us=0;sim_late_latch=0;o.limit=6;o.fps_index=0;sim_period_us=2500000;
    o.mode=LC_MODE_COMPOSITE;
    /* LCM61: Live Composite + dark frame: ONE dark frame (a maximum holds
     * each pixel's offsets once), subtracted at the end; then the column
     * correction. With Save unprocessed: LVCR + LVCK + LVFR. */
    reset();o.dark_frame=1;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("composite + dark: %s (DNGs %u)\n",status,dng_files);
    assert(!unsafe && strstr(status,"Composite saved: 6 frames, dark subtracted") && zoom==1 && allocs==frees && unpatched==5);
    assert(cover_seen>=9 && dng_files==1 && exit_timer_b==0x1E03 && strstr(last_log,"_end_wait_ms: 1,0,3,1,0,0,"));
    reset();o.save_orig=1;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("composite + dark + unprocessed: %s (DNGs %u)\n",status,dng_files);
    assert(!unsafe && strstr(status,"Composite saved: 6 frames, dark subtracted") && dng_files==3 && lvbr_files==1 && lvdk_files==1 && lvbr_reads==1 && allocs==frees);
    /* Composite + Save unprocessed without dark: nothing to save apart. */
    reset();o.dark_frame=0;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && strstr(status,"Composite saved: 6 frames") && dng_files==1 && lvbr_files==0);
    o.save_orig=0;
    /* Composite again: back to LVFR and the select kernel. */
    reset();dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && strstr(status,"Composite saved: 6 frames") && strstr(last_log,"Mode_total_exposure_ms_acc_black_too_slow: composite,2500,0,0"));
    /* Debug OFF: the same session writes the DNG only, no LOG and no CSV. */
    reset();o.debug=0;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("debug off: %s logs=%d csv=%d\n",status,log_files,csv_written);
    assert(!unsafe && strstr(status,"saved: 6 frames") && dng_in_live && log_files==0 && !csv_written);
    /* ...and Preview only with Debug OFF writes nothing at all. */
    reset();o.preview_only=1;o.limit=0;hold_cancel_vsyncs=3;hold_vsyncs=0;cancel_ptr=&cancel;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && strstr(status,"Preview: clipped") && log_files==0 && !dng_created && !csv_written);
    o.preview_only=0;o.limit=6;cancel=0;hold_cancel_vsyncs=0;
    /* The folder cannot be created: refused before any hook, nothing kept. */
    reset();dir_fail=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && nhooks==0 && allocs==0 && strstr(status,"DCIM/LIVE"));
    o.debug=1;
    /* Refusals before any hook: FPS override on, other RAW user. */
    reset();fps_now=3000;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && nhooks==0 && strstr(status,"FPS"));
    reset();raw_user=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && nhooks==0 && allocs==0);
    /* Only one full-size buffer: refused before any hook, nothing kept. */
    reset();single_alloc=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && nhooks==0 && frees==allocs && strstr(status,"memory"));
    /* Hook failure: earlier hooks removed, never zoomed. */
    reset();patch_fail_at=2;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && unpatched==2 && zoom==1 && strstr(status,"Hook"));
    /* Window not applied: restore and unhook, no capture. */
    reset();no_window=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && slurp_frames==0 && zoom==1 && unpatched==5);
    /* LV stall after switch: restore, unhook, nothing captured. */
    reset();stall_after_zoom=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && slurp_frames==0 && zoom==1 && unpatched==5);
    /* DMA never completes but AbortEDmac stops it: safe, buffers freed. */
    reset();dma_stuck=1;aborts=0;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("stuck, aborted: %s unsafe=%u aborts=%d\n",status,unsafe,aborts);
    assert(!unsafe && aborts>=1 && zoom==1 && frees==allocs && strstr(status,"Frame lost"));
    /* ...and if even the abort fails: ownership uncertain, buffers retained. */
    reset();dma_stuck=1;abort_fails=1;unsigned a0=allocs;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("stuck: %s unsafe=%u allocs=%u frees=%u\n",status,unsafe,allocs,frees);
    assert(unsafe && zoom==1 && frees<allocs && a0==0 && strstr(status,"reboot"));abort_fails=0;
    /* Global Draw on: refused before any hook. */
    reset();gd_setting=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && nhooks==0 && strstr(status,"Global Draw"));gd_setting=0;
    /* Every sub-exposure choice runs; slower ones have longer timeouts. */
    for (unsigned i=0;i<LC_FRR_FPS_CHOICES;i++) {
        reset();o.fps_index=i;sim_period_us=lc_frr_timing[i].period_ms*1000u;dng_size_ok=2048+bytes;
        unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
        printf("sub-exposure %u: %s\n",i,status);
        assert(!unsafe && strstr(status,"saved: 6 frames") && zoom==1);
    }
    o.fps_index=0;sim_period_us=2500000;
    /* No frame limit: runs until SET (simulated at frame 9), then saves. */
    reset();o.limit=0;cancel_at=9;cancel_ptr=&cancel;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("no limit: %s frames=%d\n",status,slurp_frames);
    assert(!unsafe && strstr(status,"saved") && slurp_frames>=9 && zoom==1);
    cancel=0;cancel_at=0;o.limit=6;
    /* Two partial frames: merged and counted, not fatal. */
    reset();partial_frames=2;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && strstr(status,"saved"));
    /* Merge overrun (next readout already writing): keep the composite. */
    reset();overrun_sim=1;dng_size_ok=2048+bytes;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("overrun: %s\n",status);
    assert(!unsafe && strstr(status,"saved") && zoom==1);
    /* LCM55: the same overrun in Live Bulb stops the session (a lost band
     * would leave part of the image with fewer frames) and says why. */
    {   const unsigned old_mode=o.mode;
        reset();overrun_sim=1;o.mode=LC_MODE_BULB;dng_size_ok=2048+bytes;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
        printf("bulb overrun: %s\n",status);
        assert(!unsafe && strstr(status,"Bulb saved:") && strstr(status,"use longer frames") && zoom==1 && unpatched==5 &&
               strstr(last_log,"Mode_total_exposure_ms_acc_black_too_slow: bulb,") && strstr(last_log,",1\n"));
        o.mode=old_mode; }
    /* Frames arrive faster than the (simulated 80 ms) merge: the session
     * keeps going (behind/skipped frames counted) and saves. */
    reset();sim_period_us=2500000;sim_slow=40;o.limit=8;dng_size_ok=2048+bytes;   /* merge ~3.2 s > 2.5 s frames */
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    { const char *k=strstr(last_log,"preview_skipped_behind_strips: select,");unsigned bands=0,skip=0,behind=0;
      assert(k && sscanf(k,"preview_skipped_behind_strips: select,%u,%u,%u",&bands,&skip,&behind)==3);
      printf("behind: %s (behind=%u skipped_previews=%u)\n",status,behind,skip);
      assert(!unsafe && strstr(status,"saved") && zoom==1 && behind>0); }
    sim_slow=1;
    /* Slow merge at 2.5 s: a full preview does not fit, a quarter strip does. */
    reset();sim_period_us=2500000;o.fps_index=0;sim_slow=25;o.limit=8;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    { const char *k=strstr(last_log,"preview_skipped_behind_strips: select,");unsigned bands=0,skip=0,behind=0,strips=0;
      assert(k && sscanf(k,"preview_skipped_behind_strips: select,%u,%u,%u,%u",&bands,&skip,&behind,&strips)==4);
      printf("strips: %s (strips=%u skipped=%u behind=%u)\n",status,strips,skip,behind);
      assert(!unsafe && strstr(status,"saved") && strips>=2); }
    sim_slow=1;o.fps_index=0;sim_period_us=2500000;o.limit=6;
    /* Preview only: exactly one frame captured, shown until SET (simulated
     * a few vsyncs later), nothing saved (no DNG, no CSV), clipped share in
     * the status; the next session numbers are not consumed (LVPREVW.LOG). */
    reset();o.preview_only=1;o.limit=0;hold_cancel_vsyncs=3;hold_vsyncs=0;cancel_ptr=&cancel;hud_draws=0;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("preview only: %s frames=%d hud='%s'\n",status,slurp_frames,hud_text);
    assert(!unsafe && slurp_frames==1 && !dng_created && !csv_written && strstr(status,"Preview: clipped")
           && hud_draws>0 && hud_clears>0 && zoom==1 && unpatched==5 && frees==allocs && strstr(last_log_path,"LVPREVW.LOG"));
    o.preview_only=0;o.limit=6;cancel=0;hold_cancel_vsyncs=0;
    /* Late-latching timers: the first frame after the switch is a short
     * 3 fps one; it is detected at the vsyncs and skipped, the composite
     * still starts with a full frame and leaves x5 promptly. */
    reset();sim_late_latch=1;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    { const char *k=strstr(last_log,"exit_switched_short_skipped: ");unsigned sw,ex,sk;
      assert(k && sscanf(k,"exit_switched_short_skipped: %u,%u,%u",&sw,&ex,&sk)==3);
      printf("late latch: %s, short skipped %u\n",status,sk);
      assert(!unsafe && strstr(status,"saved: 6 frames") && sk==1 && ex==1); }
    sim_late_latch=0;
    /* A composite with a limit captures no extra frame after the last one. */
    reset();o.limit=3;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && slurp_frames==3 && strstr(status,"saved: 3 frames"));
    o.limit=6;
    /* Battery reaches its last level: the session stops and saves. */
    reset();o.limit=0;battery_at=5;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("battery: %s\n",status);
    assert(!unsafe && strstr(status,"Battery low: saved") && strstr(last_log,"battery_start_end_stop: 72,40,3,0,1"));
    battery_at=0;battery_level_bars=3;o.limit=6;
    /* Canon leaves the mode mid-session (LCM43 second run): the in-flight
     * frame is aborted, the composite so far is saved, no reboot needed. */
    reset();o.limit=0;lose_mode_at=4;aborts=0;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("mode lost: %s (aborts %d)\n",status,aborts);
    assert(!unsafe && aborts>=1 && strstr(status,"stopped early") && dng_bytes==2048+bytes && frees==allocs);
    assert(strstr(last_log,"mode_lost_dma_aborts_first_arm_ms: ") && strstr(last_log,",1,"));
    lose_mode_at=0;o.limit=6;
    printf("sim ok\n");return 0;
}
'''


def run(src, name, tmp, extra=()):
    c = tmp / (name + '.c')
    c.write_text(src)
    exe = tmp / name
    subprocess.run(['cc', '-std=gnu99', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-pointer-to-int-cast', '-Wno-misleading-indentation', '-Wno-int-to-pointer-cast', '-Wno-unused-parameter',
                    '-I' + str(tmp), '-I' + str(MOD), *extra, str(c), '-o', str(exe)], check=True)
    p = subprocess.run([str(exe)], capture_output=True, text=True)
    print(p.stdout.strip())
    if p.returncode:
        print(p.stderr.strip())
        raise SystemExit(name + ' failed')


def main():
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        run(REGS_TEST, 'regs', tmp)
        (tmp / 'stub.h').write_text(STUB)
        for h in ('dryos.h', 'module.h', 'lens.h', 'raw.h', 'mem.h', 'patch.h', 'edmac.h', 'fps.h',
                  'shoot.h', 'propvalues.h', 'gui-common.h', 'powersave.h', 'vram.h', 'notify_box.h', 'zebra.h', 'bmp.h'):
            (tmp / h).write_text('#include "stub.h"\n')
        src = (MOD / 'fullres-lv.c').read_text()
        # 32-bit Canon pointers: on the 64-bit host keep addresses as uintptr_t.
        src = src.replace('(unsigned)p&~livecomp_camera.cache_alias_mask',
                          '(uintptr_t)p&~(uintptr_t)livecomp_camera.cache_alias_mask')
        src = src.replace('static unsigned physical(', 'static uintptr_t physical(')
        src = src.replace('regs[0]=(uint32_t)cmos_copy', 'regs[0]=(uint32_t)(uintptr_t)cmos_copy')
        src = src.replace('regs[1]=(uint32_t)adtg_copy', 'regs[1]=(uint32_t)(uintptr_t)adtg_copy')
        src = src.replace('#define FRLV_ALLOCATION 32488032u', '#define FRLV_ALLOCATION 32488032u\n#define LIVECOMP_BUILD_ID "LCM15 host"')
        (tmp / 'fullres-lv-host.c').write_text(src)
        sim = SIM.replace('static uint32_t *pdm_word(void){ static uint32_t w; return &w; }\n', '')
        # Hook list pointers are stored in 32-bit regs on the camera; on the host
        # the simulated lists live in a low static arena so they fit.
        run(sim, 'sim', tmp, [str(tmp / 'fullres-lv-host.c'), '-no-pie'])
    print('PASS: full-res register rewriting matches the reference preset; simulated gate '
          'switches, captures, merges, saves, restores and unhooks; refusals and DMA quarantine')


if __name__ == '__main__':
    main()
