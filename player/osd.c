#define _GNU_SOURCE
#include "osd.h"
#include "osd_render.h"
#include "stats.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#define OSD_COLS  18
#define OSD_LINES 3
#define OSD_PAD   4

struct osd {
    int drm_fd;
    uint32_t plane_id, crtc_id, fb_id, handle;
    osd_plane_props props;
    uint32_t *map;
    size_t map_size;
    int box_w, box_h, x, y, scale, stride_px;
    uint64_t zpos;
    stats_t st;
    int64_t last_ms;
    int rendered_once;
};

static int64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void render(osd_t *o)
{
    stats_sample_t s;
    stats_sample(&o->st, &s);

    osd_fill(o->map, o->box_w, o->box_h, o->stride_px, OSD_ARGB(0x80, 0, 0, 0));

    char l[OSD_LINES][32];
    if (s.cpu_pct >= 0) snprintf(l[0], sizeof l[0], "CPU  %d%%", s.cpu_pct);
    else                snprintf(l[0], sizeof l[0], "CPU  --");
    if (s.mem_used_mb >= 0) snprintf(l[1], sizeof l[1], "MEM  %d/%d MB", s.mem_used_mb, s.mem_total_mb);
    else                    snprintf(l[1], sizeof l[1], "MEM  --");
    if (s.temp_c >= 0) snprintf(l[2], sizeof l[2], "TEMP %d C", s.temp_c);
    else               snprintf(l[2], sizeof l[2], "TEMP --");

    uint32_t fg = OSD_ARGB(0xff, 0xff, 0xff, 0xff);
    for (int i = 0; i < OSD_LINES; i++)
        osd_draw_text(o->map, o->box_w, o->box_h, o->stride_px,
                      OSD_PAD, OSD_PAD + i * 16 * o->scale, l[i], fg, o->scale);
}

osd_t *osd_create(int drm_fd, uint32_t crtc_id, uint32_t plane_id,
                  const osd_plane_props *props,
                  int screen_w, int screen_h, int margin, int scale,
                  uint64_t zpos_val)
{
    osd_t *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->drm_fd = drm_fd; o->crtc_id = crtc_id; o->plane_id = plane_id;
    o->props = *props; o->scale = scale; o->zpos = zpos_val;

    osd_box_size(OSD_COLS, OSD_LINES, scale, OSD_PAD, &o->box_w, &o->box_h);
    o->x = margin; o->y = margin;
    if (o->x + o->box_w > screen_w) o->x = screen_w - o->box_w;
    if (o->y + o->box_h > screen_h) o->y = screen_h - o->box_h;
    if (o->x < 0) o->x = 0;
    if (o->y < 0) o->y = 0;

    struct drm_mode_create_dumb cd = { .width = o->box_w, .height = o->box_h, .bpp = 32 };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { free(o); return NULL; }
    o->handle = cd.handle; o->stride_px = cd.pitch / 4; o->map_size = cd.size;

    uint32_t handles[4] = { cd.handle }, pitches[4] = { cd.pitch }, offsets[4] = { 0 };
    if (drmModeAddFB2(drm_fd, o->box_w, o->box_h, DRM_FORMAT_ARGB8888,
                      handles, pitches, offsets, &o->fb_id, 0))
        goto fail_dumb;

    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
        goto fail_fb;
    o->map = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, md.offset);
    if (o->map == MAP_FAILED)
        goto fail_fb;

    stats_init(&o->st);
    render(o);
    o->last_ms = mono_ms();
    o->rendered_once = 1;
    return o;

fail_fb:
    drmModeRmFB(drm_fd, o->fb_id);
fail_dumb:
    {
        struct drm_mode_destroy_dumb dd = { .handle = cd.handle };
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    }
    free(o);
    return NULL;
}

int osd_tick(osd_t *o)
{
    if (!o) return 0;
    int64_t now = mono_ms();
    if (o->rendered_once && now - o->last_ms < 1000) return 0;
    render(o);
    o->last_ms = now;
    return 1;
}

void osd_add_to_commit(osd_t *o, drmModeAtomicReq *r, int initial)
{
    if (!o) return;
    drmModeAtomicAddProperty(r, o->plane_id, o->props.fb_id, o->fb_id);
    if (initial) {
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_id, o->crtc_id);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_x, o->x);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_y, o->y);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_w, o->box_w);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.crtc_h, o->box_h);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_x, 0);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_y, 0);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_w, (uint64_t)o->box_w << 16);
        drmModeAtomicAddProperty(r, o->plane_id, o->props.src_h, (uint64_t)o->box_h << 16);
        if (o->props.zpos) drmModeAtomicAddProperty(r, o->plane_id, o->props.zpos, o->zpos);
    }
}

void osd_destroy(osd_t *o)
{
    if (!o) return;
    if (o->map && o->map != MAP_FAILED) munmap(o->map, o->map_size);
    if (o->fb_id) drmModeRmFB(o->drm_fd, o->fb_id);
    struct drm_mode_destroy_dumb dd = { .handle = o->handle };
    if (o->handle) drmIoctl(o->drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
    free(o);
}
