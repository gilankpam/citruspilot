#ifndef CITRUSPILOT_OSD_RENDER_H
#define CITRUSPILOT_OSD_RENDER_H
#include <stdint.h>
#include <stddef.h>

/* ARGB8888 little-endian: 0xAARRGGBB packed in a uint32_t. */
#define OSD_ARGB(a, r, g, b) \
    (((uint32_t)(a) << 24) | ((uint32_t)(r) << 16) | \
     ((uint32_t)(g) << 8)  | (uint32_t)(b))

/* All functions take a pixel buffer with `stride_px` uint32_t per row. */
void osd_fill(uint32_t *buf, int w, int h, int stride_px, uint32_t argb);

/* Blit one 8x16 glyph at (x,y), integer-scaled, in `color`. Font-bit-0 pixels
 * are left untouched (transparent). Clipped to the buffer. */
void osd_draw_char(uint32_t *buf, int w, int h, int stride_px,
                   int x, int y, char c, uint32_t color, int scale);

/* Draw a NUL-terminated string; advances 8*scale px per char. Returns the x
 * just past the last glyph. */
int osd_draw_text(uint32_t *buf, int w, int h, int stride_px,
                  int x, int y, const char *s, uint32_t color, int scale);

/* Pixel size for `nlines` rows of `cols` chars at `scale`, with `pad` px on
 * every side. */
void osd_box_size(int cols, int nlines, int scale, int pad, int *w, int *h);

/* Resolution-proportional glyph scale for a screen `screen_h` px tall: rounds
 * screen_h/540 so the HUD stays a roughly constant fraction of the display
 * (=2 at 1080p, =1 at 720p, =4 at 4K). Never returns below 1. */
int osd_scale_for_height(int screen_h);

/* Top-left origin (*x,*y) that places a box_w x box_h HUD in the TOP-RIGHT
 * corner of a screen_w x screen_h screen, `margin` px in from the top and right
 * edges, clamped so the box stays on-screen. */
void osd_origin(int screen_w, int screen_h, int box_w, int box_h, int margin,
                int *x, int *y);
#endif
