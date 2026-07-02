/* Does the DE33 VI plane (NV12 video overlay) accept a scaling commit?
 * Runs DRM_MODE_ATOMIC_TEST_ONLY commits with SRC != CRTC rects and reports
 * the kernel's verdict (errno) per case — without disturbing the live screen.
 *
 * Build: cc scale-probe.c -o /tmp/scale-probe $(pkg-config --cflags --libs libdrm)
 * Run:   /tmp/scale-probe                                                        */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

static uint32_t prop_id(int fd, uint32_t obj, uint32_t type, const char *name) {
    drmModeObjectProperties *p = drmModeObjectGetProperties(fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; p && i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, p->props[i]);
        if (pr) { if (!strcmp(pr->name, name)) id = pr->prop_id; drmModeFreeProperty(pr); }
    }
    if (p) drmModeFreeObjectProperties(p);
    return id;
}

/* Find first OVERLAY plane that advertises NV12 (the VI/video plane). */
static uint32_t find_nv12_plane(int fd, uint32_t *out_crtc_id, int *out_crtc_idx) {
    drmModePlaneRes *pr = drmModeGetPlaneResources(fd);
    uint32_t found = 0;
    for (uint32_t i = 0; pr && i < pr->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(fd, pr->planes[i]);
        int has_nv12 = 0;
        for (uint32_t f = 0; f < pl->count_formats; f++)
            if (pl->formats[f] == DRM_FORMAT_NV12) has_nv12 = 1;
        /* type == OVERLAY check */
        drmModeObjectProperties *op = drmModeObjectGetProperties(fd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
        int is_overlay = 0;
        for (uint32_t k = 0; op && k < op->count_props; k++) {
            drmModePropertyRes *p = drmModeGetProperty(fd, op->props[k]);
            if (p && !strcmp(p->name, "type") && op->prop_values[k] == DRM_PLANE_TYPE_OVERLAY) is_overlay = 1;
            if (p) drmModeFreeProperty(p);
        }
        if (op) drmModeFreeObjectProperties(op);
        if (has_nv12 && is_overlay && !found) {
            found = pl->plane_id;
            /* pick lowest set bit of possible_crtcs */
            for (int b = 0; b < 32; b++) if (pl->possible_crtcs & (1u << b)) { *out_crtc_idx = b; break; }
        }
        drmModeFreePlane(pl);
    }
    if (found) {
        drmModeRes *res = drmModeGetResources(fd);
        if (res && *out_crtc_idx < res->count_crtcs) *out_crtc_id = res->crtcs[*out_crtc_idx];
        if (res) drmModeFreeResources(res);
    }
    drmModeFreePlaneResources(pr);
    return found;
}

static uint32_t make_nv12_fb(int fd, int w, int h) {
    struct drm_mode_create_dumb cd = { .width = w, .height = h * 3 / 2, .bpp = 8 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 0; }
    uint32_t handles[4] = { cd.handle, cd.handle, 0, 0 };
    uint32_t pitches[4] = { cd.pitch, cd.pitch, 0, 0 };
    uint32_t offsets[4] = { 0, cd.pitch * h, 0, 0 };
    uint32_t fb = 0;
    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_NV12, handles, pitches, offsets, &fb, 0))
        { perror("addfb2 nv12"); return 0; }
    return fb;
}

/* Like make_nv12_fb but also maps the buffer and paints a test pattern:
 * luma = horizontal gradient with a bright grid every 64 px, chroma = 8
 * vertical colour bars. Any scaling artefact (wrong pitch, chroma shift,
 * non-scaled crop) is obvious against this pattern. */
static uint32_t make_pattern_fb(int fd, int w, int h) {
    struct drm_mode_create_dumb cd = { .width = w, .height = h * 3 / 2, .bpp = 8 };
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { perror("create_dumb"); return 0; }
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md)) { perror("map_dumb"); return 0; }
    uint8_t *p = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
    if (p == MAP_FAILED) { perror("mmap"); return 0; }
    static const uint8_t ub[8] = {128,  84, 255,   0, 170,  42, 212, 128};
    static const uint8_t vb[8] = {128, 255, 107, 148,   0, 201,  50,  21};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            p[y * cd.pitch + x] = (x % 64 < 2 || y % 64 < 2) ? 235 : 16 + (x * 219) / w;
    uint8_t *uv = p + cd.pitch * h;
    for (int y = 0; y < h / 2; y++)
        for (int x = 0; x < w / 2; x++) {
            int bar = (x * 2 * 8) / w;
            uv[y * cd.pitch + 2 * x]     = ub[bar];
            uv[y * cd.pitch + 2 * x + 1] = vb[bar];
        }
    munmap(p, cd.size);
    uint32_t handles[4] = { cd.handle, cd.handle, 0, 0 };
    uint32_t pitches[4] = { cd.pitch, cd.pitch, 0, 0 };
    uint32_t offsets[4] = { 0, cd.pitch * h, 0, 0 };
    uint32_t fb = 0;
    if (drmModeAddFB2(fd, w, h, DRM_FORMAT_NV12, handles, pitches, offsets, &fb, 0))
        { perror("addfb2 pattern"); return 0; }
    return fb;
}

static int test_scale(int fd, uint32_t plane, uint32_t crtc, uint32_t fb,
                      uint32_t P_FB, uint32_t P_CRTC,
                      uint32_t P_CX, uint32_t P_CY, uint32_t P_CW, uint32_t P_CH,
                      uint32_t P_SX, uint32_t P_SY, uint32_t P_SW, uint32_t P_SH,
                      int sw, int sh, int cw, int ch, const char *label) {
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, plane, P_FB, fb);
    drmModeAtomicAddProperty(r, plane, P_CRTC, crtc);
    drmModeAtomicAddProperty(r, plane, P_CX, 0);
    drmModeAtomicAddProperty(r, plane, P_CY, 0);
    drmModeAtomicAddProperty(r, plane, P_CW, cw);
    drmModeAtomicAddProperty(r, plane, P_CH, ch);
    drmModeAtomicAddProperty(r, plane, P_SX, 0);
    drmModeAtomicAddProperty(r, plane, P_SY, 0);
    drmModeAtomicAddProperty(r, plane, P_SW, (uint64_t)sw << 16);
    drmModeAtomicAddProperty(r, plane, P_SH, (uint64_t)sh << 16);
    int rc = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
    int e = errno;
    printf("  %-28s SRC %dx%d -> CRTC %dx%d : %s%s\n",
           label, sw, sh, cw, ch,
           rc == 0 ? "ACCEPTED" : "REJECTED",
           rc == 0 ? "" : (e == ERANGE ? " (ERANGE)" : strerror(e)));
    drmModeAtomicFree(r);
    return rc;
}

int main(int argc, char **argv) {
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("open card0"); return 1; }
    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    uint32_t crtc_id = 0; int crtc_idx = 0;
    uint32_t plane = find_nv12_plane(fd, &crtc_id, &crtc_idx);
    if (!plane || !crtc_id) { fprintf(stderr, "no NV12 overlay plane / crtc\n"); return 1; }

    drmModeCrtc *c = drmModeGetCrtc(fd, crtc_id);
    int crtc_active = c && c->mode_valid;
    int mode_w = c && c->mode_valid ? c->mode.hdisplay : 1920;
    int mode_h = c && c->mode_valid ? c->mode.vdisplay : 1080;
    printf("VI plane=%u  crtc=%u (idx %d)  active mode=%dx%d\n",
           plane, crtc_id, crtc_idx, mode_w, mode_h);
    if (c) drmModeFreeCrtc(c);

    uint32_t P_FB  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    uint32_t P_CRTC= prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    uint32_t P_CX  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    uint32_t P_CY  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    uint32_t P_CW  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    uint32_t P_CH  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    uint32_t P_SX  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    uint32_t P_SY  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    uint32_t P_SW  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    uint32_t P_SH  = prop_id(fd, plane, DRM_MODE_OBJECT_PLANE, "SRC_H");

    /* Source frame: a typical 1080p stream. */
    int sw = 1920, sh = 1080;
    uint32_t fb = make_nv12_fb(fd, sw, sh);
    if (!fb) return 1;

    printf("TEST_ONLY commits (no visible change):\n");
    /* control: 1:1 within mode */
    int c11 = (sw <= mode_w && sh <= mode_h)
        ? test_scale(fd, plane, crtc_id, fb, P_FB,P_CRTC,P_CX,P_CY,P_CW,P_CH,P_SX,P_SY,P_SW,P_SH,
                     sw, sh, sw, sh, "1:1 (control)")
        : -2;
    /* upscale 1080p -> full mode (e.g. 1440p) */
    test_scale(fd, plane, crtc_id, fb, P_FB,P_CRTC,P_CX,P_CY,P_CW,P_CH,P_SX,P_SY,P_SW,P_SH,
               sw, sh, mode_w, mode_h, "upscale -> fullscreen");
    /* small downscale */
    test_scale(fd, plane, crtc_id, fb, P_FB,P_CRTC,P_CX,P_CY,P_CW,P_CH,P_SX,P_SY,P_SW,P_SH,
               sw, sh, mode_w/2, mode_h/2, "downscale -> half");

    if (c11 == -2) printf("  (skipped 1:1 control — source larger than mode)\n");

    /* --commit [seconds]: REAL scaled commit of a test pattern, fullscreen.
     * Needs DRM master (stop citrusplay first) and an active CRTC. */
    int commit_secs = 0;
    if (argc > 1 && !strcmp(argv[1], "--commit"))
        commit_secs = argc > 2 ? atoi(argv[2]) : 5;

    if (commit_secs > 0) {
        if (!crtc_active) { fprintf(stderr, "CRTC inactive - start+stop citrusplay first to bring the screen up\n"); return 1; }
        uint32_t pfb = make_pattern_fb(fd, sw, sh);
        if (!pfb) return 1;
        drmModeAtomicReq *r = drmModeAtomicAlloc();
        drmModeAtomicAddProperty(r, plane, P_FB, pfb);
        drmModeAtomicAddProperty(r, plane, P_CRTC, crtc_id);
        drmModeAtomicAddProperty(r, plane, P_CX, 0);
        drmModeAtomicAddProperty(r, plane, P_CY, 0);
        drmModeAtomicAddProperty(r, plane, P_CW, mode_w);
        drmModeAtomicAddProperty(r, plane, P_CH, mode_h);
        drmModeAtomicAddProperty(r, plane, P_SX, 0);
        drmModeAtomicAddProperty(r, plane, P_SY, 0);
        drmModeAtomicAddProperty(r, plane, P_SW, (uint64_t)sw << 16);
        drmModeAtomicAddProperty(r, plane, P_SH, (uint64_t)sh << 16);
        int rc = drmModeAtomicCommit(fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        printf("REAL commit SRC %dx%d -> CRTC %dx%d : %s\n", sw, sh, mode_w, mode_h,
               rc == 0 ? "OK - pattern should now FILL the screen" : strerror(errno));
        drmModeAtomicFree(r);
        if (rc == 0) sleep(commit_secs);
        return rc ? 1 : 0;
    }

    close(fd);
    return 0;
}
