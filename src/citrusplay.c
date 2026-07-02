// citrusplay — direct-to-plane HW video player for a LIVE RTP stream (wfb-ng FPV) on
// the Allwinner H618. Derived from drmvid (h618-mainline-video): the same
// Cedrus (v4l2request) -> DRM-PRIME NV12 -> hardware overlay-plane scanout, zero
// copy, no GPU compositing. The difference is the front-end: instead of a file
// it ingests the de-FEC'd RTP that wfb_rx forwards to a local UDP port.
//
// Live-specific changes vs drmvid (see docs/findings/stage0-player-pipe.md):
//   * libav RTP input via a BUILT-IN H.265 SDP (no external .sdp file) on a
//     configurable UDP port, with a large socket buffer (bursty RTP) + low-delay
//   * NO presentation pacing — each frame is scanned out the instant it decodes
//   * RUN FOREVER — the screen is brought up at startup (preferred mode, black
//     primary); the video plane appears on the first decoded frame, HW-scaled
//     to aspect-fit the mode (kernel scaler_mask patch; falls back to retuning
//     the HDMI mode to the stream size on unpatched 1:1-only kernels, or set
//     CITRUSPLAY_RETUNE=1 to force that). On a stream drop the frame stays FROZEN
//     and the player keeps waiting / reconnects, resuming on the next IDR. Only
//     SIGINT/SIGTERM exits.
//   * tolerant of decode errors (packet loss -> corrupt frames until the next
//     IDR) — it skips bad frames, never crashes
//
// Requires kernel patch 0099 (NV12 on the DE33 VI plane) + linear-NV12 Cedrus;
// full-screen scaling additionally needs the DE33 scaler_mask kernel patch.
//
// Build: make            (cross: make SYSROOT=/path/to/staging CC=aarch64-...-gcc)
// Use:   citrusplay [--port N]     (built-in H.265 SDP; default udp:5600)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <sys/mman.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>

/* The V4L2 Request API hwaccel is NOT in upstream/Debian ffmpeg — the target
 * runs a patched libav (sbc-groundstations ffmpeg-v4l2request) that appends this
 * type right after AV_HWDEVICE_TYPE_D3D12VA. Allow building against stock headers
 * (e.g. CI) by defining it to the same value the patch uses (last type + 1); the
 * runtime libavutil on the board is the one that actually implements it. */
#ifndef AV_HWDEVICE_TYPE_V4L2REQUEST
#define AV_HWDEVICE_TYPE_V4L2REQUEST ((enum AVHWDeviceType)(AV_HWDEVICE_TYPE_D3D12VA + 1))
#endif

#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "rtp_h265.h"
#include "osd.h"
#include "osd_render.h"
#include "fit_rect.h"

#define DIE(...) do { fprintf(stderr, "citrusplay: " __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
#define LOG(...) do { fprintf(stderr, "citrusplay: " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

static int drm_fd;
static uint32_t conn_id, crtc_id, vplane_id, pplane_id, mode_blob;
static drmModeModeInfo mode;
static int crtc_idx;

// property ids
static uint32_t C_ACTIVE, C_MODE, CON_CRTC;
static uint32_t Vp_FB, Vp_CRTC, Vp_CX, Vp_CY, Vp_CW, Vp_CH, Vp_SX, Vp_SY, Vp_SW, Vp_SH, Vp_ZPOS;
static uint32_t Pp_FB, Pp_CRTC, Pp_CX, Pp_CY, Pp_CW, Pp_CH, Pp_SX, Pp_SY, Pp_SW, Pp_SH, Pp_ZPOS;
static uint32_t Vp_CENC, Vp_CRANGE;
static uint64_t vplane_zmax = 1;
static int opt_enc, opt_range, opt_nv21, opt_debug;
static int opt_retune;              /* CITRUSPLAY_RETUNE=1: force HDMI mode retune (old behavior) */
static int opt_port;
static uint64_t video_zpos = 1;              /* set from vplane zmax at runtime */
static uint32_t osd_plane_id;
static osd_plane_props Op;          /* OSD plane prop ids */
static uint64_t osd_zpos_max = 1;
static osd_t *osd;
static int opt_osd, opt_osd_scale;

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

static void nsleep_ms(long ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&t, NULL);
}

/* Effective OSD glyph scale: the CITRUSPLAY_OSD_SCALE override when set
 * (opt_osd_scale > 0), else derived from the current mode height so the HUD
 * stays a roughly constant fraction of the screen as the resolution changes. */
static int osd_scale_now(void)
{
    return opt_osd_scale > 0 ? opt_osd_scale : osd_scale_for_height(mode.vdisplay);
}

static volatile int flip_pending;
static void on_flip(int fd, unsigned seq, unsigned s, unsigned us, unsigned crtc, void *d) { flip_pending = 0; }

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(drm_fd, obj, type);
    if (!p) return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(drm_fd, p->props[i]);
        if (pr) { if (!strcmp(pr->name, name)) id = pr->prop_id; drmModeFreeProperty(pr); }
    }
    drmModeFreeObjectProperties(p);
    return id;
}

static uint64_t prop_val(uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectProperties *p = drmModeObjectGetProperties(drm_fd, obj, type);
    uint64_t v = 0;
    for (uint32_t i = 0; i < p->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(drm_fd, p->props[i]);
        if (pr) { if (!strcmp(pr->name, name)) v = p->prop_values[i]; drmModeFreeProperty(pr); }
    }
    drmModeFreeObjectProperties(p);
    return v;
}

// Build a scanout framebuffer from a decoded DRM_PRIME frame (zero-copy import).
static uint32_t fb_from_frame(AVFrame *f)
{
    const AVDRMFrameDescriptor *d = (const AVDRMFrameDescriptor *)f->data[0];
    uint32_t oh[AV_DRM_MAX_PLANES] = {0};
    for (int i = 0; i < d->nb_objects; i++)
        if (drmPrimeFDToHandle(drm_fd, d->objects[i].fd, &oh[i]))
            return 0;

    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t mods[4] = {0};
    int n = 0;
    for (int l = 0; l < d->nb_layers; l++)
        for (int p = 0; p < d->layers[l].nb_planes; p++) {
            int oi = d->layers[l].planes[p].object_index;
            handles[n] = oh[oi];
            pitches[n] = d->layers[l].planes[p].pitch;
            offsets[n] = d->layers[l].planes[p].offset;
            mods[n]    = d->objects[oi].format_modifier;
            n++;
        }
    uint32_t fourcc = d->layers[0].format, fb = 0;
    if (opt_nv21 && fourcc == DRM_FORMAT_NV12) fourcc = DRM_FORMAT_NV21;  // opt-in chroma U/V swap; default off (the DE33 scans NV12 with correct chroma — relabeling to NV21 swaps Cb/Cr → red shows as dark blue)
    uint64_t m = d->objects[0].format_modifier;
    int rc;
    if (m != DRM_FORMAT_MOD_INVALID && m != DRM_FORMAT_MOD_LINEAR)
        rc = drmModeAddFB2WithModifiers(drm_fd, f->width, f->height, fourcc,
                                        handles, pitches, offsets, mods, &fb, DRM_MODE_FB_MODIFIERS);
    else
        rc = drmModeAddFB2(drm_fd, f->width, f->height, fourcc, handles, pitches, offsets, &fb, 0);
    if (rc) {
        static int dumped = 0;
        if (!dumped) {
            dumped = 1;
            LOG("AddFB2 desc: objs=%d layers=%d  layer0 fmt=%.4s nb_planes=%d  obj0 mod=%#llx",
                d->nb_objects, d->nb_layers, (char *)&d->layers[0].format,
                d->layers[0].nb_planes, (unsigned long long)d->objects[0].format_modifier);
            for (int l = 0; l < d->nb_layers; l++)
                for (int p = 0; p < d->layers[l].nb_planes; p++)
                    LOG("  L%d.P%d obj=%d pitch=%ld offset=%ld", l, p,
                        d->layers[l].planes[p].object_index,
                        (long)d->layers[l].planes[p].pitch, (long)d->layers[l].planes[p].offset);
        }
        LOG("AddFB2 failed (%.4s mod %#lx): %s", (char *)&fourcc, (unsigned long)m, strerror(errno));
        return 0;
    }
    return fb;
}

static void add_plane(drmModeAtomicReq *r, uint32_t pl,
                      uint32_t f_fb, uint32_t f_crtc, uint32_t f_cx, uint32_t f_cy, uint32_t f_cw, uint32_t f_ch,
                      uint32_t f_sx, uint32_t f_sy, uint32_t f_sw, uint32_t f_sh,
                      uint32_t fb, int dx, int dy, int dw, int dh, int sw, int sh)
{
    drmModeAtomicAddProperty(r, pl, f_fb, fb);
    drmModeAtomicAddProperty(r, pl, f_crtc, crtc_id);
    drmModeAtomicAddProperty(r, pl, f_cx, dx);
    drmModeAtomicAddProperty(r, pl, f_cy, dy);
    drmModeAtomicAddProperty(r, pl, f_cw, dw);
    drmModeAtomicAddProperty(r, pl, f_ch, dh);
    drmModeAtomicAddProperty(r, pl, f_sx, 0);
    drmModeAtomicAddProperty(r, pl, f_sy, 0);
    drmModeAtomicAddProperty(r, pl, f_sw, (uint64_t)sw << 16);
    drmModeAtomicAddProperty(r, pl, f_sh, (uint64_t)sh << 16);
}

/* Startup modeset: CRTC on, black primary full-screen. Video + OSD planes are
 * attached later (video on the first frame). Brings a screen up immediately so
 * the player shows something even before any signal. */
static void startup_modeset(uint32_t pfb)
{
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, conn_id, CON_CRTC, crtc_id);
    drmModeAtomicAddProperty(r, crtc_id, C_MODE, mode_blob);
    drmModeAtomicAddProperty(r, crtc_id, C_ACTIVE, 1);
    add_plane(r, pplane_id, Pp_FB, Pp_CRTC, Pp_CX, Pp_CY, Pp_CW, Pp_CH, Pp_SX, Pp_SY, Pp_SW, Pp_SH,
              pfb, 0, 0, mode.hdisplay, mode.vdisplay, mode.hdisplay, mode.vdisplay);
    if (Pp_ZPOS) drmModeAtomicAddProperty(r, pplane_id, Pp_ZPOS, 0);
    osd_add_to_commit(osd, r, 1);
    if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        DIE("startup modeset: %s", strerror(errno));
    drmModeAtomicFree(r);
}

/* Bring up / reconfigure the video overlay plane: SRC = the full vw x vh
 * frame, CRTC = rect d (either an aspect-fit scaled rect or a centred 1:1
 * one). Called on the first decoded frame and on stream-size changes. */
static void commit_video_plane(uint32_t vfb, int vw, int vh, fit_rect_t d)
{
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    add_plane(r, vplane_id, Vp_FB, Vp_CRTC, Vp_CX, Vp_CY, Vp_CW, Vp_CH, Vp_SX, Vp_SY, Vp_SW, Vp_SH,
              vfb, d.x, d.y, d.w, d.h, vw, vh);
    if (Vp_ZPOS)   drmModeAtomicAddProperty(r, vplane_id, Vp_ZPOS, video_zpos);
    if (Vp_CENC)   drmModeAtomicAddProperty(r, vplane_id, Vp_CENC, opt_enc);
    if (Vp_CRANGE) drmModeAtomicAddProperty(r, vplane_id, Vp_CRANGE, opt_range);
    if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        LOG("video plane commit: %s", strerror(errno));
    drmModeAtomicFree(r);
}

/* Centred 1:1 rect — the pre-scaling behavior (crops when the mode is
 * smaller than the video; the kernel clips the plane to the CRTC). */
static fit_rect_t centred_1to1(int vw, int vh)
{
    fit_rect_t d = { (mode.hdisplay - vw) / 2, (mode.vdisplay - vh) / 2, vw, vh };
    if (d.x < 0) d.x = 0;
    if (d.y < 0) d.y = 0;
    return d;
}

/* Will the kernel scale the video plane to rect d? TEST_ONLY commit — no
 * visible effect. An unpatched DE33 kernel rejects non-1:1 with ERANGE;
 * then we fall back to retuning the HDMI mode as before. */
static int probe_plane_scaling(uint32_t vfb, int vw, int vh, fit_rect_t d)
{
    if (d.w == vw && d.h == vh) return 1;     /* 1:1 — nothing to prove */
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    add_plane(r, vplane_id, Vp_FB, Vp_CRTC, Vp_CX, Vp_CY, Vp_CW, Vp_CH, Vp_SX, Vp_SY, Vp_SW, Vp_SH,
              vfb, d.x, d.y, d.w, d.h, vw, vh);
    int rc = drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_TEST_ONLY, NULL);
    if (rc) LOG("plane scaling unavailable (%s) — falling back to mode retune", strerror(errno));
    drmModeAtomicFree(r);
    return rc == 0;
}

// Page-flip the overlay plane to a new framebuffer (vsync'd, async + event).
static void flip(uint32_t vfb)
{
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, vplane_id, Vp_FB, vfb);
    flip_pending = 1;
    if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, NULL)) {
        flip_pending = 0;   // tolerate a failed flip; keep going
        LOG("atomic flip: %s", strerror(errno));
    }
    drmModeAtomicFree(r);
}

static void drain_flip(void)
{
    drmEventContext ev = { .version = 3, .page_flip_handler2 = on_flip };
    struct pollfd p = { .fd = drm_fd, .events = POLLIN };
    while (flip_pending) {
        if (poll(&p, 1, 1000) > 0) drmHandleEvent(drm_fd, &ev);
        else break;
    }
}

static enum AVPixelFormat get_drm_prime(AVCodecContext *c, const enum AVPixelFormat *fmts)
{
    for (; *fmts != AV_PIX_FMT_NONE; fmts++)
        if (*fmts == AV_PIX_FMT_DRM_PRIME) return *fmts;
    return AV_PIX_FMT_NONE;
}

/* Pick the connector's preferred mode (fallback 1080p, then first) to bring a
 * screen up before any signal. This is the STARTUP mode — the video plane is
 * HW-scaled to aspect-fit it on the first decoded frame; retune_mode() runs
 * only as the fallback when plane scaling is unavailable. */
static drmModeConnector *g_conn;
static void pick_preferred_mode(void)
{
    int mi = -1;
    for (int i = 0; i < g_conn->count_modes; i++)
        if (g_conn->modes[i].type & DRM_MODE_TYPE_PREFERRED) { mi = i; break; }
    if (mi < 0) for (int i = 0; i < g_conn->count_modes; i++)
        if (g_conn->modes[i].hdisplay == 1920 && g_conn->modes[i].vdisplay == 1080) { mi = i; break; }
    if (mi < 0) mi = 0;
    mode = g_conn->modes[mi];
    drmModeCreatePropertyBlob(drm_fd, &mode, sizeof(mode), &mode_blob);
    LOG("startup mode %s (%dx%d)", mode.name, mode.hdisplay, mode.vdisplay);
}

// Black XRGB8888 dumb primary FB, full screen (created once the mode is known).
static uint32_t make_black_primary(void)
{
    struct drm_mode_create_dumb cd = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
    drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd);
    uint32_t ph[4] = { cd.handle }, pp_[4] = { cd.pitch }, po[4] = { 0 }, pfb;
    drmModeAddFB2(drm_fd, mode.hdisplay, mode.vdisplay, DRM_FORMAT_XRGB8888, ph, pp_, po, &pfb, 0);
    struct drm_mode_map_dumb md = { .handle = cd.handle };
    drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &md);
    void *pm = mmap(0, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, md.offset);
    memset(pm, 0, cd.size); munmap(pm, cd.size);
    return pfb;
}

/* FALLBACK: auto-match the HDMI output to the decoded stream size. Used only
 * when plane scaling is unavailable (unpatched 1:1-only DE33 kernel — see
 * probe_plane_scaling) or forced via CITRUSPLAY_RETUNE=1. Without a scaler,
 * full-screen pixel-for-pixel display means driving the panel at the stream's
 * own resolution — otherwise a larger stream is cropped and a smaller one
 * letterboxed. The sink's EDID-preferred startup mode is often NOT the stream
 * size (FPV goggles advertise 720p100 preferred but also list 1080p). Switch
 * to the matching mode (highest refresh, non-interlaced) if the sink offers
 * one; otherwise keep the startup mode. Mode-sized resources (black primary,
 * OSD) are rebuilt for the new mode. */
static void retune_mode(int w, int h, uint32_t *pfb_io)
{
    if (mode.hdisplay == w && mode.vdisplay == h) return;   /* startup mode already matches */

    int mi = -1, best = -1;
    for (int i = 0; i < g_conn->count_modes; i++) {
        drmModeModeInfo *m = &g_conn->modes[i];
        if (m->hdisplay == w && m->vdisplay == h &&
            !(m->flags & DRM_MODE_FLAG_INTERLACE) && (int)m->vrefresh > best) {
            best = m->vrefresh; mi = i;
        }
    }
    if (mi < 0) {
        LOG("no %dx%d mode on sink — keeping %s; %dx%d stream will be %s",
            w, h, mode.name, w, h, (w > mode.hdisplay || h > mode.vdisplay) ? "cropped" : "letterboxed");
        return;
    }

    drmModeModeInfo nm = g_conn->modes[mi];
    uint32_t nblob = 0;
    if (drmModeCreatePropertyBlob(drm_fd, &nm, sizeof(nm), &nblob)) {
        LOG("retune: mode-blob create failed — keeping %s", mode.name);
        return;
    }
    if (mode_blob) drmModeDestroyPropertyBlob(drm_fd, mode_blob);
    mode = nm; mode_blob = nblob;
    LOG("retuned HDMI to %s (%dx%d@%d) to match the %dx%d stream",
        mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh, w, h);

    uint32_t newpfb = make_black_primary();        /* old pfb is the wrong size for the new mode */
    if (osd) {                                     /* OSD dumb buffer was sized to the old mode */
        osd_destroy(osd);
        osd = osd_create(drm_fd, crtc_id, osd_plane_id, &Op,
                         mode.hdisplay, mode.vdisplay, 16, osd_scale_now(), osd_zpos_max);
        if (!osd) LOG("OSD rebuild after retune failed — video unaffected");
    }
    startup_modeset(newpfb);
    if (*pfb_io) drmModeRmFB(drm_fd, *pfb_io);
    *pfb_io = newpfb;
}

typedef struct {
    int              fd;       /* UDP socket bound to the RTP port */
    rtp_h265_t       dep;      /* H.265 RTP depacketizer (RFC 7798) */
    AVCodecContext  *cc;
    AVCodecParserContext *parser;
    AVBufferRef     *hw;
    int got_key;   /* gate: don't feed the stateless decoder until the first IDR,
                    * else it wedges on P-frames with not-yet-decoded references */
} input_t;

/* Open a raw UDP socket on `port` and a low-delay Cedrus decoder. We own the
 * socket (no libavformat demuxer) so we can drain it to empty every wakeup and
 * present only the latest frame — bounding latency on a lossy link. Returns 0,
 * or -1 (caller retries). */
static int open_input(input_t *in, int port, int bufsz)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    /* SO_RCVBUFFORCE (needs CAP_NET_ADMIN, i.e. root) bypasses the net.core.rmem_max
     * cap so the full buffer applies without a sysctl wrapper; fall back to the
     * capped SO_RCVBUF if we're not privileged. */
    if (bufsz > 0 && setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &bufsz, sizeof bufsz) < 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof bufsz);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }

    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    AVCodecContext *cc = avcodec_alloc_context3(dec);
    if (!cc) { close(fd); return -1; }
    cc->flags |= AV_CODEC_FLAG_LOW_DELAY;     /* VPS/SPS/PPS arrive in-band */
    AVBufferRef *hw = NULL;
    if (av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_V4L2REQUEST, NULL, NULL, 0) < 0) {
        avcodec_free_context(&cc); close(fd); return -1;
    }
    cc->hw_device_ctx = av_buffer_ref(hw);
    cc->get_format = get_drm_prime;
    cc->extra_hw_frames = 8;
    if (avcodec_open2(cc, dec, NULL) < 0) {
        av_buffer_unref(&hw); avcodec_free_context(&cc); close(fd); return -1;
    }
    /* The stateless decoder needs slice/reference metadata; run each access unit
     * through the parser (same path a file decode takes). Our depacketizer hands
     * over complete AUs, so COMPLETE_FRAMES lets the parser emit them without
     * buffering for the next start code (lower latency). */
    in->parser = av_parser_init(AV_CODEC_ID_HEVC);
    if (in->parser) in->parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;
    else LOG("parser init failed — HW decode may stall");
    rtp_h265_init(&in->dep);
    in->got_key = 0;
    in->fd = fd; in->cc = cc; in->hw = hw;
    LOG("listening for H.265 RTP on udp:%d", port);
    return 0;
}

/* Tear down input + decoder. Does NOT touch DRM state or any held frame, so the
 * last image stays frozen on screen across a reconnect. */
static void close_input(input_t *in)
{
    if (in->parser) { av_parser_close(in->parser); in->parser = NULL; }
    if (in->cc)  avcodec_free_context(&in->cc);
    if (in->hw)  av_buffer_unref(&in->hw);
    rtp_h265_free(&in->dep);
    if (in->fd >= 0) { close(in->fd); in->fd = -1; }
}

int main(int argc, char **argv)
{
    opt_enc   = getenv("CITRUSPLAY_ENC")   ? atoi(getenv("CITRUSPLAY_ENC"))   : 1;  // BT.709
    opt_range = getenv("CITRUSPLAY_RANGE") ? atoi(getenv("CITRUSPLAY_RANGE")) : 0;  // limited
    opt_nv21  = getenv("CITRUSPLAY_NV21")  ? atoi(getenv("CITRUSPLAY_NV21"))  : 0;  // off: decoder exports true NV12; DE33 does NOT swap chroma (red↔blue if on)
    opt_debug = getenv("CITRUSPLAY_DEBUG") ? atoi(getenv("CITRUSPLAY_DEBUG")) : 0;
    opt_retune = getenv("CITRUSPLAY_RETUNE") ? atoi(getenv("CITRUSPLAY_RETUNE")) : 0;
    int bufsz = getenv("CITRUSPLAY_BUFSIZE") ? atoi(getenv("CITRUSPLAY_BUFSIZE")) : 26214400;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // ---- DRM resources (everything except the mode, which needs the video size) ----
    drm_fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) DIE("open card0");
    drmSetMaster(drm_fd);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drmModeRes *res = drmModeGetResources(drm_fd);
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes) { g_conn = c; break; }
        drmModeFreeConnector(c);
    }
    if (!g_conn) DIE("no connected output");
    conn_id = g_conn->connector_id;

    drmModeEncoder *enc = drmModeGetEncoder(drm_fd, g_conn->encoder_id ? g_conn->encoder_id : g_conn->encoders[0]);
    crtc_id = enc->crtc_id ? enc->crtc_id : res->crtcs[0];
    for (int i = 0; i < res->count_crtcs; i++) if (res->crtcs[i] == crtc_id) crtc_idx = i;

    drmModePlaneRes *pr = drmModeGetPlaneResources(drm_fd);
    for (uint32_t i = 0; i < pr->count_planes; i++) {
        drmModePlane *pl = drmModeGetPlane(drm_fd, pr->planes[i]);
        if (!(pl->possible_crtcs & (1 << crtc_idx))) { drmModeFreePlane(pl); continue; }
        int has_nv12 = 0, has_argb = 0;
        for (uint32_t f = 0; f < pl->count_formats; f++) {
            if (pl->formats[f] == DRM_FORMAT_NV12)     has_nv12 = 1;
            if (pl->formats[f] == DRM_FORMAT_ARGB8888) has_argb = 1;
        }
        uint64_t type = prop_val(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type");
        if (type == DRM_PLANE_TYPE_PRIMARY && !pplane_id) pplane_id = pl->plane_id;
        if (has_nv12 && type != DRM_PLANE_TYPE_PRIMARY && !vplane_id) vplane_id = pl->plane_id;
        else if (has_argb && type != DRM_PLANE_TYPE_PRIMARY && !osd_plane_id
                 && pl->plane_id != vplane_id) osd_plane_id = pl->plane_id;
        drmModeFreePlane(pl);
    }
    if (!vplane_id) DIE("no NV12-capable overlay plane (is kernel patch 0099 applied?)");
    if (!pplane_id) DIE("no primary plane");

    C_ACTIVE = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    C_MODE   = prop_id(crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    CON_CRTC = prop_id(conn_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    #define PP(P, plane) \
        P##_FB = prop_id(plane, DRM_MODE_OBJECT_PLANE, "FB_ID"); \
        P##_CRTC = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID"); \
        P##_CX = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_X"); \
        P##_CY = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y"); \
        P##_CW = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_W"); \
        P##_CH = prop_id(plane, DRM_MODE_OBJECT_PLANE, "CRTC_H"); \
        P##_SX = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_X"); \
        P##_SY = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_Y"); \
        P##_SW = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_W"); \
        P##_SH = prop_id(plane, DRM_MODE_OBJECT_PLANE, "SRC_H"); \
        P##_ZPOS = prop_id(plane, DRM_MODE_OBJECT_PLANE, "zpos");
    PP(Vp, vplane_id); PP(Pp, pplane_id);
    Vp_CENC   = prop_id(vplane_id, DRM_MODE_OBJECT_PLANE, "COLOR_ENCODING");
    Vp_CRANGE = prop_id(vplane_id, DRM_MODE_OBJECT_PLANE, "COLOR_RANGE");
    {
        drmModeObjectProperties *op = drmModeObjectGetProperties(drm_fd, vplane_id, DRM_MODE_OBJECT_PLANE);
        for (uint32_t i = 0; i < op->count_props; i++) {
            drmModePropertyRes *prp = drmModeGetProperty(drm_fd, op->props[i]);
            if (prp) {
                if (!strcmp(prp->name, "zpos") && prp->count_values == 2) vplane_zmax = prp->values[1];
                drmModeFreeProperty(prp);
            }
        }
        drmModeFreeObjectProperties(op);
    }
    if (osd_plane_id) {
        Op.fb_id   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
        Op.crtc_id = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
        Op.crtc_x  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
        Op.crtc_y  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
        Op.crtc_w  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
        Op.crtc_h  = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");
        Op.src_x   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
        Op.src_y   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
        Op.src_w   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
        Op.src_h   = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
        Op.zpos    = prop_id(osd_plane_id, DRM_MODE_OBJECT_PLANE, "zpos");
        drmModeObjectProperties *op = drmModeObjectGetProperties(drm_fd, osd_plane_id, DRM_MODE_OBJECT_PLANE);
        for (uint32_t i = 0; op && i < op->count_props; i++) {
            drmModePropertyRes *prp = drmModeGetProperty(drm_fd, op->props[i]);
            if (prp) {
                if (!strcmp(prp->name, "zpos") && prp->count_values == 2) osd_zpos_max = prp->values[1];
                drmModeFreeProperty(prp);
            }
        }
        if (op) drmModeFreeObjectProperties(op);
    }

    // ---- option parsing ----
    opt_port = 5600;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) opt_port = atoi(argv[++i]);
    }

    // ---- bring a screen up immediately (preferred mode, black primary) ----
    opt_osd       = getenv("CITRUSPLAY_OSD")       ? atoi(getenv("CITRUSPLAY_OSD"))       : 1;
    opt_osd_scale = getenv("CITRUSPLAY_OSD_SCALE") ? atoi(getenv("CITRUSPLAY_OSD_SCALE")) : 0;  /* 0 = auto-scale from resolution */

    pick_preferred_mode();
    uint32_t pfb = make_black_primary();

    /* Order planes: primary(0) < video < osd(top). With a mutable zpos we put
     * the OSD at its max and the video just below; otherwise natural plane order
     * (per the probe) already stacks the OSD plane above the video. */
    if (osd_plane_id && opt_osd) {
        video_zpos = (osd_zpos_max > 0) ? osd_zpos_max - 1 : 0;
        if (video_zpos > vplane_zmax) video_zpos = vplane_zmax;
        osd = osd_create(drm_fd, crtc_id, osd_plane_id, &Op,
                         mode.hdisplay, mode.vdisplay, 16, osd_scale_now(), osd_zpos_max);
        if (!osd) LOG("OSD disabled (osd_create failed) — video unaffected");
    } else {
        video_zpos = vplane_zmax;
        if (opt_osd && !osd_plane_id) LOG("OSD disabled (no ARGB overlay plane) — video unaffected");
    }
    startup_modeset(pfb);

    // ---- run forever: bind socket, depacketize, decode, present latest ----
    input_t in = { .fd = -1 };
    AVPacket *spkt = av_packet_alloc();   /* scratch for parser output; never unref'd (data is parser-owned) */
    AVFrame  *frame = av_frame_alloc();
    AVFrame  *held[2] = { NULL, NULL };
    uint32_t  heldfb[2] = { 0, 0 };
    int slot = 0, video_up = 0, warned_sw = 0;
    int cur_w = 0, cur_h = 0;   /* resolution currently committed to the plane/mode */
    AVFrame *pending = NULL; uint32_t pending_fb = 0;  /* newest decoded frame, not yet shown */
    long shown = 0;
    static uint8_t rxbuf[65536];

    while (running && open_input(&in, opt_port, bufsz) < 0) {
        LOG("socket bind failed on udp:%d — retrying ...", opt_port);
        nsleep_ms(500);
    }

    while (running) {
        osd_tick(osd);     /* self-throttled to 1 Hz; in-place buffer, no commit */

        /* Drain EVERY packet currently in the socket, decoding each completed
         * access unit, but keep only the newest decoded frame. A backlog (e.g.
         * after a loss-induced stall) is consumed at full decoder speed — not at
         * the display refresh rate — so the delay never accumulates. */
        for (;;) {
            ssize_t n = recv(in.fd, rxbuf, sizeof rxbuf, MSG_DONTWAIT);
            if (n <= 0) break;                       /* socket drained (EAGAIN) */

            const uint8_t *au; size_t au_len;
            if (rtp_h265_input(&in.dep, rxbuf, (size_t)n, &au, &au_len) != 1)
                continue;                            /* AU not complete, or dropped on loss */

            /* Reframe the access unit through the parser for the stateless decoder. */
            const uint8_t *adata = au; int asize = (int)au_len;
            while (asize > 0 && running) {
                if (in.parser) {
                    int used = av_parser_parse2(in.parser, in.cc, &spkt->data, &spkt->size,
                                                adata, asize, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                    if (used < 0) break;
                    adata += used; asize -= used;
                    if (spkt->size <= 0) { if (used == 0) break; continue; }
                    /* Decode only from an IDR; the gate is re-armed on a decode
                     * error (below), so loss skips to the next keyframe instead of
                     * feeding broken frames that wedge the stateless decoder. */
                    if (!in.got_key) {
                        if (in.parser->key_frame == 1) in.got_key = 1;
                        else continue;
                    }
                } else {
                    spkt->data = (uint8_t *)adata; spkt->size = asize; asize = 0;
                }
                if (avcodec_send_packet(in.cc, spkt) != 0) { in.got_key = 0; continue; }
                int rr = 0;
                while (running && (rr = avcodec_receive_frame(in.cc, frame)) == 0) {
                    if (frame->format != AV_PIX_FMT_DRM_PRIME) {
                        if (!warned_sw) { LOG("not hardware-decoded (got %s) — skipping",
                                              av_get_pix_fmt_name(frame->format)); warned_sw = 1; }
                        av_frame_unref(frame); continue;
                    }
                    uint32_t fb = fb_from_frame(frame);
                    if (!fb) { av_frame_unref(frame); continue; }
                    if (pending) { drmModeRmFB(drm_fd, pending_fb); av_frame_free(&pending); }
                    pending = frame; pending_fb = fb;
                    frame = av_frame_alloc();
                }
                if (rr && rr != AVERROR(EAGAIN) && rr != AVERROR_EOF)
                    in.got_key = 0;
            }
        }

        /* Socket drained = caught up to live: present the newest frame (vsync-paced). */
        if (pending) {
            /* (Re)configure the plane whenever the stream size changes — at the
             * first frame, or mid-stream when the encoder switches resolution.
             * A bare flip() only swaps FB_ID; it leaves the plane's SRC/CRTC
             * rectangle (and the HDMI mode) at the old size, so a differently
             * sized FB would fail the atomic commit and the screen would blank.
             * Re-running the full setup re-fits the plane (and retunes the mode in fallback). */
            if (!video_up || pending->width != cur_w || pending->height != cur_h) {
                drain_flip();   /* no flip may be in flight across a reconfigure */
                fit_rect_t d = fit_rect(pending->width, pending->height,
                                        mode.hdisplay, mode.vdisplay);
                int scaled = !opt_retune && d.w > 0 &&
                             probe_plane_scaling(pending_fb, pending->width, pending->height, d);
                if (!scaled) {   /* forced or kernel can't scale: old behavior */
                    retune_mode(pending->width, pending->height, &pfb);
                    d = centred_1to1(pending->width, pending->height);
                }
                commit_video_plane(pending_fb, pending->width, pending->height, d);
                cur_w = pending->width; cur_h = pending->height;
                video_up = 1;
                LOG("playing %dx%d -> %dx%d%+d%+d in %s%s.", cur_w, cur_h,
                    d.w, d.h, d.x, d.y, mode.name, scaled ? " (HW scaled)" : " (1:1)");
            } else {
                drain_flip(); flip(pending_fb);
            }
            shown++;
            if (held[slot]) { drmModeRmFB(drm_fd, heldfb[slot]); av_frame_free(&held[slot]); }
            held[slot] = pending; heldfb[slot] = pending_fb; slot ^= 1;
            pending = NULL; pending_fb = 0;
        }

        /* Wait for the next packet — POLLIN wakes immediately on arrival; the
         * timeout just bounds latency for Ctrl-C and the 1 Hz OSD tick. */
        struct pollfd pfd = { .fd = in.fd, .events = POLLIN };
        poll(&pfd, 1, 250);
    }

    drain_flip();
    LOG("presented %ld frames", shown);
    if (pending) { drmModeRmFB(drm_fd, pending_fb); av_frame_free(&pending); }
    for (int i = 0; i < 2; i++) if (held[i]) { drmModeRmFB(drm_fd, heldfb[i]); av_frame_free(&held[i]); }
    if (pfb) drmModeRmFB(drm_fd, pfb);
    av_frame_free(&frame); av_packet_free(&spkt);
    close_input(&in);
    osd_destroy(osd);
    drmDropMaster(drm_fd);
    close(drm_fd);
    return 0;
}
