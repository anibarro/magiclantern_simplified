#ifndef LIVECOMP_LV_H
#define LIVECOMP_LV_H
struct card_info;
void livecomp_lv_poll(struct card_info *card);
int livecomp_lv_busy(void);
int livecomp_lv_running(void);
int livecomp_lv_key(unsigned code);
unsigned livecomp_lv_vsync(unsigned context);
void livecomp_lv_shutdown(void);
#endif
