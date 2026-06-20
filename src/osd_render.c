#include "osd_render.h"
#include "font8x16.h"

void osd_fill(uint32_t *buf, int w, int h, int stride_px, uint32_t argb)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            buf[(size_t)y * stride_px + x] = argb;
}

void osd_draw_char(uint32_t *buf, int w, int h, int stride_px,
                   int x, int y, char c, uint32_t color, int scale)
{
    unsigned uc = (unsigned char)c;
    if (uc < 0x20 || uc > 0x7e) return;          /* blank for non-printable */
    const uint8_t *g = font8x16[uc - 0x20];
    for (int row = 0; row < 16; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < 8; col++) {
            if (!(bits & (0x80 >> col))) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    buf[(size_t)py * stride_px + px] = color;
                }
        }
    }
}

int osd_draw_text(uint32_t *buf, int w, int h, int stride_px,
                  int x, int y, const char *s, uint32_t color, int scale)
{
    for (; *s; s++) {
        osd_draw_char(buf, w, h, stride_px, x, y, *s, color, scale);
        x += 8 * scale;
    }
    return x;
}

void osd_box_size(int cols, int nlines, int scale, int pad, int *w, int *h)
{
    *w = cols * 8 * scale + 2 * pad;
    *h = nlines * 16 * scale + 2 * pad;
}

int osd_scale_for_height(int screen_h)
{
    int s = (screen_h + 270) / 540;   /* round(screen_h / 540), integer math */
    return s < 1 ? 1 : s;
}

void osd_origin(int screen_w, int screen_h, int box_w, int box_h, int margin,
                int *x, int *y)
{
    *x = screen_w - box_w - margin;   /* margin in from the right edge */
    *y = margin;                      /* margin down from the top edge */
    if (*x + box_w > screen_w) *x = screen_w - box_w;
    if (*y + box_h > screen_h) *y = screen_h - box_h;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}
