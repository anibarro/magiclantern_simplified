#ifndef XPAN_CUBE_H
#define XPAN_CUBE_H
#include <stdint.h>
#include <stddef.h>

#define XPAN_CUBE_MAX_SIZE 33
#define XPAN_CUBE_MAX_COMPONENTS (33u*33u*33u*3u)
/* Streaming text parser: fixed line storage, caller-owned RGB16 array, and no
 * allocation or floating point. Only normalized, 3D-only Cube files are enabled. */
struct xpan_cube_parser {
    uint16_t *rgb;
    unsigned capacity,size,components,line_number,line_used,error,headers;
    unsigned identity,discard_comment;
    char line[192];
};
void xpan_cube_init(struct xpan_cube_parser *p, uint16_t *rgb, unsigned capacity);
int xpan_cube_feed(struct xpan_cube_parser *p, const char *data, size_t bytes);
int xpan_cube_finish(struct xpan_cube_parser *p);
const char *xpan_cube_error(const struct xpan_cube_parser *p);
#endif
