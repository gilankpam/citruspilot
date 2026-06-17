/* Enumerate DRM planes on the active CRTC: type, formats, zpos range. Tells us
 * whether a non-primary plane supports an alpha format (ARGB8888/ABGR8888) and
 * can sit above the video — the prerequisite for the OSD overlay.
 *
 * Build: cc plane-probe.c -o /tmp/plane-probe $(pkg-config --cflags --libs libdrm)
 * Run:   /tmp/plane-probe                                                       */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static const char *type_name(int fd, uint32_t plane_id) {
    drmModeObjectProperties *p =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    const char *r = "?";
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr && !strcmp(pr->name, "type")) {
            switch (p->prop_values[i]) {
                case DRM_PLANE_TYPE_PRIMARY: r = "PRIMARY"; break;
                case DRM_PLANE_TYPE_OVERLAY: r = "OVERLAY"; break;
                case DRM_PLANE_TYPE_CURSOR:  r = "CURSOR";  break;
            }
        }
        if (pr) drmModeFreeProperty(pr);
    }
    if (p) drmModeFreeObjectProperties(p);
    return r;
}

static void print_zpos(int fd, uint32_t plane_id) {
    drmModeObjectProperties *p =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr && !strcmp(pr->name, "zpos")) {
            printf("    zpos=%llu", (unsigned long long)p->prop_values[i]);
            if (pr->count_values == 2)
                printf(" range=[%lld..%lld]%s",
                       (long long)pr->values[0], (long long)pr->values[1],
                       (pr->flags & DRM_MODE_PROP_IMMUTABLE) ? " IMMUTABLE" : " mutable");
            printf("\n");
        }
        if (pr) drmModeFreeProperty(pr);
    }
    if (p) drmModeFreeObjectProperties(p);
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card0"); return 1; }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[i]);
        printf("plane %u  type=%s  possible_crtcs=0x%x\n",
               pl->plane_id, type_name(fd, pl->plane_id), pl->possible_crtcs);
        printf("    formats:");
        int has_argb = 0, has_abgr = 0, has_nv12 = 0;
        for (uint32_t f = 0; f < pl->count_formats; f++) {
            uint32_t fmt = pl->formats[f];
            printf(" %.4s", (char *)&fmt);
            if (fmt == DRM_FORMAT_ARGB8888) has_argb = 1;
            if (fmt == DRM_FORMAT_ABGR8888) has_abgr = 1;
            if (fmt == DRM_FORMAT_NV12)     has_nv12 = 1;
        }
        printf("\n    alpha=%s%s  nv12=%s\n",
               has_argb ? "ARGB8888 " : "", has_abgr ? "ABGR8888" : (has_argb ? "" : "none"),
               has_nv12 ? "yes" : "no");
        print_zpos(fd, pl->plane_id);
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    close(fd);
    return 0;
}
