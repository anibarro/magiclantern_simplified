#ifndef XPAN_FILM_GRAIN_H
#define XPAN_FILM_GRAIN_H
struct card_info;
/* Saved-JPEG native grain; the existing XPan worker prepares small assets. */
void xpan_film_grain_poll(struct card_info *card);
void xpan_film_grain_add_menu(void);
#endif
