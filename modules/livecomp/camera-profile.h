#ifndef LIVECOMP_CAMERA_PROFILE_H
#define LIVECOMP_CAMERA_PROFILE_H
/* GPL-2.0-or-later. Runtime camera selection, never platform header inclusion.
 * Add a model only after validating its DMA, cache aliases and key adapter.
 * No hardware access occurs while selecting a profile. */
#include <stdint.h>

struct lc_camera_profile {
    uint32_t cache_alias_mask;
    unsigned raw_channel;
    int gui_idle;
    int q_set_event;
    int set_press_event;
    int set_release_event;
};

extern struct lc_camera_profile livecomp_camera;

static inline int lc_camera_configure(struct lc_camera_profile *profile)
{
    *profile=(struct lc_camera_profile){0};
    if (!is_camera("EOSM", "2.0.2")) return 0;
    /* EOS M 2.0.2: existing port's cache alias, sensor RAW write channel,
     * idle GUI state and combined Q/SET event. All selected after the guard. */
    *profile=(struct lc_camera_profile){0x40000000u,0x12u,0,0x1d,4,5};
    return 1;
}
#endif
