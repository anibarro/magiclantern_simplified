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

static uint32_t physical(uint32_t address) { return address&~0x40000000u; }
static int ram(uint32_t address,unsigned bytes)
{
    unsigned p=physical(address);
    return bytes && bytes<0x10000000u && !(address&3) &&
        p>=0x10000u && p<=0x10000000u-bytes;
}
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
static int plane_valid(const struct lc_native_plane *p,int input)
{
    if (!p || p->width!=5280 || p->pitch!=9240 || !p->height ||
        p->height>1930 || (p->height&1) || !ram(p->storage,p->bytes) ||
        !(p->pixels&0x40000000u)) return 0;
    unsigned lo=physical(p->storage),start=physical(p->pixels);
    unsigned halo=input ? 4*p->pitch:0;
    unsigned length=p->height*p->pitch;
    return ram(p->pixels,length) && start>=lo+halo && start<=lo+p->bytes &&
        !((start-lo)%p->pitch) &&
        length+halo<=lo+p->bytes-start;
}
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
    if (channel!=3 && channel!=5) return LC_NATIVE_ARGUMENT;
    if (!running) OUTPUT_CHANNEL=channel;
    if (!r) return LC_NATIVE_ARGUMENT;
    memset(r,0,sizeof(*r));r->lock=r->setup=r->wait=r->event_delete=r->unlock=UNSET;
    r->resume_safe=1;r->output_channel=OUTPUT_CHANNEL;r->resource_delete=UNSET;
    if (quarantined) { r->resume_safe=0;return r->reason=LC_NATIVE_STOP; }
    if (!is_camera("EOSM","2.0.2")) return r->reason=LC_NATIVE_ROM;
    if (!installed || running || quarantined || !plane_valid(a,1) || !plane_valid(b,1) ||
        !plane_valid(out,0) || a->height!=b->height || a->height!=out->height)
        return r->reason=LC_NATIVE_ARGUMENT;
    unsigned bytes=out->height*out->pitch;
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
    uint32_t ids[10];memcpy(ids,expected,sizeof(ids));ids[0]=OUTPUT_CHANNEL;
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
    ((void (*)(unsigned,unsigned))0x1c914)(OUTPUT_CHANNEL,0);
    ((void (*)(unsigned))0xff2c19ac)(0x55180);
    ((void (*)(unsigned,unsigned))0x1c914)(40,2);
    ((void (*)(unsigned,unsigned))0x1c914)(41,4);
    r->wait=((unsigned (*)(unsigned,unsigned,unsigned))0x1e7d4)(state[2],7,wait_ms);
    trace_dma(r->after_wait);
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
    if (!quarantined) {
        r->resource_delete=((unsigned (*)(unsigned))0xff2c5a60)(resource);
        if (r->resource_delete) {
            r->reason|=LC_NATIVE_RESOURCE;r->resume_safe=0;quarantined=1;
        }
    }
    r->elapsed_us=(unsigned)get_us_clock()-started;
    return r->reason;
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
