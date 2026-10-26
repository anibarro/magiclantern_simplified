/* Bounded observations of EOS M live-view Lucky-table transfers. No table,
 * register argument, hardware register or Canon property is changed here. */
#include "dryos.h"
#include "lens.h"
#include "menu.h"
#include "patch.h"
#include "propvalues.h"
#include "xpan-mode.h"
#include "xpan-live-color-probe.h"
#include "xpan-live-native.h"

#if !defined(CONFIG_EOSM) || defined(CONFIG_QEMU) || !defined(CONFIG_XPAN)
#error Live color observations require the physical EOS M candidate.
#endif

#define PROBE_WORDS 1024
#define PROBE_LIMIT 8
#define PROBE_COMMON 0x285bc
#define PROBE_DIANA 0x28c38
#define PROBE_INSTRUCTION 0xe1a02009 /* MOV R2,R9: original table source. */

/* The photo build needs these guarded Canon transfer hooks for the hardware
 * Live View LUT, but it does not collect tables, expose Debug menus or write logs. */
static int release_installed, release_attempted;

void xpan_live_color_probe_queue_unknown(void) {}

static void release_hook(uint32_t *regs, uint32_t *stack, uint32_t pc)
{
    xpan_live_native_transfer(regs, stack, pc);
}

void xpan_live_color_probe_poll(struct card_info *card)
{
    /* Photo XPan, or the movie-mode LUT experiment (same two hooks). */
    if (!card || !lv || PLAY_OR_QR_MODE ||
        (is_movie_mode() ? !xpan_movie_lut_wanted() : !xpan_photo_enabled())) return;
    if (!release_attempted)
    {
        release_attempted=1;
        /* Keep the exact firmware signature checks used by the proven observer. */
        if (*(volatile uint32_t *)0xff0c00e8==0xffa6ae90 &&
            *(volatile uint32_t *)0xff0c00ec==0x1900 &&
            *(volatile uint32_t *)0xff0c00f0==0x65c80 &&
            *(volatile uint32_t *)0xffff462c==0xe3a00037 &&
            *(volatile uint32_t *)0xffff4630==0xee060f15 &&
            *(volatile uint32_t *)PROBE_COMMON==PROBE_INSTRUCTION &&
            *(volatile uint32_t *)PROBE_DIANA==PROBE_INSTRUCTION &&
            *(volatile uint32_t *)0x28564==0xe92d47ff &&
            *(volatile uint32_t *)0x28bdc==0xe92d47ff &&
            *(volatile uint32_t *)0x28ba4==0xe92d4008 &&
            *(volatile uint32_t *)0x28bc0==0xe92d4008 &&
            *(volatile uint32_t *)0x285b8==0xe3a03a01 &&
            *(volatile uint32_t *)0x28c34==0xe3a03a01 &&
            *(volatile uint32_t *)0x287a8==0xc0f20000 &&
            *(volatile uint32_t *)0x287ac==0xc0f3c000 &&
            *(volatile uint32_t *)0xffa91b4c==PROBE_INSTRUCTION &&
            *(volatile uint32_t *)0xffa921c8==PROBE_INSTRUCTION)
        {
            if (!patch_hook_function(PROBE_COMMON,PROBE_INSTRUCTION,release_hook,"XPan LCD color common"))
                release_installed|=1;
            if (!patch_hook_function(PROBE_DIANA,PROBE_INSTRUCTION,release_hook,"XPan LCD color Diana"))
                release_installed|=2;
        }
    }
    xpan_live_native_init(release_installed);
}
