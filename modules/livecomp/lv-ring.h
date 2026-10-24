#ifndef LIVECOMP_LV_RING_H
#define LIVECOMP_LV_RING_H
/* Single CPU; the worker holds cli/sei around all queue transitions.
 * No slot can be rearmed while READY or being consumed by the worker. */
#define LC_LV_SLOTS 3
enum lc_slot_state { LC_FREE, LC_CAPTURING, LC_READY, LC_PROCESSING };
struct lc_ring {
    volatile unsigned state[LC_LV_SLOTS], sequence[LC_LV_SLOTS], issued, next;
    volatile int pending;
};
static void lc_ring_init(struct lc_ring *r)
{
    for (unsigned i=0;i<LC_LV_SLOTS;i++) r->state[i]=r->sequence[i]=0;
    r->issued=r->next=0;r->pending=-1;
}
static int lc_ring_complete(struct lc_ring *r)
{
    int slot=r->pending;
    if (slot<0 || r->state[slot]!=LC_CAPTURING) return -1;
    r->state[slot]=LC_READY;r->pending=-1;return slot;
}
static int lc_ring_arm(struct lc_ring *r)
{
    if (r->pending>=0) return -1;
    for (unsigned n=0;n<LC_LV_SLOTS;n++) {
        unsigned slot=(r->next+n)%LC_LV_SLOTS;
        if (r->state[slot]!=LC_FREE) continue;
        r->state[slot]=LC_CAPTURING;r->sequence[slot]=++r->issued;
        r->pending=slot;r->next=(slot+1)%LC_LV_SLOTS;return slot;
    }
    return -1;
}
static int lc_ring_take(struct lc_ring *r)
{
    int slot=-1;
    for (unsigned i=0;i<LC_LV_SLOTS;i++)
        if (r->state[i]==LC_READY && (slot<0 || r->sequence[i]<r->sequence[slot])) slot=i;
    if (slot>=0) r->state[slot]=LC_PROCESSING;
    return slot;
}
static int lc_ring_release(struct lc_ring *r,unsigned slot)
{
    if (slot>=LC_LV_SLOTS || r->state[slot]!=LC_PROCESSING) return 0;
    r->state[slot]=LC_FREE;return 1;
}
#endif
