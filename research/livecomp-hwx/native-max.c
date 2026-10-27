/* GPL-2.0-or-later. Adapted from the local EOS M factory prototype.
 * Runtime guarded; used only by the explicitly selected feasibility check.
 * Reusable native RAW operation for capture integration. Canon owns DMA
 * geometry, correction, start/stop and callbacks. Unlike ProcessPath's normal
 * lock, TryLockEngineResources refuses immediately when another path owns it. */
#include "dryos.h"
#include "module.h"
#include "patch.h"
#include "edmac.h"
#include "native-max.h"

#define NATIVE_STATE 0x54fc8u
#define TABLE_SITE 0xff42a01cu
#define TABLE_INSTRUCTION 0xe59f0290u
#define TABLE_ADDRESS (NATIVE_STATE+0x218u)
#define TABLE_BYTES 0x188u
#define DEFAULT_OUTPUT_CHANNEL 3u   /* LCM13/14 synthetic check route */
#define UNSET 0xffffffffu
static uint32_t *private_geometry;
static unsigned installed,quarantined;
/* The setup hook observes these fields in the calling Canon task. */
static volatile unsigned running,table_calls,table_error;
/* Output write channel/resource of the current job: 5 = Canon's own route
 * (proved in the LCM1 factory composite), 3 = the LCM13 replacement. */
static unsigned OUTPUT_CHANNEL=DEFAULT_OUTPUT_CHANNEL;
/* Completion wait; the engine needs ~60 ms per band, 5 s was the synthetic check. */
static unsigned wait_ms=5000;
void lc_native_set_wait_ms(unsigned ms) { if (!running) wait_ms=ms ? ms:5000; }
static unsigned divert_on;
void lc_native_set_divert(unsigned on) { if (!running) divert_on=on; }

/* Observe completion without changing Canon's event notification. Each channel
 * owns its counter; no file I/O, allocation or waiting occurs in callbacks. */
static volatile unsigned completed[3];
static void completed_a(void *ctx)
{ completed[0]++;((void (*)(void *))0xff429d28)(ctx); }
static void completed_b(void *ctx)
{ completed[1]++;((void (*)(void *))0xff429d00)(ctx); }
static void completed_out(void *ctx)
{ completed[2]++;((void (*)(void *))0xff429d50)(ctx); }
static void trace_dma(uint32_t trace[12])
{
    const unsigned channels[3]={40,41,OUTPUT_CHANNEL};
    for (unsigned i=0;i<3;i++) {
        trace[4*i]=edmac_get_state(channels[i]);
        trace[4*i+1]=edmac_get_flags(channels[i]);
        trace[4*i+2]=edmac_get_address(channels[i]);
        trace[4*i+3]=edmac_get_pointer(channels[i]);
    }
}

static uint32_t physical(uint32_t address) { return lc_native_physical(address); }

/* LCM23 write-source router (see native-max.h). Register reads only, done
 * while the engine resources are held, like trace_dma. */
static void route_scan(struct lc_native_engine_result *r)
{
    r->out_conn=edmac_get_connection(OUTPUT_CHANNEL,EDMAC_DIR_WRITE);
    for (unsigned i=0;i<LC_NATIVE_WRITE_CHANNELS;i++) {
        unsigned ch=lc_native_write_channels[i];
        r->conn[i]=edmac_get_connection(ch,EDMAC_DIR_WRITE);
        if (ch==OUTPUT_CHANNEL || r->conn[i]!=LC_NATIVE_ENGINE_SOURCE) continue;
        if (r->listeners<LC_NATIVE_MAX_LISTENERS) {
            unsigned k=r->listeners;
            r->lst_ch[k]=ch;r->lst_state[k]=edmac_get_state(ch);
            r->lst_addr[k]=edmac_get_address(ch);r->lst_ptr0[k]=edmac_get_pointer(ch);
        }
        r->listeners++;
    }
}
/* Point every other listener of source 1 at a source nobody uses, so only
 * our output channel can take the engine output. Canon's ConnectWriteEDmac
 * writes the route register through EngDrvOut (shadow kept consistent). The
 * diverted channels are Canon's and are restored right after the wait, long
 * before the next sensor readout (job ~120 ms, readout ~1 s later). */
static void all_channels(uint32_t state[LC_NATIVE_WRITE_CHANNELS],uint32_t ptr[LC_NATIVE_WRITE_CHANNELS])
{
    for (unsigned i=0;i<LC_NATIVE_WRITE_CHANNELS;i++) {
        state[i]=edmac_get_state(lc_native_write_channels[i]);
        ptr[i]=edmac_get_pointer(lc_native_write_channels[i]);
    }
}
static unsigned route_divert(struct lc_native_engine_result *r)
{
    if (!divert_on || !r->listeners) return 0;
    if (r->listeners>LC_NATIVE_MAX_LISTENERS) return LC_NATIVE_ROUTE;
    r->divert_source=lc_native_free_source(r->conn);
    if (!r->divert_source) return LC_NATIVE_ROUTE;
    for (unsigned k=0;k<r->listeners;k++) {
        ConnectWriteEDmac(r->lst_ch[k],r->divert_source);
        r->diverted|=1u<<k;
        if (edmac_get_connection(r->lst_ch[k],EDMAC_DIR_WRITE)!=r->divert_source) return LC_NATIVE_ROUTE;
    }
    return 0;
}
static unsigned route_restore(struct lc_native_engine_result *r)
{
    unsigned ok=1;
    for (unsigned k=0;k<LC_NATIVE_MAX_LISTENERS;k++) if (r->diverted&(1u<<k)) {
        ConnectWriteEDmac(r->lst_ch[k],LC_NATIVE_ENGINE_SOURCE);
        if (edmac_get_connection(r->lst_ch[k],EDMAC_DIR_WRITE)!=LC_NATIVE_ENGINE_SOURCE) ok=0;
    }
    r->route_restored=ok;
    return ok;
}
static int ram(uint32_t address,unsigned bytes) { return lc_native_ram(address,bytes); }
static unsigned hash(const volatile uint8_t *p,unsigned bytes)
{
    unsigned h=2166136261u;
    for (unsigned i=0;i<bytes;i++) h=(h^p[i])*16777619u;
    return h;
}
static int rom_valid(void)
{
    /* Retain the proved engine fingerprint and pin the complete non-waiting
     * lock and unlock functions from this camera's verified ROM. */
    if (!is_camera("EOSM","2.0.2")) return 0;
    return *(volatile uint32_t *)TABLE_SITE==TABLE_INSTRUCTION &&
        *(volatile uint32_t *)0x1cd30==0xe92d47f0u &&
        hash((void *)0xff429b5c,0xa50)==0xf08dab7eu &&
        hash((void *)0xff48d608,0xd4)==0xa7dcd2d2u &&
        hash((void *)0xff2c6228,0xc4)==0xb9162c5bu &&
        hash((void *)0xff2c5eb4,0x100)==0xbb8642c4u &&
        hash((void *)0xff2c5848,0x218)==0xbfad5254u &&
        hash((void *)0xff2c5a60,0x60)==0x68de5c2bu &&
        *(volatile uint32_t *)0xff0c00e8==0xffa6ae90u &&
        *(volatile uint32_t *)0xff0c00ec==0x1900u &&
        *(volatile uint32_t *)0xff0c00f0==0x65c80u;
}
static void clean_table(uint32_t *regs,uint32_t *stack,uint32_t pc)
{
    (void)stack;
    if (!running || pc!=TABLE_SITE ||
        *(volatile uint32_t *)(NATIVE_STATE+20)!=(uint32_t)private_geometry) return;
    if (regs[1]!=TABLE_ADDRESS || regs[2]!=TABLE_BYTES) { table_error=1; return; }
    /* Test62/71/72 show the first job needs this writeback. Clean only Canon's
     * correction table, never captured pixels or the shared size descriptor. */
    unsigned expected=hash((void *)TABLE_ADDRESS,TABLE_BYTES),irq=cli();
    for (unsigned address=TABLE_ADDRESS&~31u;address<TABLE_ADDRESS+TABLE_BYTES;address+=32)
        __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 1" :: "r"(address) : "memory");
    __asm__ __volatile__("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
    sei(irq);
    if (hash((void *)(TABLE_ADDRESS+0x40000000u),TABLE_BYTES)!=expected) table_error=1;
    table_calls++;
}
unsigned lc_native_engine_prepare(uint32_t *geometry)
{
    if (!is_camera("EOSM","2.0.2")) return LC_NATIVE_ROM;
    if (installed || quarantined || running || !ram((uint32_t)geometry,64) ||
        !((uint32_t)geometry&0x40000000u)) return LC_NATIVE_ARGUMENT;
    if (!rom_valid()) return LC_NATIVE_ROM;
    unsigned error=patch_hook_function(TABLE_SITE,TABLE_INSTRUCTION,clean_table,"Composite RAW table visibility");
    if (error) return LC_NATIVE_TABLE;
    private_geometry=geometry; installed=1; return 0;
}
unsigned lc_native_engine_close(void)
{
    if (running || quarantined) return LC_NATIVE_BUSY;
    if (installed && unpatch_memory(TABLE_SITE)) return LC_NATIVE_TABLE;
    installed=0; private_geometry=NULL; return 0;
}
static int plane_valid(const struct lc_native_plane *p,int input) { return lc_native_plane_ok(p,input); }
static int overlap(unsigned a,unsigned na,unsigned b,unsigned nb)
{
    a=physical(a);b=physical(b);return a<b+nb && b<a+na;
}
unsigned lc_native_engine_max(const struct lc_native_plane *a,
    const struct lc_native_plane *b,const struct lc_native_plane *out,
    struct lc_native_engine_result *r)
{
    return lc_native_engine_max_ch(a,b,out,r,DEFAULT_OUTPUT_CHANNEL);
}
unsigned lc_native_engine_max_ch(const struct lc_native_plane *a,
    const struct lc_native_plane *b,const struct lc_native_plane *out,
    struct lc_native_engine_result *r,unsigned channel)
{
    return lc_native_engine_max_lock(a,b,out,r,channel,channel);
}
/* LCM29: output on `channel`, but lock the engine with the resource set of
 * `lock_channel`. Used to borrow channel 5 (only route the engine output
 * reaches, LCM28) while Live View keeps resource 5: the caller has already
 * taken channel 5 over from Live View and restores it afterwards. */
unsigned lc_native_engine_max_lock(const struct lc_native_plane *a,
    const struct lc_native_plane *b,const struct lc_native_plane *out,
    struct lc_native_engine_result *r,unsigned channel,unsigned lock_channel)
{
    /* LCM28: 5 = Canon's native route (verified exact with Live View
     * paused, LCM27); 0/3/4/6 = replacement routes under test. */
    if (channel!=0 && channel!=3 && channel!=4 && channel!=5 && channel!=6) return LC_NATIVE_ARGUMENT;
    if (lock_channel!=channel && !(channel==5 && lock_channel==3)) return LC_NATIVE_ARGUMENT;
    if (!running) OUTPUT_CHANNEL=channel;
    if (!r) return LC_NATIVE_ARGUMENT;
    memset(r,0,sizeof(*r));r->lock=r->setup=r->wait=r->event_delete=r->unlock=UNSET;
    r->resume_safe=1;r->output_channel=OUTPUT_CHANNEL;r->resource_delete=UNSET;
    if (quarantined) { r->resume_safe=0;return r->reason=LC_NATIVE_STOP; }
    if (!is_camera("EOSM","2.0.2")) return r->reason=LC_NATIVE_ROM;
    if (!installed || running || quarantined || !plane_valid(a,1) || !plane_valid(b,1) ||
        !plane_valid(out,0) || a->height!=b->height || a->height!=out->height)
        return r->reason=LC_NATIVE_ARGUMENT;
    unsigned bytes=out->height*out->pitch,route_bad=0;
    const uint32_t out_start=physical(out->pixels);
    if ((overlap(out->pixels,bytes,a->storage,a->bytes) && physical(out->pixels)!=physical(a->pixels)) ||
        (overlap(out->pixels,bytes,b->storage,b->bytes) && physical(out->pixels)!=physical(b->pixels)) ||
        overlap((unsigned)private_geometry,64,a->storage,a->bytes) ||
        overlap((unsigned)private_geometry,64,b->storage,b->bytes) ||
        overlap((unsigned)private_geometry,64,out->storage,out->bytes))
        return r->reason=LC_NATIVE_ARGUMENT;
    volatile uint32_t *state=(void *)NATIVE_STATE;
    if (!state[1] || !state[4] || state[2] || state[6]!=40 || state[7]!=2 ||
        state[8]!=41 || state[9]!=4 || state[10]!=5 || state[11] || !ram(state[5],64))
        return r->reason=LC_NATIVE_CONTEXT;
    /* Preserve every native dependency except the occupied write channel.
     * Registry observations never authorize DMA; the new entry must acquire
     * the complete replacement set atomically through Canon's try-lock. */
    static const uint32_t expected[10]={5,0x1000c,0x1000d,0x50002,0x50003,
        0x50006,0x80000,0x20001,0x30000,0x30001};
    struct lc_resource_snapshot snapshot;
    if (!lc_native_resources(&snapshot) || snapshot.count!=10 ||
        memcmp(snapshot.ids,expected,sizeof(expected))) return r->reason=LC_NATIVE_CONTEXT;
    /* Channel 5 keeps Canon's own resource list unchanged. */
    uint32_t ids[10];memcpy(ids,expected,sizeof(ids));ids[0]=lock_channel;
    unsigned started=(unsigned)get_us_clock(),old_resource=state[1],old_geometry=state[5];
    unsigned resource=((unsigned (*)(uint32_t *,unsigned))0xff2c5848)(ids,10);
    if (!resource) return r->reason=LC_NATIVE_RESOURCE;
    /* This native function atomically checks/acquires under Canon's scheduler
     * lock. A busy result(0xD) creates no waiter and cannot deadlock SSS. */
    r->lock=((unsigned (*)(unsigned))0xff2c6228)(resource);
    if (r->lock) { r->reason=LC_NATIVE_BUSY; goto done; }
    if (state[1]!=old_resource || state[2] || state[5]!=old_geometry ||
        state[6]!=40 || state[7]!=2 || state[8]!=41 || state[9]!=4 || state[10]!=5 || state[11]) { r->reason=LC_NATIVE_CONTEXT; goto unlock; }
    /* The registry is advisory for some Canon paths. Do not configure an
     * already active DMA, or unlock it (unlock itself resets channel setup). */
    r->channels[0]=edmac_get_state(40);r->channels[1]=edmac_get_state(41);
    r->channels[2]=edmac_get_state(OUTPUT_CHANNEL);
    if (r->channels[0] || r->channels[1] || r->channels[2]) {
        r->reason=LC_NATIVE_OWNERSHIP;r->resume_safe=0;quarantined=1;goto done;
    }
    for (unsigned i=0;i<16;i++) private_geometry[i]=((volatile unsigned *)old_geometry)[i];
    private_geometry[12]=a->width;private_geometry[13]=a->height;
    state[5]=(unsigned)private_geometry;state[10]=OUTPUT_CHANNEL;
    running=1;table_calls=table_error=0;
    state[2]=((unsigned (*)(const char *,unsigned))0x1e5f8)("LCNativeCheck",0);
    if (!state[2] || state[2]==UNSET) { state[2]=0;r->reason=LC_NATIVE_EVENT;goto restore; }
    uint32_t job[10]={a->pixels,b->pixels,10,10,10,10,0x8888dddd,3,2047,out->pixels};
    /* Same Canon setup/start/wait/stop sequence as FF42A3B4, with try-lock and
     * the real wait result retained. No software image arithmetic is used. */
    r->setup=((unsigned (*)(uint32_t *))0xff429e68)(job);
    if (r->setup) { r->reason|=LC_NATIVE_SETUP;goto stop; }
    completed[0]=completed[1]=completed[2]=0;
    RegisterEDmacCompleteCBR(40,completed_a,NULL);
    RegisterEDmacCompleteCBR(41,completed_b,NULL);
    RegisterEDmacCompleteCBR(OUTPUT_CHANNEL,completed_out,NULL);
    /* Snapshot only while all engine resources are owned and powered. */
    trace_dma(r->before_start);
    const uint32_t starts[3]={physical(a->pixels)-a->pitch,
        physical(b->pixels)-b->pitch,physical(out->pixels)};
    for (unsigned i=0;i<3;i++) {
        if (physical(r->before_start[4*i+2])!=starts[i] || r->before_start[4*i])
            goto ownership_lost;
    }
    route_scan(r);
    if (route_divert(r)) {
        r->reason|=LC_NATIVE_ROUTE;
        if (!route_restore(r)) route_bad=1;
        goto stop;                                   /* nothing started yet */
    }
    all_channels(r->all_state0,r->all_ptr0);
    ((void (*)(unsigned,unsigned))0x1c914)(OUTPUT_CHANNEL,0);
    ((void (*)(unsigned))0xff2c19ac)(0x55180);
    ((void (*)(unsigned,unsigned))0x1c914)(40,2);
    ((void (*)(unsigned,unsigned))0x1c914)(41,4);
    r->wait=((unsigned (*)(unsigned,unsigned,unsigned))0x1e7d4)(state[2],7,wait_ms);
    trace_dma(r->after_wait);
    all_channels(r->all_state1,r->all_ptr1);
    for (unsigned k=0;k<r->listeners && k<LC_NATIVE_MAX_LISTENERS;k++) r->lst_ptr1[k]=edmac_get_pointer(r->lst_ch[k]);
    if (r->diverted && !route_restore(r)) route_bad=1;
    r->table_calls=table_calls;r->table_error=table_error;
    for (unsigned i=0;i<3;i++) r->completed[i]=completed[i];
    if (r->wait) r->reason|=LC_NATIVE_WAIT;
    for (unsigned i=0;i<3;i++) {
        if (physical(r->after_wait[4*i+2])!=starts[i]) goto ownership_lost;
    }
stop:
    ((void (*)(unsigned))0xff2c19ac)(0x551b0);
    /* 0x1cb3c unregisters completion callbacks; it does NOT abort DMA.
     * Retain resources and buffers if any channel remains active below. */
    ((void (*)(unsigned))0x1cb3c)(OUTPUT_CHANNEL);
    ((void (*)(unsigned))0x1cb3c)(40);
    ((void (*)(unsigned))0x1cb3c)(41);
    /* Read DMA state only while our acquired engine resources keep it powered.
     * Never perform these reads in an ordinary passive capture observer. */
    r->channels[0]=edmac_get_state(40);r->channels[1]=edmac_get_state(41);r->channels[2]=edmac_get_state(OUTPUT_CHANNEL);
    /* LCM22: inputs completed, output channel 3 still armed at our own start
     * address with nothing written. Channel 3 is never configured by Live
     * View (LCM21/22 activity map), so the stuck transfer is ours: abort it
     * (AbortEDmac, RAM 0x1CA60: writes 3 to the channel's control register)
     * instead of quarantining the whole session. */
    /* Ownership proof is our own programmed start address (LCM28: any
     * output channel; LCM23-27 restricted this to channel 3). */
    if (r->channels[2] && !r->channels[0] && !r->channels[1] &&
        physical(edmac_get_address(OUTPUT_CHANNEL))==out_start) {
        ((void (*)(unsigned))0x1ca60)(OUTPUT_CHANNEL);
        unsigned t0=(unsigned)get_us_clock();
        while (edmac_get_state(OUTPUT_CHANNEL) && (unsigned)get_us_clock()-t0<20000u) continue;
        r->channels[2]=edmac_get_state(OUTPUT_CHANNEL);
        r->aborted=r->channels[2] ? 3:1;
    }
    if (r->channels[0] || r->channels[1] || r->channels[2]) {
        r->reason|=LC_NATIVE_STOP;r->resume_safe=0;quarantined=1;goto done;
    }
    r->event_delete=((unsigned (*)(unsigned))0x1e644)(state[2]);
    if (r->event_delete) { r->reason|=LC_NATIVE_DELETE;r->resume_safe=0;quarantined=1;goto done; }
    state[2]=0;
restore:
    r->table_calls=table_calls;r->table_error=table_error;
    if (r->setup==0 && (table_calls!=1 || table_error)) r->reason|=LC_NATIVE_TABLE;
    if (state[5]==(unsigned)private_geometry) state[5]=old_geometry;
    if (state[10]==OUTPUT_CHANNEL) state[10]=5;
    r->context_restored=state[5]==old_geometry && state[10]==5 && state[1]==old_resource;
    if (!r->context_restored) { r->reason|=LC_NATIVE_CONTEXT;r->resume_safe=0;quarantined=1;goto done; }
    running=0;
unlock:
    r->unlock=((unsigned (*)(unsigned))0xff2c5eb4)(resource);
    if (r->unlock) { r->reason|=LC_NATIVE_UNLOCK;r->resume_safe=0;quarantined=1; }
    goto done;
ownership_lost:
    /* A Canon path can reconfigure DMA without respecting the registry lock.
     * Stop our processing blocks, but do not unregister/abort a channel that
     * may now belong to that path. Keep events, hooks, buffers and locks pinned. */
    ((void (*)(unsigned))0xff2c19ac)(0x551b0);
    r->reason|=LC_NATIVE_OWNERSHIP;r->resume_safe=0;quarantined=1;
    r->table_calls=table_calls;r->table_error=table_error;
done:
    if (route_bad) { r->reason|=LC_NATIVE_ROUTE;r->resume_safe=0;quarantined=1; }
    if (!quarantined) {
        r->resource_delete=((unsigned (*)(unsigned))0xff2c5a60)(resource);
        if (r->resource_delete) {
            r->reason|=LC_NATIVE_RESOURCE;r->resume_safe=0;quarantined=1;
        }
    }
    r->elapsed_us=(unsigned)get_us_clock()-started;
    return r->reason;
}

/* LCM29: borrow channel 5 from Live View.
 * LCM28: the engine output (source 1) reaches only write channel 5; channels
 * 3 and 4 receive nothing, with or without Live View. Live View holds
 * resource 5 and keeps channel 5 armed on source 2 (its own frame output)
 * from ~0.3 s after each vsync until the next readout, idle in between.
 * Canon's per-channel EDMAC record (RAM table at *(0x1C404), 32 bytes per
 * channel; StartEDmac/AbortEDmac/Register*CBR, RAM 0x1C914/0x1CA60/0x1CB00/
 * 0x1CBE8): +0 abort flag, +4 control word (SetEDmac flags | start mode<<6),
 * +8 complete CBR, +12 abort CBR, +16 pop CBR, +20/+24/+28 their contexts.
 * Default (no-op) CBR address at *(0x1C830).
 * Borrow: only when armed and idle (pointer at start, source 2); save the
 * programmed address, size/offset registers, control word, source and the
 * record; point the abort CBR at the default so Live View sees no abort;
 * AbortEDmac; wait for idle. Return: route back to the saved source,
 * SetEDmac with the saved geometry, restore the whole record, StartEDmac
 * with the saved mode, then verify state/address/pointer/control/source. */
#define EDMAC_RECORDS 0x1c404u
#define EDMAC_DEFAULT_CBR 0x1c830u
static struct edmac_info borrow_info;
static uint32_t borrow_rec[8];
static volatile uint32_t *borrow_record(void)
{
    uint32_t table=*(volatile uint32_t *)EDMAC_RECORDS;
    return ram(table+5*32,32) ? (volatile uint32_t *)(table+5*32):NULL;
}
unsigned lc_native_borrow5(struct lc_native_borrow *b)
{
    memset(b,0,sizeof(*b));
    if (!rom_valid()) return b->reason=LC_NATIVE_ROM;
    if (running || quarantined) return b->reason=LC_NATIVE_BUSY;
    volatile uint32_t *rec=borrow_record();
    uint32_t dflt=*(volatile uint32_t *)EDMAC_DEFAULT_CBR;
    if (!rec || !dflt) return b->reason=LC_NATIVE_CONTEXT;
    b->state=edmac_get_state(5);b->addr=edmac_get_address(5);b->ptr=edmac_get_pointer(5);
    b->flags=edmac_get_flags(5);b->conn=edmac_get_connection(5,EDMAC_DIR_WRITE);
    for (unsigned i=0;i<8;i++) b->rec[i]=borrow_rec[i]=rec[i];
    if (b->state!=1 || physical(b->ptr)!=physical(b->addr) || b->conn!=2 || b->rec[1]!=b->flags)
        return b->reason=LC_NATIVE_CONTEXT;          /* not armed-and-idle as observed */
    borrow_info=edmac_get_info(5);
    unsigned irq=cli();
    if (edmac_get_state(5)!=1 || physical(edmac_get_pointer(5))!=physical(b->addr)) {
        sei(irq);return b->reason=LC_NATIVE_CONTEXT; /* data started meanwhile */
    }
    rec[3]=dflt;                                     /* abort CBR: Live View not told */
    ((void (*)(unsigned))0x1ca60)(5);                /* AbortEDmac */
    sei(irq);
    unsigned t0=(unsigned)get_us_clock();
    while (edmac_get_state(5) && (unsigned)get_us_clock()-t0<20000u) continue;
    b->abort_wait_us=(unsigned)get_us_clock()-t0;
    if (edmac_get_state(5)) { rec[3]=borrow_rec[3];quarantined=1;return b->reason=LC_NATIVE_STOP; }
    b->ok=1;
    return 0;
}
unsigned lc_native_return5(struct lc_native_borrow *b)
{
    if (!b->ok) return 0;
    volatile uint32_t *rec=borrow_record();
    if (!rec || edmac_get_state(5)) { quarantined=1;return b->reason|=LC_NATIVE_STOP; }
    ConnectWriteEDmac(5,b->conn);
    SetEDmac(5,(void *)b->addr,&borrow_info,(int)(b->flags&~0xC0u));
    for (unsigned i=0;i<8;i++) rec[i]=borrow_rec[i];
    ((void (*)(unsigned,unsigned))0x1c914)(5,(b->flags>>6)&3u);   /* StartEDmac */
    b->after_state=edmac_get_state(5);b->after_addr=edmac_get_address(5);b->after_ptr=edmac_get_pointer(5);
    b->after_flags=edmac_get_flags(5);b->after_conn=edmac_get_connection(5,EDMAC_DIR_WRITE);
    b->restored=b->after_state==1 && b->after_addr==b->addr && physical(b->after_ptr)==physical(b->addr) &&
        b->after_flags==b->flags && b->after_conn==b->conn;
    b->ok=0;
    if (!b->restored) { quarantined=1;return b->reason|=LC_NATIVE_CONTEXT; }
    return 0;
}

unsigned lc_native_engdrv(const uint32_t *regs,const uint32_t *vals,unsigned n)
{
    if (!rom_valid()) return LC_NATIVE_ROM;
    /* LCM33: image-pipeline registers 0xC0F01000-0xC0F3FFFC; never EDMAC
     * (pages 04/05/26/30) or sensor timing (06/07). */
    for (unsigned i=0;i<n;i++) {
        uint32_t r=regs[i],pg=(r>>12)&0xFFFu;
        if ((r&0xFFFC0003u)!=0xC0F00000u || pg<0xF01u || pg==0xF04u || pg==0xF05u || pg==0xF06u ||
            pg==0xF07u || pg==0xF26u || pg==0xF30u) return LC_NATIVE_ARGUMENT;
    }
    for (unsigned i=0;i<n;i++) ((void (*)(uint32_t,uint32_t))0xff2c1694)(regs[i],vals[i]);
    return 0;
}
#define JOB_LIST 0x55020u
unsigned lc_native_job_list(uint32_t *pairs,unsigned max_pairs)
{
    if (!rom_valid()) return 0;
    const volatile uint32_t *l=(const volatile uint32_t *)JOB_LIST;
    unsigned n=0;
    while (n<max_pairs && l[2*n]!=0xFFFFFFFFu && (l[2*n]&0xFFF00000u)==0xC0F00000u) {
        pairs[2*n]=l[2*n];pairs[2*n+1]=l[2*n+1];n++;
    }
    return n;
}

/* RAM/ROM only. No powered peripheral reads, lock or engine job. A matching
 * snapshot is a prerequisite, not proof that Canon will grant the resource. */
int lc_native_snapshot(uint32_t state[12])
{
    memset(state,0,12*sizeof(*state));
    if (!rom_valid()) return 0;
    unsigned irq=cli();
    for (unsigned i=0;i<12;i++) state[i]=((volatile uint32_t *)NATIVE_STATE)[i];
    sei(irq);
    return 1;
}

static int read_ram_word(void *unused,uint32_t address,uint32_t *value)
{
    (void)unused;
    if (!ram(address,4)) return 0;
    *value=*(volatile uint32_t *)address;return 1;
}
int lc_native_resources(struct lc_resource_snapshot *snapshot)
{
    /* Validate the actual registry-walk code and its literal addresses before
     * interpreting RAM structures. No ENGIO/DMA reads or lock acquisition. */
    if (!rom_valid() || hash((void *)0xff2c5bf8,0xcc)!=0x78ca9956u ||
        *(volatile uint32_t *)0xff2c59ac!=0x4c3f8u ||
        *(volatile uint32_t *)0xff2c59f4!=0xff8ffab0u) return 0;
    uint32_t node_signature=*(volatile uint32_t *)0xff8ffab0;
    uint32_t lock_signature=*(volatile uint32_t *)0xff8ffab4;
    unsigned irq=cli();
    uint32_t lock=*((volatile uint32_t *)NATIVE_STATE+1);
    int ok=lc_resource_read(read_ram_word,NULL,lock,0x4c3f8,
        node_signature,lock_signature,snapshot);
    sei(irq);return ok;
}
unsigned lc_native_resource_list(uint32_t *ids,uint32_t *states,unsigned max)
{
    /* Same ROM fingerprint and literal checks as lc_native_resources. */
    if (!rom_valid() || hash((void *)0xff2c5bf8,0xcc)!=0x78ca9956u ||
        *(volatile uint32_t *)0xff2c59ac!=0x4c3f8u ||
        *(volatile uint32_t *)0xff2c59f4!=0xff8ffab0u) return 0;
    uint32_t node_signature=*(volatile uint32_t *)0xff8ffab0;
    unsigned irq=cli();
    unsigned n=lc_resource_enum(read_ram_word,NULL,0x4c3f8,node_signature,ids,states,max);
    sei(irq);return n;
}
unsigned lc_native_hold(const uint32_t *ids,unsigned n,uint32_t *handle)
{
    *handle=0;
    if (!rom_valid()) return LC_NATIVE_ROM;
    if (running || quarantined || !n || n>16) return LC_NATIVE_ARGUMENT;
    static uint32_t list[16];
    for (unsigned i=0;i<n;i++) list[i]=ids[i];
    unsigned h=((unsigned (*)(uint32_t *,unsigned))0xff2c5848)(list,n);
    if (!h) return LC_NATIVE_RESOURCE;
    if (((unsigned (*)(unsigned))0xff2c6228)(h)) {   /* busy: create only */
        ((unsigned (*)(unsigned))0xff2c5a60)(h);
        return LC_NATIVE_BUSY;
    }
    *handle=h;
    return 0;
}
unsigned lc_native_release(uint32_t handle)
{
    if (!handle) return 0;
    unsigned e=((unsigned (*)(unsigned))0xff2c5eb4)(handle) ? LC_NATIVE_UNLOCK:0;
    if (((unsigned (*)(unsigned))0xff2c5a60)(handle)) e|=LC_NATIVE_RESOURCE;
    return e;
}
/* LCX2: 1 while an engine job owns its resources (powered), for observers. */
unsigned lc_native_job_active(void) { return running && !quarantined; }
