/* GPL-2.0-or-later. Full-resolution feasibility check, AI-assisted.
 * Deliberately does not install CMOS/ADTG/ENGIO readout or preview redirects.
 * Canon's max engine is asked to process exclusively owned synthetic RAW data
 * while ordinary Live View runs. Busy/uninitialised contexts are reported, not
 * forced or initialised by starting a factory capture. */
#include "dryos.h"
#include "module.h"
#include "lens.h"
#include "raw.h"
#include "mem.h"
#include "propvalues.h"
#include "gui-common.h"
#include "powersave.h"
#include "camera-profile.h"
#include "fullres-check.h"
#include "fullres-layout.h"
#include "native-max.h"
#include "raw14-max.h"

extern int raw_lv_is_enabled(void);
static volatile unsigned observing,vsyncs,last_ms,max_gap_ms;
struct check_result {
    unsigned stage,rom_ok,allocated,prepared,error,unsafe;
    uint32_t state[12];
    unsigned pointers[LC_FR_BUFFERS],engine_us;
    unsigned checked,mismatches,first_bad,expected,actual,guards_ok,source_ok;
    unsigned callbacks,max_gap,elapsed;
    unsigned resource_attempts,resource_valid,resource_free,busy_any,busy_all,quality;
    struct lc_resource_snapshot resources;
    unsigned segment_blocks,segment_bytes;
    unsigned cpu_us,cpu_checked,cpu_mismatches,cpu_source_ok,cpu_callbacks,cpu_gap;
    unsigned log_error,attempted,case_checked[LC_FR_CASES],case_mismatches[LC_FR_CASES],case_source_ok[LC_FR_CASES];
    struct lc_native_engine_result bands[LC_FR_CASES];
};
static unsigned physical(const void *p)
{ return (unsigned)p&~livecomp_camera.cache_alias_mask; }
void livecomp_fullres_vsync(void)
{
    if (!observing) return;
    unsigned now=get_ms_clock();
    if (vsyncs && now-last_ms>max_gap_ms) max_gap_ms=now-last_ms;
    last_ms=now;vsyncs++;
}
static int valid(const volatile unsigned *cancel,struct card_info *card)
{
    return !*cancel && !ml_shutdown_requested && get_ml_card()==card &&
        lv && !LV_PAUSED && is_movie_mode() && lv_dispsize==1 &&
        !RECORDING && !lens_info.job_state && !raw_lv_is_enabled();
}
static int choose_path(char *path,struct card_info *card)
{
    uint32_t size;
    for (unsigned i=0;i<10000;i++) {
        snprintf(path,FIO_MAX_PATH_LENGTH,"%s:/LCFR%04d.LOG",card->drive_letter,i);
        if (FIO_GetFileSize(path,&size)) return 1;
    }
    return 0;
}
static int report(const char *path,const struct check_result *r)
{
    char text[2048];
    int n=snprintf(text,sizeof(text),LIVECOMP_BUILD_ID "\n"
        "Synthetic native-engine check, NOT a full-sensor photograph.\n"
        "Stage_ROM_allocated_prepared_error_unsafe: %d,%d,%d,%d,%d,%d\n"
        "Raster_width_height_pitch_bytes: %d,%d,%d,%d\n"
        "Allocation_each_required: %d,%d\n"
        "Picture_quality: %x\n"
        "Resource_attempts_valid_free_busy_any_busy_all: %d,%d,%d,%x,%x\n"
        "Pointers: %x,%x,%x,%x\n"
        "Native_state: %x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x,%x\n"
        "Checked_mismatches_first_expected_actual: %d,%d,%d,%d,%d\n"
        "Guards_source_ok: %d,%d\n"
        "Engine_us_callbacks_max_gap_ms_elapsed_ms: %d,%d,%d,%d\n"
        "Stages: 1=context 2=memory 3=prepared 4=engine 5=verified 6=cleaned\n",
        r->stage,r->rom_ok,r->allocated,r->prepared,r->error,r->unsafe,
        LC_FR_WIDTH,LC_FR_HEIGHT,LC_FR_PITCH,LC_FR_BYTES,
        LC_FR_ALLOCATION,LC_FR_BUFFERS,r->quality,
        r->resource_attempts,r->resource_valid,r->resource_free,r->busy_any,r->busy_all,
        r->pointers[0],r->pointers[1],r->pointers[2],r->pointers[3],
        r->state[0],r->state[1],r->state[2],r->state[3],r->state[4],r->state[5],
        r->state[6],r->state[7],r->state[8],r->state[9],r->state[10],r->state[11],
        r->checked,r->mismatches,r->first_bad,r->expected,r->actual,
        r->guards_ok,r->source_ok,r->engine_us,r->callbacks,r->max_gap,r->elapsed);
    if (n<=0 || (unsigned)n>=sizeof(text)) return 0;
    FILE *f=FIO_CreateFile(path);if (!f) return 0;
    unsigned done=0;int ok=1;
    while (done<(unsigned)n) {
        int wrote=FIO_WriteFile(f,text+done,n-done);
        if (wrote<=0 || (unsigned)wrote>(unsigned)n-done) { ok=0;break; }
        done+=wrote;
    }
    n=snprintf(text,sizeof(text),
        "CPU_us_checked_mismatches_source_ok_callbacks_max_gap: %d,%d,%d,%d,%d,%d\n"
        "Native_cases_attempted: %d\n"
        "Segmented_blocks_bytes: %d,%d\n",
        r->cpu_us,r->cpu_checked,r->cpu_mismatches,r->cpu_source_ok,r->cpu_callbacks,r->cpu_gap,r->attempted,r->segment_blocks,r->segment_bytes);
    if (FIO_WriteFile(f,text,n)!=n) ok=0;
    for (unsigned i=0;i<LC_FR_CASES && ok;i++) {
        n=snprintf(text,sizeof(text),"Case%d_rows_output_row_checked_mismatches_source_ok: %d,%d,%d,%d,%d\n",
            i,lc_fr_case_rows[i],lc_fr_output_row(i),r->case_checked[i],r->case_mismatches[i],r->case_source_ok[i]);
        if (FIO_WriteFile(f,text,n)!=n) ok=0;
    }
    for (unsigned i=0;i<LC_FR_CASES && ok;i++) {
        const struct lc_native_engine_result *b=&r->bands[i];
        n=snprintf(text,sizeof(text),
            "Band%d_reason_lock_setup_wait_delete_unlock_us: %d,%d,%d,%d,%d,%d,%d\n"
            "Band%d_output_channel_resource_delete: %d,%d\n"
            "Band%d_channels_table_calls_error_restored_safe: %d,%d,%d,%d,%d,%d,%d\n",
            i,b->reason,b->lock,b->setup,b->wait,b->event_delete,b->unlock,b->elapsed_us,
            i,b->output_channel,b->resource_delete,
            i,b->channels[0],b->channels[1],b->channels[2],b->table_calls,
            b->table_error,b->context_restored,b->resume_safe);
        done=0;
        while (done<(unsigned)n) {
            int wrote=FIO_WriteFile(f,text+done,n-done);
            if (wrote<=0 || (unsigned)wrote>(unsigned)n-done) { ok=0;break; }
            done+=wrote;
        }
    }
    for (unsigned band=0;band<LC_FR_CASES && ok;band++) {
        const struct lc_native_engine_result *b=&r->bands[band];
        for (unsigned ch=0;ch<3 && ok;ch++) {
            const uint32_t *before=b->before_start+4*ch,*after=b->after_wait+4*ch;
            n=snprintf(text,sizeof(text),
                "Band%d_DMA%d_callbacks_before_state_flags_address_pointer_after_state_flags_address_pointer: %d,%x,%x,%x,%x,%x,%x,%x,%x\n",
                band,ch,b->completed[ch],before[0],before[1],before[2],before[3],
                after[0],after[1],after[2],after[3]);
            done=0;
            while (done<(unsigned)n) {
                int wrote=FIO_WriteFile(f,text+done,n-done);
                if (wrote<=0 || (unsigned)wrote>(unsigned)n-done) { ok=0;break; }
                done+=wrote;
            }
        }
    }
    for (unsigned i=0;i<r->resources.count && ok;i++) {
        n=snprintf(text,sizeof(text),"Resource%d_id: %x\n",i,r->resources.ids[i]);
        done=0;
        while (done<(unsigned)n) {
            int wrote=FIO_WriteFile(f,text+done,n-done);
            if (wrote<=0 || (unsigned)wrote>(unsigned)n-done) { ok=0;break; }
            done+=wrote;
        }
    }
    FIO_CloseFile(f);return ok;
}
static void probe_segments(struct check_result *r,const volatile unsigned *cancel,struct card_info *card)
{
    /* Two large capture-sized reservations may coexist with a segmented
     * accumulator even when a third contiguous allocation fails. Bounded to
     * one 32 MiB candidate; the ordinary allocator keeps its reserve policy. */
    void *blocks[32]={0};
    for (unsigned i=0;i<32 && valid(cancel,card);i++) {
        blocks[i]=fio_malloc(1024*1024);
        if (!blocks[i]) break;
        volatile uint32_t *p=blocks[i];p[0]=0x13579bdf;p[1024*1024/4-1]=0x2468ace0;
        r->segment_blocks++;r->segment_bytes+=1024*1024;
        msleep(1);
    }
    for (unsigned i=0;i<r->segment_blocks;i++) fio_free(blocks[i]);
}
static int guards_intact(uint8_t *storage[LC_FR_BUFFERS],unsigned count)
{
    for (unsigned i=0;i<count;i++) for (unsigned k=0;k<LC_FR_GUARD;k++)
        if (storage[i][(int)k-(int)LC_FR_GUARD]!=0xa5 || storage[i][LC_FR_STORAGE+k]!=0xa5)
            return 0;
    return 1;
}
static void fill(uint8_t *base,unsigned input)
{
    uint16_t *words=(void *)base,p[8];
    unsigned samples=LC_FR_STORAGE*8/14;
    for (unsigned i=0;i<samples;i+=8,words+=7) {
        for (unsigned k=0;k<8;k++) p[k]=lc_fr_pattern(i+k,input);
        lc_raw14_pack(words,p);
        if (!(i&0x3ffffu)) msleep(1);
    }
}
static void verify(uint8_t *a,uint8_t *b,struct check_result *r,unsigned output_row,unsigned rows)
{
    /* CPU reads only after native idle fencing and cache invalidation. Check
     * both halos, every output sample, and every untouched source sample. */
    const uint16_t *wa=(void *)physical(a),*wb=(void *)physical(b);
    uint16_t pa[8],pb[8];r->source_ok=1;
    r->checked=r->mismatches=r->first_bad=r->expected=r->actual=0;
    unsigned first=output_row*LC_FR_WIDTH,last=first+LC_FR_WIDTH*rows;
    for (unsigned i=0;i<LC_FR_STORAGE*8/14;i+=8,wa+=7,wb+=7) {
        lc_raw14_unpack(wa,pa);lc_raw14_unpack(wb,pb);
        for (unsigned k=0;k<8;k++) {
            unsigned x=i+k,aa=lc_fr_pattern(x,0),bb=lc_fr_pattern(x,1);
            unsigned expected=aa;
            if (x>=first && x<last) {
                unsigned src=LC_FR_HALO_ROWS*LC_FR_WIDTH+x-first;
                unsigned sa=lc_fr_pattern(src,0),sb=lc_fr_pattern(src,1);
                expected=MAX(sa,sb);
            }
            if (pb[k]!=bb) r->source_ok=0;
            if (pa[k]!=expected) {
                if (!r->mismatches) { r->first_bad=x;r->expected=expected;r->actual=pa[k]; }
                r->mismatches++;
            }
            r->checked++;
        }
        if (!(i&0x3ffffu)) msleep(1);
    }
}
unsigned livecomp_fullres_check(struct card_info *card,
    const volatile unsigned *cancel,const char **status)
{
    struct check_result r={0};
    void *allocation[LC_FR_BUFFERS]={0},*geometry_allocation=NULL;
    uint8_t *storage[LC_FR_BUFFERS]={0};
    char path[FIO_MAX_PATH_LENGTH]={0};
    unsigned old_lock=0,locked=0,power=0,started=get_ms_clock();
    *status="Full-res check refused; use idle Movie Live View";
    if (!is_camera("EOSM","2.0.2") || !card || !valid(cancel,card) ||
        !choose_path(path,card)) return 0;
    old_lock=icu_uilock&0xffffu;
    if (old_lock&~UILOCK_SHUTTER) return 0;
    gui_uilock(old_lock|UILOCK_SHUTTER);locked=1;
    powersave_prohibit();power=1;
    r.stage=1;r.rom_ok=lc_native_snapshot(r.state);
    if (!r.rom_ok) { *status="Full-res check: ROM mismatch; see LCFR log";goto done; }
    /* A missing factory context is a result, not permission to invent one. */
    if (!r.state[1] || !r.state[4] || r.state[2] || r.state[6]!=40 ||
        r.state[7]!=2 || r.state[8]!=41 || r.state[9]!=4 || r.state[10]!=5 || r.state[11]) {
        r.error=LC_NATIVE_CONTEXT;
    }
    r.quality=pic_quality;
    *status="Observing native resource availability";
    for (unsigned i=0;i<180 && valid(cancel,card);i++) {
        struct lc_resource_snapshot snapshot;
        r.resource_attempts++;
        if (lc_native_resources(&snapshot)) {
            if (!r.resource_valid) { r.resources=snapshot;r.busy_all=snapshot.busy; }
            if (snapshot.count==r.resources.count &&
                !memcmp(snapshot.ids,r.resources.ids,snapshot.count*sizeof(snapshot.ids[0]))) {
                r.resource_valid++;r.resource_free+=snapshot.busy==0;
                r.busy_any|=snapshot.busy;r.busy_all&=snapshot.busy;
            }
        }
        msleep(17); /* vary phase relative to the 333 ms sensor period */
    }
    r.stage=2;*status="Checking full-res memory capacity";
    for (unsigned i=0;i<LC_FR_BUFFERS && valid(cancel,card);i++) {
        allocation[i]=fio_malloc(LC_FR_ALLOCATION);
        if (!allocation[i]) break;
        storage[i]=(void *)(((unsigned)allocation[i]+2*LC_FR_GUARD-1)&~(LC_FR_GUARD-1));
        r.pointers[i]=(unsigned)storage[i];r.allocated++;
        memset(storage[i]-LC_FR_GUARD,0xa5,LC_FR_GUARD);
        memset(storage[i]+LC_FR_STORAGE,0xa5,LC_FR_GUARD);
    }
    /* Keep this initial report on the card if a native call never returns. */
    if (!report(path,&r)) { *status="Full-res check: could not write log";goto cleanup; }
    if (r.error) { *status="Native engine context unavailable; see LCFR log";goto cleanup; }
    if (r.allocated<2) { *status="Full-res check: insufficient memory; see LCFR log";goto cleanup; }
    if (!valid(cancel,card)) { *status="Full-res check cancelled";goto cleanup; }
    *status="Checking segmented accumulator capacity";
    probe_segments(&r,cancel,card);
    if (!report(path,&r)) { *status="Memory result log failed";goto cleanup; }
    *status="Preparing full-width native RAW test";
    fill(storage[0],0);fill(storage[1],1);
    if (!valid(cancel,card)) { *status="Full-res check cancelled";goto cleanup; }
    /* Measure a usable fallback before any hardware job can require reboot. */
    *status="Benchmarking full-frame CPU blend";
    sync_caches();
    { unsigned irq=cli();vsyncs=last_ms=max_gap_ms=0;observing=1;sei(irq); }
    unsigned cpu_start=get_us_clock();
    for (unsigned row=0;row<LC_FR_HEIGHT && valid(cancel,card);row+=64) {
        unsigned rows=MIN(64u,LC_FR_HEIGHT-row),offset=lc_fr_pixel_offset(row);
        lc_raw14_max((void *)(physical(storage[0])+offset),
            (void *)(physical(storage[1])+offset),LC_FR_WIDTH*rows/8);
        msleep(1);
    }
    sync_caches();r.cpu_us=(unsigned)get_us_clock()-cpu_start;
    { unsigned irq=cli();observing=0;r.cpu_callbacks=vsyncs;r.cpu_gap=max_gap_ms;sei(irq); }
    if (!valid(cancel,card)) { *status="Full-res check cancelled";goto cleanup; }
    verify(storage[0],storage[1],&r,LC_FR_HALO_ROWS,LC_FR_HEIGHT);
    r.cpu_checked=r.checked;r.cpu_mismatches=r.mismatches;r.cpu_source_ok=r.source_ok;
    if (!report(path,&r)) { *status="CPU result log failed";goto cleanup; }
    if (!guards_intact(storage,r.allocated)) { r.unsafe=1;goto cleanup; }
    if (r.cpu_mismatches || !r.cpu_source_ok) { *status="CPU validation failed; see log";goto cleanup; }
    geometry_allocation=fio_malloc(128);
    if (!geometry_allocation) { *status="Full-res check: no geometry memory";goto cleanup; }
    uint32_t *geometry=(void *)(((unsigned)geometry_allocation+63u)&~63u);
    sync_caches();
    r.error=lc_native_engine_prepare(geometry);
    if (r.error) { *status="Full-res engine preparation failed; see LCFR log";goto cleanup; }
    r.prepared=1;r.stage=3;
    if (!report(path,&r)) { *status="Full-res check: could not write log";goto cleanup; }
    *status="Testing native blend while Live View runs";
    { unsigned irq=cli();vsyncs=last_ms=max_gap_ms=0;observing=1;sei(irq); }
    /* Let callback timing establish a baseline before the first native job. */
    msleep(1000);r.stage=4;
    for (unsigned band=0;band<LC_FR_CASES && valid(cancel,card);band++) {
        *status="Testing native size and buffer layouts";
        fill(storage[0],0);fill(storage[1],1);sync_caches();
        if (!valid(cancel,card)) break;
        unsigned rows=lc_fr_case_rows[band],out_row=lc_fr_output_row(band);
        unsigned input_bytes=(rows+2*LC_FR_HALO_ROWS)*LC_FR_PITCH;
        struct lc_native_plane a={(unsigned)storage[0],input_bytes,
            (unsigned)storage[0]+lc_fr_pixel_offset(0),LC_FR_WIDTH,rows,LC_FR_PITCH};
        struct lc_native_plane b={(unsigned)storage[1],input_bytes,
            (unsigned)storage[1]+lc_fr_pixel_offset(0),LC_FR_WIDTH,rows,LC_FR_PITCH};
        struct lc_native_plane out=a;
        if (!lc_fr_case_inplace(band)) {
            out.storage=out.pixels=(unsigned)storage[0]+out_row*LC_FR_PITCH;
            out.bytes=rows*LC_FR_PITCH;
        }
        r.attempted=band+1;
        if (!r.unsafe && !guards_intact(storage,r.allocated)) r.unsafe=1;
        if (!report(path,&r)) { r.log_error=1;*status="Native progress log failed";break; }
        if (r.unsafe) break;
        r.error=lc_native_engine_max(&a,&b,&out,&r.bands[band]);
        r.engine_us+=r.bands[band].elapsed_us;
        if (!r.bands[band].resume_safe) r.unsafe=1;
        if (!r.error && !r.unsafe) {
            sync_caches();verify(storage[0],storage[1],&r,out_row,rows);
            r.case_checked[band]=r.checked;r.case_mismatches[band]=r.mismatches;
            r.case_source_ok[band]=r.source_ok;
        }
        if (!r.unsafe && !guards_intact(storage,r.allocated)) r.unsafe=1;
        if (!report(path,&r)) { r.log_error=1;*status="Native progress log failed";break; }
        if (r.error || r.unsafe || r.mismatches || !r.source_ok) break;
    }
    msleep(1000);
    { unsigned irq=cli();observing=0;r.callbacks=vsyncs;r.max_gap=max_gap_ms;sei(irq); }
    if (r.unsafe) { *status="Native engine ownership uncertain; keep log and reboot";goto cleanup; }
    if (r.log_error) { *status="Native progress log failed";goto cleanup; }
    if (r.error) { *status="Native engine unavailable/failed; see LCFR log";goto cleanup; }
    if (!valid(cancel,card)) { *status="Full-res check cancelled";goto cleanup; }
    if (r.attempted!=LC_FR_CASES || !r.case_checked[LC_FR_CASES-1]) {
        *status="Native suite incomplete; see LCFR log";goto cleanup;
    }
    r.stage=5;
    *status=r.mismatches || !r.source_ok ? "Native blend mismatch; see LCFR log":
        r.allocated<LC_FR_BUFFERS ? "Native blend passed; memory insufficient; see log":
        "Native blend passed; sensor readout still pending";
cleanup:
    if (r.prepared && !r.unsafe && lc_native_engine_close()) r.unsafe=1;
    if (!r.unsafe) {
        r.guards_ok=1;
        for (unsigned i=0;i<r.allocated;i++) {
            for (unsigned k=0;k<LC_FR_GUARD;k++)
                if (storage[i][(int)k-(int)LC_FR_GUARD]!=0xa5 || storage[i][LC_FR_STORAGE+k]!=0xa5)
                    r.guards_ok=0;
        }
        /* Corruption outside an exclusively owned plane invalidates the test. */
        if (!r.guards_ok) { r.unsafe=1;*status="Native buffer guard changed; keep log and reboot"; }
    }
    if (!r.unsafe) {
        for (unsigned i=0;i<LC_FR_BUFFERS;i++) if (allocation[i]) fio_free(allocation[i]);
        if (geometry_allocation) fio_free(geometry_allocation);
        /* Do not hide the stopping stage; 6 means the full verification ran. */
        if (r.stage==5) r.stage=6;
    } else *status="Native check uncertain; keep log and reboot";
done:
    r.elapsed=get_ms_clock()-started;
    if (!ml_shutdown_requested && get_ml_card()==card && !report(path,&r))
        *status=r.unsafe ? "Native check uncertain; log failed; reboot":"Full-res check log write failed";
    if (locked) gui_uilock(old_lock);
    if (power) powersave_permit();
    return r.unsafe;
}
