#ifndef XPAN_LIVE_COLOR_PROBE_H
#define XPAN_LIVE_COLOR_PROBE_H
struct card_info;
/* Observation-only candidate: called by the existing idle XPan worker. */
void xpan_live_color_probe_poll(struct card_info *card);
/* Queue another existing bounded snapshot; no file work or allocation. */
void xpan_live_color_probe_queue_unknown(void);
#endif
