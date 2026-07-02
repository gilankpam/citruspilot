/* fit_rect — aspect-preserving fit of a video into a display mode.
 * Pure/header-only so the host unit tests build it without DRM/libav. */
#ifndef CITRUSPILOT_FIT_RECT_H
#define CITRUSPILOT_FIT_RECT_H

typedef struct { int x, y, w, h; } fit_rect_t;

/* Largest src_w:src_h rectangle that fits mode_w x mode_h, centred
 * (letterbox/pillarbox). x/y/w/h are rounded DOWN to even — NV12 chroma is
 * 2x2-subsampled and even coords keep the DE33 blender happy. Non-positive
 * input yields the zero rect (caller: treat as "cannot scale"). */
static inline fit_rect_t fit_rect(int src_w, int src_h, int mode_w, int mode_h)
{
    fit_rect_t r = {0, 0, 0, 0};
    if (src_w <= 0 || src_h <= 0 || mode_w <= 0 || mode_h <= 0)
        return r;
    long long sw_mh = (long long)src_w * mode_h;
    long long sh_mw = (long long)src_h * mode_w;
    if (sw_mh >= sh_mw) {              /* source is wider: full width, letterbox */
        r.w = mode_w;
        r.h = (int)(sh_mw / src_w);
    } else {                           /* source is taller: full height, pillarbox */
        r.h = mode_h;
        r.w = (int)(sw_mh / src_h);
    }
    r.w &= ~1;
    r.h &= ~1;
    if (r.w <= 0 || r.h <= 0)
        return (fit_rect_t){0, 0, 0, 0};
    r.x = ((mode_w - r.w) / 2) & ~1;
    r.y = ((mode_h - r.h) / 2) & ~1;
    return r;
}

#endif
