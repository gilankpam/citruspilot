/* Dump each DRM plane's IN_FORMATS blob: every (format, modifier) pair the plane
 * can scan out. Answers whether the video overlay plane accepts the format the
 * Cedrus/ffmpeg path actually exports (e.g. 'YU08' with AFBC modifier
 * 0x0800000000000061) or only plain NV12.
 *
 * Build: cc plane-formats.c -o /tmp/plane-formats $(pkg-config --cflags --libs libdrm)
 *   (cross): aarch64-...-gcc --sysroot=$S -I$S/usr/include -I$S/usr/include/libdrm \
 *            plane-formats.c -o plane-formats -L$S/usr/lib -ldrm
 * Run:   /tmp/plane-formats                                                       */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <drm/drm_mode.h>

#ifndef DRM_FORMAT_MOD_VENDOR_ARM
#define DRM_FORMAT_MOD_VENDOR_ARM 0x08
#endif

struct fmt_mod_blob {
    uint32_t version, flags;
    uint32_t count_formats, formats_offset;
    uint32_t count_modifiers, modifiers_offset;
};
struct fmt_mod {
    uint64_t formats;   /* bitmask: bit j selects formats[offset + j] */
    uint32_t offset, pad;
    uint64_t modifier;
};

static uint64_t plane_prop_blob(int fd, uint32_t plane_id, const char *name) {
    drmModeObjectProperties *p =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    uint64_t blob = 0;
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr && !strcmp(pr->name, name)) blob = p->prop_values[i];
        if (pr) drmModeFreeProperty(pr);
    }
    if (p) drmModeFreeObjectProperties(p);
    return blob;
}

static const char *type_name(int fd, uint32_t plane_id) {
    uint64_t t = 0;
    drmModeObjectProperties *p =
        drmModeObjectGetProperties(fd, plane_id, DRM_MODE_OBJECT_PLANE);
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr && !strcmp(pr->name, "type")) t = p->prop_values[i];
        if (pr) drmModeFreeProperty(pr);
    }
    if (p) drmModeFreeObjectProperties(p);
    return t == DRM_PLANE_TYPE_PRIMARY ? "PRIMARY"
         : t == DRM_PLANE_TYPE_CURSOR  ? "CURSOR" : "OVERLAY";
}

int main(void) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card0"); return 1; }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[i]);
        printf("== plane %u  type=%s  possible_crtcs=0x%x ==\n",
               pl->plane_id, type_name(fd, pl->plane_id), pl->possible_crtcs);

        uint64_t blob_id = plane_prop_blob(fd, pl->plane_id, "IN_FORMATS");
        if (!blob_id) {
            printf("   no IN_FORMATS; legacy formats:");
            for (uint32_t f = 0; f < pl->count_formats; f++)
                printf(" %.4s", (char *)&pl->formats[f]);
            printf("\n");
            drmModeFreePlane(pl); continue;
        }
        drmModePropertyBlobRes *b = drmModeGetPropertyBlob(fd, blob_id);
        if (!b) { printf("   IN_FORMATS blob unreadable\n"); drmModeFreePlane(pl); continue; }

        const uint8_t *base = b->data;
        const struct fmt_mod_blob *h = (const struct fmt_mod_blob *)base;
        const uint32_t *fmts = (const uint32_t *)(base + h->formats_offset);
        const struct fmt_mod *mods = (const struct fmt_mod *)(base + h->modifiers_offset);

        for (uint32_t m = 0; m < h->count_modifiers; m++) {
            for (uint32_t j = 0; j < 64; j++) {
                if (!(mods[m].formats & (1ULL << j))) continue;
                uint32_t fi = mods[m].offset + j;
                if (fi >= h->count_formats) continue;
                uint32_t fc = fmts[fi];
                uint64_t mod = mods[m].modifier;
                int arm = (mod >> 56) == DRM_FORMAT_MOD_VENDOR_ARM;
                printf("   fmt=%.4s  modifier=%#018llx%s\n",
                       (char *)&fc, (unsigned long long)mod,
                       mod == DRM_FORMAT_MOD_LINEAR ? "  (LINEAR)" :
                       arm ? "  (ARM/AFBC)" : "");
            }
        }
        drmModeFreePropertyBlob(b);
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pr);
    close(fd);
    return 0;
}
