/* GPL-2.0-or-later. Explicitly selected development check, not sensor capture. */
#ifndef LIVECOMP_FULLRES_CHECK_H
#define LIVECOMP_FULLRES_CHECK_H
struct card_info;
/* Returns nonzero if memory/hook ownership is uncertain; caller blocks retries. */
unsigned livecomp_fullres_check(struct card_info *card,
    const volatile unsigned *cancel, const char **status);
void livecomp_fullres_vsync(void);
#endif
