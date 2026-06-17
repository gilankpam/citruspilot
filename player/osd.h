#ifndef CITRUSPILOT_OSD_H
#define CITRUSPILOT_OSD_H
#include <stdint.h>
#include <xf86drmMode.h>

/* Plane property ids the OSD needs to attach itself to an atomic commit. */
typedef struct {
    uint32_t fb_id, crtc_id, crtc_x, crtc_y, crtc_w, crtc_h,
             src_x, src_y, src_w, src_h, zpos;
} osd_plane_props;

typedef struct osd osd_t;

/* Create an OSD on `plane_id`: allocate an ARGB8888 dumb buffer sized to the HUD
 * box, place it `margin` px in from the top-left of the screen, assign `zpos_val`
 * (above the video), render the first frame. Returns NULL on any failure — the
 * caller then simply runs without an OSD. */
osd_t *osd_create(int drm_fd, uint32_t crtc_id, uint32_t plane_id,
                  const osd_plane_props *props,
                  int screen_w, int screen_h, int margin, int scale,
                  uint64_t zpos_val);

/* Re-sample stats and re-render the buffer in place if >= 1 s since last render.
 * Returns 1 if it re-rendered, else 0. No atomic commit needed (in-place buffer). */
int osd_tick(osd_t *o);

/* Add the OSD plane's props to an atomic request. With `initial` != 0 it adds
 * CRTC_ID/position/zpos (for the startup modeset); otherwise only FB_ID. */
void osd_add_to_commit(osd_t *o, drmModeAtomicReq *r, int initial);

void osd_destroy(osd_t *o);
#endif
