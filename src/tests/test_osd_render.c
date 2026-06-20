#include "../osd_render.h"
#include "../font8x16.h"
#include <assert.h>
#include <stdlib.h>

int main(void) {
    int w, h;
    osd_box_size(12, 3, 2, 4, &w, &h);    /* cols, lines, scale, pad */
    assert(w == 12 * 8 * 2 + 2 * 4);
    assert(h == 3 * 16 * 2 + 2 * 4);

    /* resolution-proportional OSD glyph scale: ~2 at 1080p, rounds, floors at 1 */
    assert(osd_scale_for_height(1080) == 2);   /* 1080/540 = 2.0  */
    assert(osd_scale_for_height(720)  == 1);   /* 720/540 = 1.33 -> 1 */
    assert(osd_scale_for_height(540)  == 1);   /* 540/540 = 1.0  */
    assert(osd_scale_for_height(1440) == 3);   /* 1440/540 = 2.67 -> 3 */
    assert(osd_scale_for_height(2160) == 4);   /* 2160/540 = 4.0 (4K) */
    assert(osd_scale_for_height(0)    == 1);   /* never below 1 */

    /* HUD origin sits in the TOP-RIGHT corner, `margin` px in from the top and
     * right edges, clamped on-screen. */
    int ox, oy;
    osd_origin(1280, 720, 152, 56, 16, &ox, &oy);
    assert(ox == 1280 - 152 - 16);   /* 1112: margin in from the right edge */
    assert(oy == 16);                /* margin down from the top edge */
    /* box wider than the screen -> clamp x to 0, keep top margin */
    osd_origin(100, 100, 152, 56, 16, &ox, &oy);
    assert(ox == 0);
    assert(oy == 16);

    uint32_t bg = OSD_ARGB(0x80, 0, 0, 0);
    uint32_t fg = OSD_ARGB(0xff, 0xff, 0xff, 0xff);
    uint32_t *buf = calloc((size_t)w * h, sizeof *buf);
    assert(buf);

    osd_fill(buf, w, h, w, bg);
    assert(buf[0] == bg);
    assert(buf[(size_t)w * h - 1] == bg);

    /* draw_text advances 8*scale px per glyph and returns the new x */
    int x2 = osd_draw_text(buf, w, h, w, 4, 4, "AB", fg, 2);
    assert(x2 == 4 + 2 * (8 * 2));

    /* the font actually has ink for a printable glyph */
    int ink = 0;
    for (int i = 0; i < 16; i++) ink += font8x16['A' - 0x20][i];
    assert(ink > 0);

    /* drawing past the right edge must not crash (clipping) */
    osd_draw_char(buf, w, h, w, w - 2, h - 2, 'X', fg, 3);

    free(buf);
    return 0;
}
