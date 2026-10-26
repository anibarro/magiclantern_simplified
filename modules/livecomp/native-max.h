#ifndef LIVECOMP_NATIVE_MAX_H
#define LIVECOMP_NATIVE_MAX_H
#include <stdint.h>
#include "resource-snapshot.h"

/* A view is usable only while its caller owns the complete backing allocation.
 * Address validation does not create ownership of a Canon capture buffer. */
struct lc_native_plane {
    uint32_t storage,bytes,pixels,width,height,pitch;
};
/* Pure argument checks shared with the host tests (no hardware access).
 * Input planes need 4 halo rows before AND after the band inside storage;
 * the band must start a whole number of rows after the storage start. */
static inline uint32_t lc_native_physical(uint32_t address) { return address&~0x40000000u; }
static inline int lc_native_ram(uint32_t address,unsigned bytes)
{
    unsigned p=lc_native_physical(address);
    return bytes && bytes<0x10000000u && !(address&3) && p>=0x10000u && p<=0x10000000u-bytes;
}
static inline int lc_native_plane_ok(const struct lc_native_plane *p,int input)
{
    if (!p || p->width!=5280 || p->pitch!=9240 || !p->height ||
        p->height>1930 || (p->height&1) || !lc_native_ram(p->storage,p->bytes) ||
        !(p->pixels&0x40000000u)) return 0;
    unsigned lo=lc_native_physical(p->storage),start=lc_native_physical(p->pixels);
    unsigned halo=input ? 4*p->pitch:0;
    unsigned length=p->height*p->pitch;
    return lc_native_ram(p->pixels,length) && start>=lo+halo && start<=lo+p->bytes &&
        !((start-lo)%p->pitch) &&
        length+halo<=lo+p->bytes-start;
}
/* Write-source router. ConnectWriteEDmac (RAM 0x1C6A8) stores the source
 * connection of each write channel in its own register: 0xC0F05000+4*ch for
 * ch<16, 0xC0F0501C for ch 16, else 0xC0F05200+4*((ch<32 ? ch-1:ch+6)&15).
 * Several channels may name the same source. Canon's engine setup (FF429E68)
 * always routes the engine output to source 1, whatever the output channel.
 * LCM22 evidence: inputs consumed completely, output channel 3 got 0 bytes. */
#define LC_NATIVE_ENGINE_SOURCE 1u
#define LC_NATIVE_WRITE_CHANNELS 16u
#define LC_NATIVE_MAX_LISTENERS 4u
static const uint8_t lc_native_write_channels[LC_NATIVE_WRITE_CHANNELS]={
    0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x10,0x11,0x12,0x13,0x14,0x15,0x16,0x20,0x21};
static inline uint32_t lc_native_write_route_reg(unsigned ch)
{
    if (ch<16) return 0xC0F05000u+4u*ch;
    if (ch==16) return 0xC0F0501Cu;
    return 0xC0F05200u+4u*((ch<32 ? ch-1u:ch+6u)&15u);
}
/* Lowest source >= 3 that no write channel names (0 = sensor RAW used by the
 * RAW slurp, 1 = engine output, 2 = Live View's channel 5). 0 if none. */
static inline unsigned lc_native_free_source(const uint32_t conn[LC_NATIVE_WRITE_CHANNELS])
{
    for (unsigned s=3;s<32;s++) {
        unsigned used=0;
        for (unsigned i=0;i<LC_NATIVE_WRITE_CHANNELS;i++) if (conn[i]==s) used=1;
        if (!used) return s;
    }
    return 0;
}
struct lc_native_engine_result {
    uint32_t reason,lock,setup,wait,event_delete,unlock,elapsed_us;
    uint32_t output_channel,resource_delete;
    uint32_t completed[3],before_start[12],after_wait[12];
    uint32_t channels[3],table_calls,table_error,context_restored,resume_safe;
    /* LCM23 router evidence, sampled after setup and before start. */
    uint32_t conn[LC_NATIVE_WRITE_CHANNELS]; /* source of each write channel */
    uint32_t out_conn;                       /* output channel's source (expect 1) */
    uint32_t listeners;                      /* other channels on source 1 */
    uint32_t lst_ch[LC_NATIVE_MAX_LISTENERS],lst_state[LC_NATIVE_MAX_LISTENERS];
    uint32_t lst_addr[LC_NATIVE_MAX_LISTENERS],lst_ptr0[LC_NATIVE_MAX_LISTENERS],lst_ptr1[LC_NATIVE_MAX_LISTENERS];
    uint32_t diverted;      /* bit i: listener i moved to divert_source for the job */
    uint32_t divert_source,route_restored;
    uint32_t aborted;       /* 1: stuck output aborted, 3: abort did not idle it */
    /* LCM27: every write channel just before start and right after the wait,
     * to see whether the engine output lands on any other channel. */
    uint32_t all_state0[LC_NATIVE_WRITE_CHANNELS],all_ptr0[LC_NATIVE_WRITE_CHANNELS];
    uint32_t all_state1[LC_NATIVE_WRITE_CHANNELS],all_ptr1[LC_NATIVE_WRITE_CHANNELS];
};
enum {
    LC_NATIVE_ARGUMENT=1,LC_NATIVE_ROM=2,LC_NATIVE_BUSY=4,
    LC_NATIVE_EVENT=8,LC_NATIVE_SETUP=16,LC_NATIVE_WAIT=32,
    LC_NATIVE_STOP=64,LC_NATIVE_DELETE=128,LC_NATIVE_UNLOCK=256,
    LC_NATIVE_TABLE=512,LC_NATIVE_CONTEXT=1024,LC_NATIVE_RESOURCE=2048,LC_NATIVE_OWNERSHIP=4096,
    LC_NATIVE_ROUTE=8192
};
/* 1: move other armed listeners of source 1 away for the job (LCM23). */
void lc_native_set_divert(unsigned on);
/* Prepare/remove hooks from the idle worker. Geometry must be persistent,
 * uncached, 64-byte RAM storage outside all image ranges. */
unsigned lc_native_engine_prepare(uint32_t *geometry);
unsigned lc_native_engine_close(void);
/* The caller must exclusively own all input/output allocations. No properties,
 * ML heap allocations or file I/O occur here. Canon's temporary completion
 * event has a bounded lifetime. Resource acquisition never waits. */
unsigned lc_native_engine_max(const struct lc_native_plane *a,
    const struct lc_native_plane *b,const struct lc_native_plane *out,
    struct lc_native_engine_result *result);
/* Same job with an explicit output write channel: 5 (Canon's native route,
 * only usable when Canon's resource 5 is free) or 3. */
unsigned lc_native_engine_max_ch(const struct lc_native_plane *a,
    const struct lc_native_plane *b,const struct lc_native_plane *out,
    struct lc_native_engine_result *r,unsigned channel);
/* LCM29: borrow Canon's channel 5 from Live View for one engine job.
 * Live View keeps it armed and idle (source 2) between readouts. */
struct lc_native_borrow {
    uint32_t reason,ok,state,addr,ptr,flags,conn,rec[8];
    uint32_t abort_wait_us,restored,after_state,after_addr,after_ptr,after_flags,after_conn;
};
unsigned lc_native_borrow5(struct lc_native_borrow *b);
/* LCM30: Canon's engine job configuration list (ENGIO pairs written by
 * setup FF429E68 at RAM 0x55020, seen in the LCM24 trace). Returns pairs. */
unsigned lc_native_job_list(uint32_t *pairs,unsigned max_pairs);
/* LCM31: write engine-area registers through Canon's EngDrvOut (FF2C1694,
 * keeps the ENGIO shadow consistent). Only 0xC0F08000-0xC0F08FFC. */
unsigned lc_native_engdrv(const uint32_t *regs,const uint32_t *vals,unsigned n);
unsigned lc_native_return5(struct lc_native_borrow *b);
/* Output on `channel`, engine resources locked with `lock_channel`'s set
 * (only channel 5 with lock 3: Live View holds resource 5). */
unsigned lc_native_engine_max_lock(const struct lc_native_plane *a,
    const struct lc_native_plane *b,const struct lc_native_plane *out,
    struct lc_native_engine_result *r,unsigned channel,unsigned lock_channel);
void lc_native_set_wait_ms(unsigned ms);
int lc_native_snapshot(uint32_t state[12]);
int lc_native_resources(struct lc_resource_snapshot *snapshot);
#endif
