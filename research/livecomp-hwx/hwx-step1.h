/* GPL-2.0-or-later. EXPERIMENTAL (livecomp-hwx branch, LC_HWX builds only).
 * Hardware-merge Step 1: does the full-sensor readout keep delivering frames
 * while Live View is paused? (The engine only runs with Live View paused,
 * LCM9-40.) Included by fullres-lv.c after one normal full-res frame, still
 * in x5 at 3 fps, with our RAW slurp idle.
 *
 * Tries, cumulative, each arming one RAW slurp (EDMAC 0x12) for <= 3 s:
 *   L  Live View running (control: must complete in ~1-2 frames)
 *   T0 paused, nothing else
 *   T1 + hold the Canon resources Live View had locked (powers their domains)
 *   T2 + rewrite the timing generator (0xC0F06000-0xC0F07FFC) to its LV values
 *   T3 + replay the CMOS/ADTG lists recorded while entering x5
 *   T4 + put the 189 pipeline registers that differ when paused (LCM32
 *        whole-window diff: routing, start flags, clocks) back to LV values
 * Stops at the first complete frame. Then undoes everything in reverse,
 * resumes Live View and the normal session restore leaves x5.
 * Results: LVHWXD.LOG (LVHWX.LOG is the normal session LOG). */
#ifndef LIVECOMP_HWX_STEP1_H
#define LIVECOMP_HWX_STEP1_H
#include "hwx-res.h"
#include "lv-pause-regs.h"

#define HWX_TG0 0xC0F06000u
#define HWX_TG_WORDS 2048u
#define HWX_TRIES 6u
#define HWX_MAX_LOCKED 128u
#define HWX_HANDLES 8u
#define HWX_TG_WRITES 256u

struct hwx_try {
    unsigned ran,armed,moved,first_move_ms,end_ms,cb_kind,state_end,offset,complete;
    unsigned aborted,abort_ok,guards,center,vsyncs,cmos,adtg,engio,lv,paused,zoom;
};
static struct {
    unsigned ran,ok_level,pause_ms,resume_ms,resumed,lv_after,paused_after,zoom_after;
    unsigned res_lv,res_lv_locked,res_paused,res_paused_locked,held,hold_err,release_err,handles;
    unsigned tg_diff,tg_written,tg_restored,pipe_written,pipe_restored,cmos_lists,adtg_lists,replayed;
    unsigned unsafe,lv_vsyncs_paused;
    struct hwx_try t[HWX_TRIES];
} hwx;
static uint32_t hwx_tg_lv[HWX_TG_WORDS],hwx_tg_paused[HWX_TG_WORDS];
static uint32_t hwx_ids[HWX_MAX_LOCKED],hwx_states[HWX_MAX_LOCKED],hwx_locked[HWX_MAX_LOCKED];
static uint32_t hwx_handle[HWX_HANDLES];
static uint32_t hwx_pipe_lv[LC_PAUSE_ABC],hwx_pipe_paused[LC_PAUSE_ABC];
static uint16_t hwx_tg_reg[HWX_TG_WRITES];   /* word index of each TG write */

/* CMOS/ADTG lists as written (after our rewrite) while entering x5. */
#define HWX_LISTS 8u
#define HWX_LIST_MAX 160u
static struct { uint32_t cs,n;uint32_t v[HWX_LIST_MAX]; } hwx_adtg[HWX_LISTS];
static struct { uint32_t n;uint16_t v[HWX_LIST_MAX]; } hwx_cmos[HWX_LISTS];
static volatile unsigned hwx_recording,hwx_nadtg,hwx_ncmos;
static void FAST hwx_rec_cmos(const uint16_t *l)
{
    if (!hwx_recording) return;
    unsigned n=0;while (n<HWX_LIST_MAX && l[n]!=0xFFFFu) n++;
    if (n>=HWX_LIST_MAX) return;
    /* Same first register: a newer version of the same list. */
    unsigned k=0;
    for (;k<hwx_ncmos;k++) if (hwx_cmos[k].n && (hwx_cmos[k].v[0]>>12)==(l[0]>>12) && hwx_cmos[k].n==n+1) break;
    if (k==hwx_ncmos) { if (k>=HWX_LISTS) return;hwx_ncmos++; }
    for (unsigned i=0;i<=n;i++) hwx_cmos[k].v[i]=l[i];
    hwx_cmos[k].n=n+1;
}
static void FAST hwx_rec_adtg(uint32_t cs,const uint32_t *l)
{
    if (!hwx_recording) return;
    unsigned n=0;while (n<HWX_LIST_MAX && l[n]!=0xFFFFFFFFu) n++;
    if (n>=HWX_LIST_MAX) return;
    unsigned k=0;
    for (;k<hwx_nadtg;k++) if (hwx_adtg[k].cs==cs && hwx_adtg[k].n==n+1 && (hwx_adtg[k].v[0]>>16)==(l[0]>>16)) break;
    if (k==hwx_nadtg) { if (k>=HWX_LISTS) return;hwx_nadtg++; }
    for (unsigned i=0;i<=n;i++) hwx_adtg[k].v[i]=l[i];
    hwx_adtg[k].cs=cs;hwx_adtg[k].n=n+1;
}

static void hwx_engdrv(uint32_t reg,uint32_t val) { ((void (*)(uint32_t,uint32_t))0xff2c1694)(reg,val); }

static unsigned hwx_center(const uint8_t *buf,unsigned pitch,unsigned w,unsigned h)
{
    unsigned s=0;
    for (unsigned j=0;j<8;j++) for (unsigned i=0;i<8;i++) s+=sample(buf,pitch,w/2-32+8*i,h/2-32+8*j);
    return s/64;
}
/* One RAW slurp, polled for up to 3 s. 0 = DMA idle afterwards. */
static unsigned hwx_try(struct hwx_try *t,struct frlv_result *r,uint8_t *cap)
{
    t->ran=1;t->lv=lv;t->paused=LV_PAUSED;t->zoom=pdm_zoom();
    const unsigned v0=vsyncs,c0=stats.cmos_calls,a0=stats.adtg_calls,e0=stats.engio_calls;
    const uint32_t start=physical(cap);
    memset(cap-FRLV_GUARD,0x5a,FRLV_GUARD);memset(cap+r->bytes,0x5a,FRLV_GUARD);
    EngDrvOut(FRLV_RAW_TYPE_REG,FRLV_RAW_TYPE);
    frlv_slurp((void *)start,r->pitch,r->height);t->armed=1;
    const unsigned t0=get_ms_clock();
    for (;;) {
        const uint32_t p=edmac_get_pointer(raw_write_chan);
        if (p!=start && !t->moved) { t->moved=1;t->first_move_ms=get_ms_clock()-t0; }
        if (cb_kind || !edmac_get_state(raw_write_chan) || get_ms_clock()-t0>3000) break;
        msleep(5);
    }
    t->end_ms=get_ms_clock()-t0;t->cb_kind=cb_kind;
    t->state_end=edmac_get_state(raw_write_chan);
    t->offset=edmac_get_pointer(raw_write_chan)-start;
    t->vsyncs=vsyncs-v0;t->cmos=stats.cmos_calls-c0;t->adtg=stats.adtg_calls-a0;t->engio=stats.engio_calls-e0;
    if (t->state_end) {
        t->aborted=1;AbortEDmac(raw_write_chan);
        for (unsigned i=0;i<30 && edmac_get_state(raw_write_chan);i++) msleep(10);
        t->abort_ok=!edmac_get_state(raw_write_chan);
        if (!t->abort_ok) return 1;
    }
    t->guards=guards_ok(cap,r->bytes);
    t->complete=(t->cb_kind&1) && t->offset==r->bytes && t->guards;
    if (t->moved && t->guards) { sync_caches();t->center=hwx_center((const uint8_t *)start,r->pitch,r->width,r->height); }
    return 0;
}

static void hwx_step1(struct frlv_result *r,uint8_t *cap,unsigned *dma_uncertain,const char **msg)
{
    memset(&hwx,0,sizeof(hwx));hwx.ran=1;hwx.ok_level=~0u;
    hwx_recording=0;
    /* LV values and LV's locked resources. */
    for (unsigned i=0;i<HWX_TG_WORDS;i++) hwx_tg_lv[i]=shamem_read(HWX_TG0+4*i);
    for (unsigned i=0;i<LC_PAUSE_ABC;i++) hwx_pipe_lv[i]=shamem_read(lc_pause_regs[i].reg);
    hwx.res_lv=hwx_resource_list(hwx_ids,hwx_states,HWX_MAX_LOCKED);
    unsigned nlocked=0;
    for (unsigned i=0;i<hwx.res_lv;i++) if (hwx_states[i]==1) hwx_locked[nlocked++]=hwx_ids[i];
    hwx.res_lv_locked=nlocked;
    hwx.cmos_lists=hwx_ncmos;hwx.adtg_lists=hwx_nadtg;

    /* L: control with Live View running, armed 40 ms after a vsync (LCX1
     * armed right after one and Canon cut it after 18 rows). */
    show_text("HW test: control frame");
    { const unsigned v=vsyncs,w0=get_ms_clock();while (vsyncs==v && get_ms_clock()-w0<1000) msleep(2);msleep(40); }
    if (hwx_try(&hwx.t[0],r,cap)) { *dma_uncertain=1;goto out; }

    show_text("HW test: pausing LV");
    unsigned t0=get_ms_clock();
    const unsigned v0=vsyncs;
    PauseLiveView();
    hwx.pause_ms=get_ms_clock()-t0;
    if (!LV_PAUSED) { *msg="HW test: pause failed";goto resume; }
    msleep(300);
    for (unsigned i=0;i<HWX_TG_WORDS;i++) hwx_tg_paused[i]=shamem_read(HWX_TG0+4*i);
    for (unsigned i=0;i<LC_PAUSE_ABC;i++) hwx_pipe_paused[i]=shamem_read(lc_pause_regs[i].reg);
    for (unsigned i=0;i<HWX_TG_WORDS;i++) hwx.tg_diff+=hwx_tg_lv[i]!=hwx_tg_paused[i];
    {
        static uint32_t ids[HWX_MAX_LOCKED],st[HWX_MAX_LOCKED];
        hwx.res_paused=hwx_resource_list(ids,st,HWX_MAX_LOCKED);
        for (unsigned i=0;i<hwx.res_paused;i++) hwx.res_paused_locked+=st[i]==1;
    }

    for (unsigned level=1;level<HWX_TRIES;level++) {
        static char line[40];
        snprintf(line,sizeof(line),"HW test: paused T%d",level-1);show_text(line);
        if (level==2) {          /* T1: hold LV's resources, 16 ids per set */
            for (unsigned i=0;i<nlocked && hwx.handles<HWX_HANDLES;i+=16) {
                const unsigned n=MIN(16u,nlocked-i);
                const unsigned e=hwx_hold(&hwx_locked[i],n,&hwx_handle[hwx.handles]);
                if (e) { hwx.hold_err=hwx.hold_err ? hwx.hold_err:e;continue; }
                hwx.handles++;hwx.held+=n;
            }
            msleep(100);
        }
        if (level==3) {          /* T2: timing generator back to LV values */
            for (unsigned i=1;i<HWX_TG_WORDS && hwx.tg_written<HWX_TG_WRITES;i++)
                if (hwx_tg_lv[i]!=hwx_tg_paused[i]) { hwx_engdrv(HWX_TG0+4*i,hwx_tg_lv[i]);hwx_tg_reg[hwx.tg_written++]=i; }
            hwx_engdrv(HWX_TG0,1);          /* apply, as the FPS override does */
            msleep(100);
        }
        if (level==4) {          /* T3: sensor lists replayed via Canon's writers */
            for (unsigned k=0;k<hwx_ncmos;k++) { ((void (*)(uint16_t *))FRLV_CMOS_WRITE)(hwx_cmos[k].v);hwx.replayed++; }
            for (unsigned k=0;k<hwx_nadtg;k++) { ((void (*)(uint32_t,uint32_t *))FRLV_ADTG_WRITE)(hwx_adtg[k].cs,hwx_adtg[k].v);hwx.replayed++; }
            hwx_engdrv(HWX_TG0,1);
            msleep(100);
        }
        if (level==5) {          /* T4: pipeline routing/start/clock registers */
            for (unsigned i=LC_PAUSE_ABC;i-->0;)
                if (hwx_pipe_lv[i]!=hwx_pipe_paused[i]) { hwx_engdrv(lc_pause_regs[i].reg,hwx_pipe_lv[i]);hwx.pipe_written++; }
            hwx_engdrv(HWX_TG0,1);
            msleep(100);
        }
        if (hwx_try(&hwx.t[level],r,cap)) { *dma_uncertain=1;break; }
        if (hwx.t[level].complete) { hwx.ok_level=level-1;break; }
    }
    hwx.lv_vsyncs_paused=vsyncs-v0;

    /* Undo in reverse order (paused values), then resume. */
    for (unsigned i=0;i<LC_PAUSE_ABC && hwx.pipe_written;i++)
        if (hwx_pipe_lv[i]!=hwx_pipe_paused[i]) { hwx_engdrv(lc_pause_regs[i].reg,hwx_pipe_paused[i]);hwx.pipe_restored++; }
    for (unsigned k=hwx.tg_written;k-->0;) { const unsigned i=hwx_tg_reg[k];hwx_engdrv(HWX_TG0+4*i,hwx_tg_paused[i]);hwx.tg_restored++; }
    for (unsigned h=hwx.handles;h-->0;) hwx.release_err|=hwx_release(hwx_handle[h])<<(2*h);
resume:
    show_text("HW test: resuming LV");
    t0=get_ms_clock();
    ResumeLiveView();
    hwx.resume_ms=get_ms_clock()-t0;
    hwx.resumed=1;
    {
        const unsigned w0=get_ms_clock();
        while (!(lv && !LV_PAUSED && pdm_zoom()==5) && get_ms_clock()-w0<5000) msleep(50);
    }
    hwx.lv_after=lv;hwx.paused_after=LV_PAUSED;hwx.zoom_after=pdm_zoom();
out:
    hwx.unsafe=*dma_uncertain;
    if (!*dma_uncertain) *msg=hwx.ok_level!=~0u ? "HW test: paused frames OK":"HW test: no paused frames";
}

static void hwx_write_log(struct card_info *card,const struct frlv_result *r)
{
    if (!hwx.ran || ml_shutdown_requested || !card) return;
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/LVHWXD.LOG",card->drive_letter);
    FILE *f=FIO_CreateFile(path);
    if (!f) return;
    static char t[4096];
    int n=snprintf(t,sizeof(t),LIVECOMP_BUILD_ID "\nHW Step 1: paused readout\n"
        "OK_level (T0..T4, -1 none): %d\nUnsafe: %d\nWidth_height_bytes: %d,%d,%d\n"
        "Pause_ms_resume_ms_resumed_lv_paused_zoom_after: %d,%d,%d,%d,%d,%d\n"
        "Vsyncs_while_paused: %d\n"
        "Resources_lv_locked_paused_locked: %d,%d,%d,%d\nHeld_handles_hold_err_release_err: %d,%d,%d,%x\n"
        "TG_diff_written_restored: %d,%d,%d\nPipe_written_restored: %d,%d\n"
        "Lists_cmos_adtg_replayed: %d,%d,%d\n"
        "Try: ran,lv,paused,zoom,armed,moved,first_move_ms,end_ms,cb_kind,state_end,offset,complete,aborted,abort_ok,guards,center,vsyncs,cmos,adtg,engio\n",
        (int)hwx.ok_level,hwx.unsafe,r->width,r->height,r->bytes,
        hwx.pause_ms,hwx.resume_ms,hwx.resumed,hwx.lv_after,hwx.paused_after,hwx.zoom_after,hwx.lv_vsyncs_paused,
        hwx.res_lv,hwx.res_lv_locked,hwx.res_paused,hwx.res_paused_locked,hwx.held,hwx.handles,hwx.hold_err,hwx.release_err,
        hwx.tg_diff,hwx.tg_written,hwx.tg_restored,hwx.pipe_written,hwx.pipe_restored,
        hwx_ncmos,hwx_nadtg,hwx.replayed);
    static const char *names[HWX_TRIES]={"L ","T0","T1","T2","T3","T4"};
    for (unsigned k=0;k<HWX_TRIES;k++) {
        const struct hwx_try *q=&hwx.t[k];
        n+=snprintf(t+n,sizeof(t)-n,"%s: %d,%d,%d,%d,%d,%d,%d,%d,%x,%x,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",names[k],
            q->ran,q->lv,q->paused,q->zoom,q->armed,q->moved,q->first_move_ms,q->end_ms,q->cb_kind,q->state_end,
            q->offset,q->complete,q->aborted,q->abort_ok,q->guards,q->center,q->vsyncs,q->cmos,q->adtg,q->engio);
    }
    n+=snprintf(t+n,sizeof(t)-n,"TG_diff (word:lv:paused, first 120):");
    for (unsigned i=0,m=0;i<HWX_TG_WORDS && m<120 && n<(int)sizeof(t)-40;i++)
        if (hwx_tg_lv[i]!=hwx_tg_paused[i]) { n+=snprintf(t+n,sizeof(t)-n,"%s%x:%x:%x",m%6 ? " ":"\n",HWX_TG0+4*i,hwx_tg_lv[i],hwx_tg_paused[i]);m++; }
    t[n++]='\n';
    FIO_WriteFile(f,t,n);
    n=snprintf(t,sizeof(t),"Locked_in_LV:");
    for (unsigned i=0;i<hwx.res_lv_locked && n<(int)sizeof(t)-16;i++) n+=snprintf(t+n,sizeof(t)-n,"%s%x",i%12 ? " ":"\n",hwx_locked[i]);
    n+=snprintf(t+n,sizeof(t)-n,"\nPipe_diff (reg:lv:paused):");
    for (unsigned i=0,m=0;i<LC_PAUSE_ABC && n<(int)sizeof(t)-40;i++)
        if (hwx_pipe_lv[i]!=hwx_pipe_paused[i]) { n+=snprintf(t+n,sizeof(t)-n,"%s%x:%x:%x",m%6 ? " ":"\n",lc_pause_regs[i].reg,hwx_pipe_lv[i],hwx_pipe_paused[i]);m++; }
    n+=snprintf(t+n,sizeof(t)-n,"\nCMOS lists:");
    for (unsigned k=0;k<hwx_ncmos;k++) n+=snprintf(t+n,sizeof(t)-n," %d",hwx_cmos[k].n);
    n+=snprintf(t+n,sizeof(t)-n,"\nADTG lists (cs:n):");
    for (unsigned k=0;k<hwx_nadtg;k++) n+=snprintf(t+n,sizeof(t)-n," %x:%d",hwx_adtg[k].cs,hwx_adtg[k].n);
    t[n++]='\n';
    FIO_WriteFile(f,t,n);
    FIO_CloseFile(f);
}
#endif
