/* GPL-2.0-or-later. EXPERIMENTAL (livecomp-hwx branch, LC_HWX builds only).
 * Hardware-merge Step 1b: Canon's max engine while Live View RUNS.
 *
 * LCM9-40 always ran the in-LV engine job inside the idle window of a slow
 * full-res frame (0.25-0.5 fps), where it never wrote a byte. The only
 * in-LV output ever seen was LCM13: continuous 3 fps Live View, output
 * channel 3 advanced 422 KB during a 5 s wait. Hypothesis: in Live View the
 * engine output is paced by Live View's frames (it only moves while frames
 * run). Test: x5 full-res at 3 fps (frames every 333 ms), one engine job on
 * the top band (rows 4..1753) with output on channel 3 (never used by Live
 * View; abort proven safe in LV, LCM23), waited for up to 3 s, while a
 * helper task records every change of the output/input pointers with its
 * time and vsync number. A second job uses a 6 s wait.
 *
 * Content check: before each job, cap rows 100-107 get the original frame
 * back and the same accumulator rows are set to 0xFF bytes (all values 16383); out = max(acc, cap) is written into
 * cap, so those cap rows must become all 0xFF if the engine really ran.
 * The composite is never saved in this mode. Results: LVHWXE.LOG. */
#ifndef LIVECOMP_HWX_ENGINE_H
#define LIVECOMP_HWX_ENGINE_H
#include "native-max.h"

#define HWXE_JOBS 2u
#define HWXE_EVENTS 160u
#define HWXE_ROW0 4u
#define HWXE_ROWS 1750u
#define HWXE_MARK_ROW 100u
#define HWXE_MARK_ROWS 8u
struct hwxe_event { uint16_t ms,vs;uint8_t ch,state;uint32_t off; };
static struct {
    unsigned ran,prepare,close,unsafe;
    struct { unsigned ran,wait_ms,reason,lock,setup,wait,us,completed[3],aborted,resume_safe,
        out_bytes,mark_ok,mark_bad,other_moved,listeners,divert,vs0,vs1,events,lost; } j[HWXE_JOBS];
} hwxe;
static struct hwxe_event hwxe_ev[HWXE_JOBS][HWXE_EVENTS];
static volatile unsigned hwxe_tracing,hwxe_job,hwxe_t0,hwxe_task_done;
static uint32_t hwxe_base[3];
static void hwxe_trace_task(void *unused)
{
    (void)unused;
    static const unsigned ch[3]={3,40,41};
    uint32_t last[3]={~0u,~0u,~0u},lastst[3]={9,9,9};
    while (hwxe_tracing) {
        /* Peripheral reads only while the job holds (powers) the channels. */
        if (!lc_native_job_active()) { msleep(1);continue; }
        for (unsigned i=0;i<3;i++) {
            const uint32_t p=edmac_get_pointer(ch[i]),s=edmac_get_state(ch[i]);
            if (p==last[i] && s==lastst[i]) continue;
            last[i]=p;lastst[i]=s;
            const unsigned j=hwxe_job,n=hwxe.j[j].events;
            if (n<HWXE_EVENTS) {
                struct hwxe_event *e=&hwxe_ev[j][n];
                e->ms=(uint16_t)(get_ms_clock()-hwxe_t0);e->vs=(uint16_t)vsyncs;e->ch=(uint8_t)ch[i];
                e->state=(uint8_t)s;e->off=p-hwxe_base[i];hwxe.j[j].events=n+1;
            } else hwxe.j[j].lost++;
        }
        msleep(2);
    }
    hwxe_task_done=1;
}
static void hwxe_plane(uint8_t *base,unsigned bytes,unsigned pitch,struct lc_native_plane *p)
{
    const uint32_t u=physical(base)|0x40000000u;
    p->storage=u;p->bytes=bytes;p->width=5280;p->pitch=pitch;p->height=HWXE_ROWS;
    p->pixels=u+HWXE_ROW0*pitch;
}
static void hwxe_run(struct frlv_result *r,uint8_t *cap,uint8_t *acc,unsigned *dma_uncertain,const char **msg)
{
    memset(&hwxe,0,sizeof(hwxe));hwxe.ran=1;
    static uint32_t *geometry;
    if (!geometry) geometry=fio_malloc(128);
    if (!geometry) { hwxe.prepare=999;return; }
    hwxe.prepare=lc_native_engine_prepare((uint32_t *)(((unsigned)geometry+63)&~63u));
    if (hwxe.prepare) { *msg="HW engine: prepare failed";return; }
    struct lc_native_plane a,b;
    hwxe_plane(acc,r->bytes,r->pitch,&a);hwxe_plane(cap,r->bytes,r->pitch,&b);
    uint8_t *acc_mark=(uint8_t *)physical(acc)+HWXE_MARK_ROW*r->pitch;
    uint8_t *cap_mark=(uint8_t *)(physical(cap)|0x40000000u)+HWXE_MARK_ROW*r->pitch;
    /* Original frame rows, to reset cap before each job (a job may write
     * part of the band before stalling). */
    uint8_t *orig=malloc(HWXE_MARK_ROWS*r->pitch);
    if (!orig) { *msg="HW engine: no memory";lc_native_engine_close();return; }
    memcpy(orig,cap_mark,HWXE_MARK_ROWS*r->pitch);
    for (unsigned k=0;k<HWXE_JOBS && !*dma_uncertain;k++) {
        static char line[40];
        snprintf(line,sizeof(line),"HW engine in LV: job %d",k+1);show_text(line);
        hwxe_job=k;hwxe.j[k].ran=1;hwxe.j[k].wait_ms=k ? 6000:3000;
        /* Fresh content each job: cap rows hold the frame again (from acc,
         * which equals the frame after one merge), then the marker in acc. */
        memcpy(cap_mark,orig,HWXE_MARK_ROWS*r->pitch);
        memset(acc_mark,0xFF,HWXE_MARK_ROWS*r->pitch);
        sync_caches();
        hwxe_base[0]=physical(cap)+HWXE_ROW0*r->pitch;
        hwxe_base[1]=physical(acc)+(HWXE_ROW0-1)*r->pitch;
        hwxe_base[2]=physical(cap)+(HWXE_ROW0-1)*r->pitch;
        lc_native_set_wait_ms(hwxe.j[k].wait_ms);
        lc_native_set_divert(1);
        /* Start ~40 ms after a vsync, like the RAW slurp. */
        { const unsigned v=vsyncs,w0=get_ms_clock();while (vsyncs==v && get_ms_clock()-w0<1000) msleep(2);msleep(40); }
        hwxe_tracing=1;hwxe_task_done=0;hwxe_t0=get_ms_clock();
        task_create("hwxtrace",0x1a,0x1000,hwxe_trace_task,0);
        hwxe.j[k].vs0=vsyncs;
        struct lc_native_engine_result res;
        const unsigned reason=lc_native_engine_max_ch(&a,&b,&b,&res,3);
        hwxe.j[k].vs1=vsyncs;
        hwxe_tracing=0;
        for (unsigned i=0;i<50 && !hwxe_task_done;i++) msleep(5);
        hwxe.j[k].reason=reason;hwxe.j[k].lock=res.lock;hwxe.j[k].setup=res.setup;hwxe.j[k].wait=res.wait;
        hwxe.j[k].us=res.elapsed_us;memcpy(hwxe.j[k].completed,res.completed,sizeof(res.completed));
        hwxe.j[k].aborted=res.aborted;hwxe.j[k].resume_safe=res.resume_safe;
        hwxe.j[k].out_bytes=res.after_wait[11]-res.after_wait[10];
        hwxe.j[k].listeners=res.listeners;hwxe.j[k].divert=res.diverted;
        for (unsigned i=0;i<LC_NATIVE_WRITE_CHANNELS;i++)
            if (res.all_ptr0[i]!=res.all_ptr1[i] && lc_native_write_channels[i]!=3) hwxe.j[k].other_moved|=1u<<i;
        if (!res.resume_safe) { *dma_uncertain=1;break; }
        sync_caches();
        for (unsigned i=0;i<HWXE_MARK_ROWS*r->pitch;i++) if (cap_mark[i]!=0xFF) hwxe.j[k].mark_bad++;
        hwxe.j[k].mark_ok=!hwxe.j[k].mark_bad;
        if (hwxe.j[k].mark_ok) break;                    /* proved: no need for the long wait */
    }
    free(orig);
    /* acc rows are not saved in this mode; leave them. */
    if (!*dma_uncertain) hwxe.close=lc_native_engine_close();
    if (hwxe.close) *dma_uncertain=1;
    hwxe.unsafe=*dma_uncertain;
    if (!*dma_uncertain) *msg=hwxe.j[0].mark_ok || hwxe.j[1].mark_ok ? "HW engine in LV: OUTPUT OK":"HW engine in LV: no output";
}
static void hwxe_write_log(struct card_info *card)
{
    if (!hwxe.ran || ml_shutdown_requested || !card) return;
    char path[FIO_MAX_PATH_LENGTH];
    snprintf(path,sizeof(path),"%s:/LVHWXE.LOG",card->drive_letter);
    FILE *f=FIO_CreateFile(path);
    if (!f) return;
    static char t[8192];
    int n=snprintf(t,sizeof(t),LIVECOMP_BUILD_ID "\nHW Step 1b: engine with Live View running (x5, 3 fps, out ch3)\n"
        "Prepare_close_unsafe: %d,%d,%d\n"
        "Job: ran,wait_ms,reason,lock,setup,wait,us,done_a,done_b,done_out,aborted,resume_safe,out_bytes,mark_ok,mark_bad,other_moved,listeners,divert,vs0,vs1,events,lost\n",
        hwxe.prepare,hwxe.close,hwxe.unsafe);
    for (unsigned k=0;k<HWXE_JOBS;k++) {
        const typeof(hwxe.j[0]) *q=&hwxe.j[k];
        n+=snprintf(t+n,sizeof(t)-n,"J%d: %d,%d,%x,%x,%x,%x,%d,%d,%d,%d,%d,%d,%d,%d,%d,%x,%d,%x,%d,%d,%d,%d\n",k+1,
            q->ran,q->wait_ms,q->reason,q->lock,q->setup,q->wait,q->us,q->completed[0],q->completed[1],q->completed[2],
            q->aborted,q->resume_safe,q->out_bytes,q->mark_ok,q->mark_bad,q->other_moved,q->listeners,q->divert,q->vs0,q->vs1,q->events,q->lost);
    }
    for (unsigned k=0;k<HWXE_JOBS;k++) {
        if (!hwxe.j[k].ran) continue;
        n+=snprintf(t+n,sizeof(t)-n,"J%d events (ms:vsync:ch:state:offset):",k+1);
        for (unsigned i=0;i<hwxe.j[k].events && n<(int)sizeof(t)-48;i++) {
            const struct hwxe_event *e=&hwxe_ev[k][i];
            n+=snprintf(t+n,sizeof(t)-n,"%s%d:%d:%d:%d:%d",i%6 ? " ":"\n",e->ms,e->vs,e->ch,e->state,e->off);
        }
        t[n++]='\n';
    }
    FIO_WriteFile(f,t,n);
    FIO_CloseFile(f);
}
#endif
