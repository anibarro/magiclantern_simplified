/* GPL-2.0-or-later. EXPERIMENTAL (livecomp-hwx branch, LC_HWX builds only).
 * Canon resource registry walk and non-blocking hold/release for the
 * hardware-merge experiments; ported from the LCM39/LCM40 code (removed from
 * the product in LCM41). Every ROM routine used is fingerprinted first.
 * Registry: manager 0x4C3F8, 16 types, node-4 signature, node+4 next,
 * node+8 id, node+12 state (1 = locked). Create FF2C5848, try-lock
 * FF2C6228 (never waits), unlock FF2C5EB4, delete FF2C5A60. */
#ifndef LIVECOMP_HWX_RES_H
#define LIVECOMP_HWX_RES_H
#include <stdint.h>

static unsigned hwx_hash(const volatile uint8_t *p,unsigned bytes)
{
    unsigned h=2166136261u;
    for (unsigned i=0;i<bytes;i++) h=(h^p[i])*16777619u;
    return h;
}
static int hwx_rom_valid(void)
{
    if (!is_camera("EOSM","2.0.2")) return 0;
    return hwx_hash((void *)0xff2c6228,0xc4)==0xb9162c5bu &&
        hwx_hash((void *)0xff2c5eb4,0x100)==0xbb8642c4u &&
        hwx_hash((void *)0xff2c5848,0x218)==0xbfad5254u &&
        hwx_hash((void *)0xff2c5a60,0x60)==0x68de5c2bu &&
        hwx_hash((void *)0xff2c5bf8,0xcc)==0x78ca9956u &&
        *(volatile uint32_t *)0xff2c59ac==0x4c3f8u &&
        *(volatile uint32_t *)0xff2c59f4==0xff8ffab0u;
}
static int hwx_ram(uint32_t a) { const uint32_t p=a&~0x40000000u; return !(a&3) && p>=0x10000u && p<0x10000000u; }
/* Every node of every resource type; returns the number stored, 0 on any
 * inconsistency (read-only, interrupts off for a consistent walk). */
static unsigned hwx_resource_list(uint32_t *ids,uint32_t *states,unsigned max)
{
    if (!hwx_rom_valid()) return 0;
    const uint32_t sig=*(volatile uint32_t *)0xff8ffab0,manager=0x4c3f8u;
    unsigned n=0,bad=0;
    const uint32_t irq=cli();
    const uint32_t heads=*(volatile uint32_t *)(manager+4);
    if (!hwx_ram(heads)) bad=1;
    for (unsigned type=0;type<16 && !bad;type++) {
        const uint32_t head=heads+8*type;
        uint32_t node=*(volatile uint32_t *)(head+4);
        for (unsigned k=0;k<64 && node!=head;k++) {
            if (!hwx_ram(node) || *(volatile uint32_t *)(node-4)!=sig) { bad=1;break; }
            const uint32_t id=*(volatile uint32_t *)(node+8),st=*(volatile uint32_t *)(node+12);
            if ((id>>16)!=type) { bad=1;break; }
            if (n<max) { ids[n]=id;states[n]=st;n++; }
            node=*(volatile uint32_t *)(node+4);
        }
    }
    sei(irq);
    return bad ? 0:n;
}
/* Create a resource set and try-lock it (never waits). 0 = held. */
static unsigned hwx_hold(const uint32_t *ids,unsigned n,uint32_t *handle)
{
    *handle=0;
    if (!hwx_rom_valid()) return 2;
    if (!n || n>16) return 1;
    static uint32_t list[16];
    for (unsigned i=0;i<n;i++) list[i]=ids[i];
    const unsigned h=((unsigned (*)(uint32_t *,unsigned))0xff2c5848)(list,n);
    if (!h) return 3;
    if (((unsigned (*)(unsigned))0xff2c6228)(h)) { ((unsigned (*)(unsigned))0xff2c5a60)(h);return 4; }
    *handle=h;return 0;
}
static unsigned hwx_release(uint32_t handle)
{
    if (!handle) return 0;
    unsigned e=((unsigned (*)(unsigned))0xff2c5eb4)(handle) ? 1:0;
    if (((unsigned (*)(unsigned))0xff2c5a60)(handle)) e|=2;
    return e;
}
#endif
