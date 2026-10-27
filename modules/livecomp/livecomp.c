/* GPL-2.0-or-later. Standard ML module callbacks only. */
#include "dryos.h"
#include "module.h"
#include "gui-common.h"
#include "livecomp-lv.h"
#include "camera-profile.h"

struct lc_camera_profile livecomp_camera;
static unsigned initialized;
/* Seen presses are either synthetic long presses or presses in a menu. Their
 * physical release must not become a second action or start after menu exit. */
static unsigned set_release_blocked;

static unsigned livecomp_init(void)
{
    initialized=0;
    set_release_blocked=0;
    if (!lc_camera_configure(&livecomp_camera)) return 1;
    initialized=1;
    livecomp_lv_poll(NULL);
    return 0;
}
static unsigned livecomp_poll(unsigned unused)
{
    (void)unused;
    if (initialized) livecomp_lv_poll(get_ml_card());
    return CBR_RET_CONTINUE;
}
static unsigned livecomp_key(unsigned key)
{
    return initialized ? livecomp_lv_key(key):1;
}
/* This core does not map EOS M's combined Q/SET into MODULE_KEY_Q.
 * The shared event layout is camera-independent; only the guarded code varies. */
static unsigned livecomp_raw_key(unsigned context)
{
    if (!initialized) return 1;
    const struct event *event=(const struct event *)context;
    if (event->type!=0) return 1;
    if (event->param==livecomp_camera.q_set_event ||
        event->param==livecomp_camera.set_press_event) {
        set_release_blocked=1;
        if (event->param==livecomp_camera.q_set_event)
            return livecomp_lv_key(MODULE_KEY_Q);
    }
    if (event->param==livecomp_camera.set_release_event) {
        /* Core longpress_check emits a fake release before the real button is
         * released. Keep suppression until the physical release arrives. */
        if (IS_FAKE(event)) return 1;
        if (set_release_blocked) { set_release_blocked=0;return 1; }
        /* The core swallowed the physical Q/SET down in idle LV, but forwards
         * its up. Reuse all existing Off/menu/recording/shutter/debounce gates. */
        return livecomp_lv_key(MODULE_KEY_Q);
    }
    return 1;
}
static unsigned livecomp_deinit(void)
{
    if (initialized) livecomp_lv_shutdown();
    return 0;
}
MODULE_INFO_START()
    MODULE_INIT(livecomp_init)
    MODULE_DEINIT(livecomp_deinit)
MODULE_INFO_END()
MODULE_CBRS_START()
    MODULE_CBR(CBR_SHOOT_TASK, livecomp_poll, 0)
    MODULE_CBR(CBR_KEYPRESS_RAW, livecomp_raw_key, 0)
    MODULE_CBR(CBR_KEYPRESS, livecomp_key, 0)
    MODULE_CBR(CBR_VSYNC, livecomp_lv_vsync, 0)
MODULE_CBRS_END()
