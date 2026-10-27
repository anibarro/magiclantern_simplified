/* GPL-2.0-or-later. Bounded read-only walk of EOS M's resource registry.
 * Layout follows the fingerprinted FF2C5BF8 checker. A snapshot is diagnostic;
 * it NEVER grants ownership or replaces Canon's atomic resource lock. */
#ifndef LIVECOMP_RESOURCE_SNAPSHOT_H
#define LIVECOMP_RESOURCE_SNAPSHOT_H
#include <stdint.h>
#define LC_RESOURCE_LIMIT 16u
struct lc_resource_snapshot { uint32_t count,ids[LC_RESOURCE_LIMIT],busy; };
typedef int (*lc_read_word)(void *,uint32_t,uint32_t *);
static inline int lc_resource_read(lc_read_word read,void *ctx,uint32_t lock,
    uint32_t manager,uint32_t node_signature,uint32_t lock_signature,
    struct lc_resource_snapshot *out)
{
    struct lc_resource_snapshot s={0};
    uint32_t signature,ids,heads;
    if (!read(ctx,lock,&signature) || signature!=lock_signature ||
        !read(ctx,lock+20,&ids) || !read(ctx,lock+24,&s.count) ||
        !s.count || s.count>LC_RESOURCE_LIMIT || !read(ctx,manager+4,&heads)) return 0;
    for (unsigned i=0;i<s.count;i++) {
        if (!read(ctx,ids+4*i,&s.ids[i])) return 0;
        unsigned type=s.ids[i]>>16;
        if (type>=16) return 0;
        uint32_t head=heads+8*type,node;
        if (!read(ctx,head+4,&node)) return 0;
        unsigned found=0;
        for (unsigned n=0;n<64 && node!=head;n++) {
            uint32_t id,busy;
            if (node<4 || !read(ctx,node-4,&signature) || signature!=node_signature ||
                !read(ctx,node+8,&id) || !read(ctx,node+12,&busy)) return 0;
            if (id==s.ids[i]) { if (busy==1) s.busy|=1u<<i;found=1;break; }
            if (!read(ctx,node+4,&node)) return 0;
        }
        if (!found) return 0;
    }
    *out=s;return 1;
}
/* LCM39: every node of every resource type (16 types, <= 64 nodes each),
 * same validated layout: list head heads+8*type, node-4 signature, node+4
 * next, node+8 id, node+12 state (1 = locked). Read-only. Returns the
 * number of nodes stored (<= max), 0 on any inconsistency. */
static inline unsigned lc_resource_enum(lc_read_word read,void *ctx,uint32_t manager,
    uint32_t node_signature,uint32_t *ids,uint32_t *states,unsigned max)
{
    uint32_t heads,signature;
    unsigned n=0;
    if (!read(ctx,manager+4,&heads)) return 0;
    for (unsigned type=0;type<16;type++) {
        uint32_t head=heads+8*type,node;
        if (!read(ctx,head+4,&node)) return 0;
        for (unsigned k=0;k<64 && node!=head;k++) {
            uint32_t id,st;
            if (node<4 || !read(ctx,node-4,&signature) || signature!=node_signature ||
                !read(ctx,node+8,&id) || !read(ctx,node+12,&st)) return 0;
            if ((id>>16)!=type) return 0;
            if (n<max) { ids[n]=id;states[n]=st;n++; }
            if (!read(ctx,node+4,&node)) return 0;
        }
    }
    return n;
}
#endif
