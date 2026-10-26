#!/usr/bin/env python3
"""Host checks for the LCM15 full-sensor readout gate (not hardware evidence).

1. fullres-regs.h: reference register values, NRZI, blanking, list rewriting.
2. fullres-lv.c compiled against stub headers and a simulated camera:
   hooks, x5 switch, our RAW slurp, merge, DNG, restore and fault paths.
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
int main(void) {
    { /* sparse max == reference max, every sample */
        enum { B=20000 }; static uint16_t a[7*B],b[7*B],c[7*B];
        srand(7);
        for (int round=0;round<4;round++) {
            for (int i=0;i<7*B;i++) { b[i]=rand(); if (!round) a[i]=c[i]=rand(); }
            if (round==3) memcpy(b,a,sizeof(a)); /* nothing brighter */
            lc_raw14_max(c,b,B); unsigned ch=lc_raw14_max_sparse(a,b,B);
            assert(!memcmp(a,c,sizeof(a)) && (round<3 || ch==0));
        }
    }
    { /* LCM24 select max == reference max, every sample, incl. ties/edges */
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
    lc_frr_configure(&c,5,0);   /* 3 fps entry */
    /* Reference EOS M CROP_Full_Res constants (crop_rec 5f0cda8). */
    assert(c.raw_h==0x538 && c.raw_v==0xDB2 && c.timer_a==0x56B && c.timer_b==0x1E03);
    /* fps = 32 MHz / ((A+1)(B+1)) */
    for (unsigned i=0;i<LC_FRR_FPS_CHOICES;i++) {
        const struct lc_frr_timing *t=&lc_frr_timing[i];
        double fps=32e6/((double)(t->timer_a+1)*(t->timer_b+1));
        double want=t->fps_x1000/1000.0;
        assert(fps>want*0.998 && fps<want*1.002 && t->timer_b<=0xFFFF && t->timer_a>=LC_FRR_TIMER_A);
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
struct lens_info_t { int raw_iso,aperture,focal_len,job_state,wb_mode,kelvin; unsigned WBGain_R,WBGain_G,WBGain_B; };
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
uint32_t edmac_get_address(uint32_t ch); uint32_t edmac_get_length(uint32_t ch); uint32_t edmac_get_flags(uint32_t ch); uint32_t edmac_get_connection(uint32_t ch,uint32_t dir);
struct edmac_info { unsigned off1a,off1b,off2a,off2b,off3,xa,xb,ya,yb,xn,yn; };
#define EDMAC_2_BYTES_PER_TRANSFER 0x20000000
#define EDMAC_DIR_WRITE 1
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
FILE *FIO_CreateFile(const char *p); int FIO_WriteFile(FILE *f,const void *b,int n);
void FIO_CloseFile(FILE *f); int FIO_GetFileSize(const char *p,uint32_t *s);
'''

SIM = r'''
#include <assert.h>
#include "stub.h"
#include "camera-profile.h"
#include "fullres-lv.h"
struct lc_camera_profile livecomp_camera={0,0x12u,0,0x1d,4,5,{0,0,0}}; /* host: no alias bit, no LCD */
struct raw_info raw_info={15000,0x02010100,{6602,10000,-841,10000,-939,10000,-4472,10000,12458,10000,2247,10000,-975,10000,2039,10000,6148,10000},21};
struct lens_info_t lens_info={72,40,22,0,1,5200,1024,1024,1024};
int lv=1,LV_PAUSED,RECORDING,lv_dispsize=1,ml_shutdown_requested,icu_uilock;
uint32_t raw_write_chan=0x12;
static struct card_info card={"B"};
/* Simulated camera state */
static unsigned now_us,zoom=1,pdm_zoom=1,raw_user,fps_now=29970,next_vsync_us;
static uint32_t reg6800=0x10012,reg6804=0x4540298,reg6014=0x5DC;
static patch_hook_function_cbr hooks[4];static uint32_t sites[4];static int nhooks,patch_fail_at=-1;
static int unpatched,stall_after_zoom,no_window,dma_stuck,single_alloc,locked,partial_frames;
static int csv_written;
static unsigned sim_period_us=333333;
static uint8_t *slurp_dst;static int slurp_w,slurp_h,slurp_armed,slurp_frames;
static unsigned allocs,frees,dma_state;
uint32_t sim_mem(uint32_t a)
{
    if (a==0x2998C) return 0xE92D41F0; if (a==0x2986C) return 0xE92D43F8;
    if (a==0xFF2C19AC) return 0xE51FC15C; if (a==0xFF2C1694) return 0xE92D4010;
    if (a==0x892E8+0x10) return pdm_zoom<<16;
    return 0;
}
int is_movie_mode(void){return 1;} unsigned get_ms_clock(void){return now_us/1000;} unsigned get_us_clock(void){return now_us;}
void sync_caches(void){}
void *fio_malloc(size_t n){ if (n>1000000 && single_alloc && allocs>=1) return NULL; allocs++; return calloc(1,n); }
void fio_free(void *p){ frees++; free(p); }
int patch_hook_function(uintptr_t addr,uint32_t orig,patch_hook_function_cbr cb,const char *d)
{ (void)d; if (nhooks==patch_fail_at) return 1; assert(sim_mem(addr)==orig); sites[nhooks]=addr; hooks[nhooks++]=cb; return 0; }
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
void NotifyBox(int ms,const char *fmt,...){ (void)ms;(void)fmt; }
/* Canon engine stubs: registry busy or free, engine refuses (no DMA). */
#include "native-max.h"
static int sim_hw_busy=1,sim_hw_calls,sim_hw_prepared,sim_hw_unsafe,sim_hw_divert;
void lc_native_set_divert(unsigned on){ sim_hw_divert=on; }
int lc_native_resources(struct lc_resource_snapshot *s){ memset(s,0,sizeof(*s)); s->count=10; s->busy=sim_hw_busy; return 1; }
unsigned lc_native_engine_prepare(uint32_t *g){ (void)g; sim_hw_prepared++; return 0; }
unsigned lc_native_engine_close(void){ if (sim_hw_unsafe) return LC_NATIVE_BUSY; sim_hw_prepared--; return 0; }
void lc_native_set_wait_ms(unsigned ms){ (void)ms; }
uint32_t edmac_get_address(uint32_t ch){ (void)ch; return 0; }
uint32_t edmac_get_length(uint32_t ch){ (void)ch; return 0; }
uint32_t edmac_get_flags(uint32_t ch){ (void)ch; return 0; }
uint32_t edmac_get_connection(uint32_t ch,uint32_t dir){ (void)ch;(void)dir; return 0; }
unsigned lc_native_engine_max_ch(const struct lc_native_plane *a,const struct lc_native_plane *b,
    const struct lc_native_plane *o,struct lc_native_engine_result *r,unsigned ch)
{ (void)a;(void)b;(void)o;(void)ch; memset(r,0,sizeof(*r)); r->resume_safe=!sim_hw_unsafe; sim_hw_calls++;
  if (sim_hw_unsafe) { sim_hw_prepared++; return r->reason=LC_NATIVE_WAIT|LC_NATIVE_STOP; } /* quarantined: close refuses */
  return r->reason=LC_NATIVE_BUSY; }
struct vram_info *get_yuv422_vram(void){ return &vram; }
static void (*sim_cb)(void*);static void *sim_cb_ctx;
void RegisterEDmacCompleteCBR(int ch,void (*cbr)(void*),void *ctx){ (void)ch; sim_cb=cbr; sim_cb_ctx=ctx; }
void RegisterEDmacAbortCBR(int ch,void (*cbr)(void*),void *ctx){ (void)ch;(void)cbr;(void)ctx; }
void RegisterEDmacPopCBR(int ch,void (*cbr)(void*),void *ctx){ (void)ch;(void)cbr;(void)ctx; }
void ConnectWriteEDmac(unsigned ch,unsigned where){ (void)ch;(void)where; }
static uint8_t *pending_dst;static int pending_w,pending_h;
void SetEDmac(unsigned ch,void *addr,struct edmac_info *info,int flags){ (void)ch;(void)flags; pending_dst=addr; pending_w=info->xb; pending_h=info->yb+1; }
void StartEDmac(unsigned ch,int flags){ (void)ch;(void)flags; slurp_dst=pending_dst; slurp_w=pending_w; slurp_h=pending_h; slurp_armed=1; }
int raw_lv_is_enabled(void){ return raw_user; } int fps_get_current_x1000(void){ return fps_now; }
struct card_info *get_ml_card(void){ return &card; }
void powersave_prohibit(void){} void powersave_permit(void){} void gui_uilock(int l){ locked=l; }
int raw2iso(int r){ return r; }
static int file_token; static unsigned dng_bytes,log_files;
int is_camera(const char *m,const char *f){ return !strcmp(m,"EOSM") && !strcmp(f,"2.0.2"); }
static int dng_created,cur_dng;
FILE *FIO_CreateFile(const char *p){ if (strstr(p,".CSV")) csv_written=1; if (strstr(p,".LOG")) log_files++; cur_dng=!!strstr(p,".DNG"); if (cur_dng) dng_created=1; return (FILE *)&file_token; }
int FIO_WriteFile(FILE *f,const void *b,int n){ (void)f;(void)b; if (cur_dng) dng_bytes+=n; return n; }
void FIO_CloseFile(FILE *f){ (void)f; }
static unsigned dng_size_ok;
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
void set_lv_zoom(int z){ zoom=z; lv_dispsize=z; canon_reconfigure(z); }
/* Pointers in this simulation are host addresses; keep the alias bit clear. */
static void frame(void)
{
    if (stall_after_zoom && zoom==5) return;
    if (slurp_armed && partial_frames) { partial_frames--; memset(slurp_dst,0x22,1000); last_written=1000; }
    else if (slurp_armed && !dma_stuck) {
        last_written=slurp_w*slurp_h;
        for (int i=0;i<slurp_w*slurp_h;i+=4096) slurp_dst[i]=(uint8_t)(slurp_frames*7+i);
        memset(slurp_dst+slurp_w*slurp_h-8,0x11,8);
        slurp_frames++;
        if (sim_cb) sim_cb(sim_cb_ctx);
    }
    dma_state=dma_stuck && (slurp_armed || dma_state);
    slurp_armed=0;
    livecomp_frlv_vsync();
}
void msleep(int ms)
{
    unsigned period=(zoom==5) ? sim_period_us:33333;
    unsigned end=now_us+ms*1000;
    while (next_vsync_us<=end) { now_us=next_vsync_us; frame(); next_vsync_us+=period; }
    now_us=end;
}
static void reset(void)
{
    now_us=next_vsync_us=0;zoom=pdm_zoom=1;lv_dispsize=1;raw_user=0;fps_now=29970;
    reg6800=0x10012;reg6804=0x4540298;nhooks=0;patch_fail_at=-1;unpatched=0;stall_after_zoom=0;
    no_window=0;dma_stuck=0;partial_frames=0;overrun_sim=0;csv_written=0;single_alloc=0;slurp_armed=0;slurp_frames=0;allocs=frees=0;dma_state=0;
    dng_bytes=0;dng_size_ok=0;locked=0;dng_created=0;
}
int main(void)
{
    setvbuf(stdout,0,_IONBF,0);
    volatile unsigned cancel=0;const char *status="";
    struct lc_frlv_options o={5,0,4,0,0,1,1};   /* index 5 = 3 fps, fix focus pixels, hw test */
    /* Happy path: 4 inputs, geometry from registers, restore, unhook, DNG. */
    reset();
    unsigned w=(0x538-0x12)*4,h=0xDB2-1+1,bytes=w*14/8*h;
    dng_size_ok=2048+bytes;
    unsigned unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("happy: %s unsafe=%u frames=%d unpatched=%d zoom=%u\n",status,unsafe,slurp_frames,unpatched,zoom);
    assert(!unsafe && slurp_frames==4 && unpatched==4 && zoom==1 && lv_dispsize==1);
    assert(slurp_w==(int)(w*14/8) && slurp_h==(int)h && allocs==frees+0 /* scratch freed too */);
    assert(strstr(status,"saved") && dng_bytes==2048+bytes && locked==0);
    /* Refusals before any hook: FPS override on, other RAW user, zoomed. */
    reset();fps_now=3000;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && nhooks==0 && strstr(status,"FPS"));
    reset();raw_user=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && nhooks==0 && allocs==0);
    /* Hook failure: earlier hooks removed, never zoomed. */
    reset();patch_fail_at=2;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && unpatched==2 && zoom==1 && strstr(status,"Hook"));
    /* Window not applied: restore and unhook, no capture. */
    reset();no_window=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && slurp_frames==0 && zoom==1 && unpatched==4);
    /* LV stall after switch: restore, unhook, nothing captured. */
    reset();stall_after_zoom=1;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && slurp_frames==0 && zoom==1 && unpatched==4);
    /* DMA never completes: ownership uncertain, buffers retained. */
    reset();dma_stuck=1;unsigned a0=allocs;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("stuck: %s unsafe=%u allocs=%u frees=%u\n",status,unsafe,allocs,frees);
    assert(unsafe && zoom==1 && frees<allocs && a0==0);
    /* Single allocation: last frame saved from the capture buffer. */
    reset();single_alloc=1;dng_size_ok=2048+bytes;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && slurp_frames==4 && strstr(status,"saved") && frees==allocs);
    /* Slow mode: 0.5 fps timer and 100% exposure are what the hooks apply. */
    reset();o.fps_index=3;dng_size_ok=2048+bytes;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && reg6014!=0x1E03);
    /* Partial frames (idle channel, tail untouched) are retried, not merged. */
    reset();o.fps_index=5;partial_frames=2;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && slurp_frames==4 && strstr(status,"saved"));
    reset();partial_frames=20;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && strstr(status,"partial") && zoom==1 && unpatched==4);
    /* Slowest timing: timeouts scale with the frame period. */
    reset();sim_period_us=3000000;o.fps_index=2;dng_size_ok=2048+bytes;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("slow: %s\n",status);assert(!unsafe && strstr(status,"saved"));sim_period_us=333333;
    /* Continuous composite: every completed frame merged, limit, CSV, DNG. */
    reset();sim_period_us=4000000;o.fps_index=0;o.continuous=1;o.limit=6;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("cont: %s frames=%d\n",status,slurp_frames);
    assert(!unsafe && strstr(status,"saved") && csv_written && zoom==1 && unpatched==4 && slurp_frames>=6);
    /* Continuous with two partial frames: merged and counted, not fatal. */
    reset();partial_frames=2;dng_size_ok=2048+bytes;
    { unsigned before=slurp_frames; (void)before; }
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && strstr(status,"saved"));
    /* Merge overrun (next readout already writing): stop, keep the composite. */
    reset();overrun_sim=1;dng_size_ok=2048+bytes;unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("overrun: %s\n",status);
    assert(!unsafe && strstr(status,"saved") && zoom==1);
    /* HW merge test: registry busy -> no engine call; free -> one call. */
    reset();sim_hw_busy=2;sim_hw_calls=0;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    assert(!unsafe && sim_hw_calls==0 && sim_hw_prepared==0 && strstr(status,"saved"));
    reset();sim_hw_busy=1|0xA000;sim_hw_calls=0;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("hw: %s calls=%d\n",status,sim_hw_calls);
    assert(!unsafe && sim_hw_calls==1 && sim_hw_prepared==0 && strstr(status,"saved") && sim_hw_divert==1);
    /* LCM22 camera case: engine left its output channel busy. The session
     * stops, but the accumulator (never an engine target) is still saved. */
    reset();sim_hw_busy=1|0xA000;sim_hw_calls=0;sim_hw_unsafe=1;dng_size_ok=2048+bytes;
    unsafe=livecomp_frlv_run(&card,&o,&cancel,&status);
    printf("hw unsafe: %s\n",status);
    assert(unsafe && sim_hw_calls==1 && strstr(status,"Composite saved") && dng_bytes>=bytes);
    sim_hw_unsafe=0;sim_hw_prepared=0;
    o.continuous=0;sim_period_us=333333;
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
                  'shoot.h', 'propvalues.h', 'gui-common.h', 'powersave.h', 'vram.h', 'notify_box.h'):
            (tmp / h).write_text('#include "stub.h"\n')
        src = (MOD / 'fullres-lv.c').read_text()
        # 32-bit Canon pointers: on the 64-bit host keep addresses as uintptr_t.
        src = src.replace('(unsigned)p&~livecomp_camera.cache_alias_mask',
                          '(uintptr_t)p&~(uintptr_t)livecomp_camera.cache_alias_mask')
        src = src.replace('static unsigned physical(', 'static uintptr_t physical(')
        src = src.replace('cap=(uint8_t *)lc_frlv_pixels((uint32_t)alloc[0],1);',
                          'cap=(uint8_t *)(((uintptr_t)alloc[0]+FRLV_HALO+2*FRLV_GUARD-1)&~(uintptr_t)(FRLV_GUARD-1));')
        src = src.replace('acc=(uint8_t *)physical((void *)lc_frlv_pixels((uint32_t)alloc[1],0));',
                          'acc=(uint8_t *)(((uintptr_t)alloc[1]+FRLV_HALO+FRLV_GUARD-1)&~(uintptr_t)(FRLV_GUARD-1));')
        assert 'lc_frlv_pixels((uint32_t)alloc' not in src
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
