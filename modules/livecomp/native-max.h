#ifndef LIVECOMP_NATIVE_MAX_H
#define LIVECOMP_NATIVE_MAX_H
#include <stdint.h>
#include "resource-snapshot.h"

/* A view is usable only while its caller owns the complete backing allocation.
 * Address validation does not create ownership of a Canon capture buffer. */
struct lc_native_plane {
    uint32_t storage,bytes,pixels,width,height,pitch;
};
struct lc_native_engine_result {
    uint32_t reason,lock,setup,wait,event_delete,unlock,elapsed_us;
    uint32_t output_channel,resource_delete;
    uint32_t completed[3],before_start[12],after_wait[12];
    uint32_t channels[3],table_calls,table_error,context_restored,resume_safe;
};
enum {
    LC_NATIVE_ARGUMENT=1,LC_NATIVE_ROM=2,LC_NATIVE_BUSY=4,
    LC_NATIVE_EVENT=8,LC_NATIVE_SETUP=16,LC_NATIVE_WAIT=32,
    LC_NATIVE_STOP=64,LC_NATIVE_DELETE=128,LC_NATIVE_UNLOCK=256,
    LC_NATIVE_TABLE=512,LC_NATIVE_CONTEXT=1024,LC_NATIVE_RESOURCE=2048,LC_NATIVE_OWNERSHIP=4096
};
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
int lc_native_snapshot(uint32_t state[12]);
int lc_native_resources(struct lc_resource_snapshot *snapshot);
#endif
