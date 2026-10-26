#ifndef XPAN_HUD_CONTROLS_H
#define XPAN_HUD_CONTROLS_H
/* XPan Live View HUD: one selected setting, changed with the rear dial.
 * INFO moves the selection left; tapping a field selects it. Only while the XPan
 * photo HUD owns Live View (no menu, Canon dialog, playback or recording). */
enum {
    XPAN_HUD_ISO, XPAN_HUD_APERTURE, XPAN_HUD_SHUTTER, XPAN_HUD_WB, XPAN_HUD_FOCUS,
    XPAN_HUD_DRIVE, XPAN_HUD_METER, XPAN_HUD_FORMAT, XPAN_HUD_EV, XPAN_HUD_LUT,
    XPAN_HUD_MODE,
    XPAN_HUD_FIELDS
};
struct event;
int xpan_hud_controls_key(struct event *event); /* 0 = consumed */
int xpan_hud_controls_field(void);
int xpan_hud_controls_field_enabled(int field); /* 0 = no effect now: grey, skipped */
void xpan_hud_controls_step(void);  /* from shoot_task */
void xpan_hud_controls_debug(char *buf, int size); /* temporary test line */
void xpan_hud_controls_events(char *buf, int size); /* temporary: last key codes */
#endif
