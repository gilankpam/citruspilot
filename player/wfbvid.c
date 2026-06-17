// wfbvid — direct-to-plane HW video player for a LIVE RTP stream (wfb-ng FPV) on
// the Allwinner H618. Derived from drmvid (h618-mainline-video): the same
// Cedrus (v4l2request) -> DRM-PRIME NV12 -> hardware overlay-plane scanout, zero
// copy, no GPU compositing. The difference is the front-end: instead of a file
// it ingests the de-FEC'd RTP that wfb_rx forwards to a local UDP port.
//
// Live-specific changes vs drmvid (see docs/findings/stage0-player-pipe.md):
//   * libav RTP/SDP input with a large UDP socket buffer (bursty RTP) + low-delay
//   * NO presentation pacing — each frame is scanned out the instant it decodes
//   * mode pick + modeset are DEFERRED to the first decoded frame (a live stream
//     has no container header, so the resolution isn't known up front)
//   * tolerant of decode errors (packet loss -> corrupt frames until the next
//     IDR) — it skips bad frames, never crashes
//
// Requires kernel patch 0099 (NV12 on the DE33 VI plane) + linear-NV12 Cedrus,
// and scans 1:1 (the DE33 VI scaler can't upscale), centring sub-mode video.
//
// Build: cc wfbvid.c -o wfbvid $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
// Use:   wfbvid stream.sdp        (SDP describing the wfb_rx RTP, e.g. udp:5600)

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

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixdesc.h>

#define DIE(...) do { fprintf(stderr, "wfbvid: " __VA_ARGS__); fprintf(stderr, "\n"); exit(1); } while (0)
#define LOG(...) do { fprintf(stderr, "wfbvid: " __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

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

static volatile sig_atomic_t running = 1;
static void on_signal(int s) { (void)s; running = 0; }

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
    if (opt_nv21 && fourcc == DRM_FORMAT_NV12) fourcc = DRM_FORMAT_NV21;  // DE33 chroma U/V swap workaround
    uint64_t m = d->objects[0].format_modifier;
    int rc;
    if (m != DRM_FORMAT_MOD_INVALID && m != DRM_FORMAT_MOD_LINEAR)
        rc = drmModeAddFB2WithModifiers(drm_fd, f->width, f->height, fourcc,
                                        handles, pitches, offsets, mods, &fb, DRM_MODE_FB_MODIFIERS);
    else
        rc = drmModeAddFB2(drm_fd, f->width, f->height, fourcc, handles, pitches, offsets, &fb, 0);
    if (rc) { LOG("AddFB2 failed (%.4s mod %#lx): %s", (char *)&fourcc, (unsigned long)m, strerror(errno)); return 0; }
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

// Full modeset: CRTC on, black primary full-screen, video on the overlay 1:1 (centred).
static void modeset(uint32_t vfb, uint32_t pfb, int vw, int vh)
{
    int dx = (mode.hdisplay - vw) / 2, dy = (mode.vdisplay - vh) / 2;
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    drmModeAtomicReq *r = drmModeAtomicAlloc();
    drmModeAtomicAddProperty(r, conn_id, CON_CRTC, crtc_id);
    drmModeAtomicAddProperty(r, crtc_id, C_MODE, mode_blob);
    drmModeAtomicAddProperty(r, crtc_id, C_ACTIVE, 1);
    add_plane(r, pplane_id, Pp_FB, Pp_CRTC, Pp_CX, Pp_CY, Pp_CW, Pp_CH, Pp_SX, Pp_SY, Pp_SW, Pp_SH,
              pfb, 0, 0, mode.hdisplay, mode.vdisplay, mode.hdisplay, mode.vdisplay);
    add_plane(r, vplane_id, Vp_FB, Vp_CRTC, Vp_CX, Vp_CY, Vp_CW, Vp_CH, Vp_SX, Vp_SY, Vp_SW, Vp_SH,
              vfb, dx, dy, vw, vh, vw, vh);
    if (Pp_ZPOS) drmModeAtomicAddProperty(r, pplane_id, Pp_ZPOS, 0);
    if (Vp_ZPOS) drmModeAtomicAddProperty(r, vplane_id, Vp_ZPOS, vplane_zmax);
    if (Vp_CENC)   drmModeAtomicAddProperty(r, vplane_id, Vp_CENC, opt_enc);
    if (Vp_CRANGE) drmModeAtomicAddProperty(r, vplane_id, Vp_CRANGE, opt_range);
    if (drmModeAtomicCommit(drm_fd, r, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL))
        DIE("atomic modeset: %s", strerror(errno));
    drmModeAtomicFree(r);
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

// Pick a scanout mode for the (now known) video size: exact match -> 1080p -> first.
static drmModeConnector *g_conn;
static void pick_mode(int vw, int vh)
{
    int mi = -1;
    for (int i = 0; i < g_conn->count_modes; i++)
        if (g_conn->modes[i].hdisplay == vw && g_conn->modes[i].vdisplay == vh) { mi = i; break; }
    if (mi < 0) for (int i = 0; i < g_conn->count_modes; i++)
        if (g_conn->modes[i].hdisplay == 1920 && g_conn->modes[i].vdisplay == 1080) { mi = i; break; }
    if (mi < 0) mi = 0;
    mode = g_conn->modes[mi];
    drmModeCreatePropertyBlob(drm_fd, &mode, sizeof(mode), &mode_blob);
    LOG("video %dx%d -> mode %s (%s)", vw, vh, mode.name,
        (mode.hdisplay == vw && mode.vdisplay == vh) ? "1:1" : "centred");
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

int main(int argc, char **argv)
{
    if (argc < 2) DIE("usage: wfbvid <stream.sdp | rtp://...>  (live RTP from wfb_rx)");
    const char *path = argv[1];
    opt_enc   = getenv("WFBVID_ENC")   ? atoi(getenv("WFBVID_ENC"))   : 1;  // BT.709
    opt_range = getenv("WFBVID_RANGE") ? atoi(getenv("WFBVID_RANGE")) : 0;  // limited
    opt_nv21  = getenv("WFBVID_NV21")  ? atoi(getenv("WFBVID_NV21"))  : 1;  // DE33 chroma swap workaround
    opt_debug = getenv("WFBVID_DEBUG") ? atoi(getenv("WFBVID_DEBUG")) : 0;
    const char *bufsz = getenv("WFBVID_BUFSIZE") ? getenv("WFBVID_BUFSIZE") : "26214400";

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
        int has_nv12 = 0;
        for (uint32_t f = 0; f < pl->count_formats; f++) if (pl->formats[f] == DRM_FORMAT_NV12) has_nv12 = 1;
        uint64_t type = prop_val(pl->plane_id, DRM_MODE_OBJECT_PLANE, "type");
        if (type == DRM_PLANE_TYPE_PRIMARY && !pplane_id) pplane_id = pl->plane_id;
        if (has_nv12 && type != DRM_PLANE_TYPE_PRIMARY && !vplane_id) vplane_id = pl->plane_id;
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

    // ---- open the live RTP/SDP input (big socket buffer + low latency) ----
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "protocol_whitelist", "file,crypto,data,udp,rtp", 0);
    av_dict_set(&opts, "buffer_size", bufsz, 0);          // SO_RCVBUF — absorb bursty RTP
    av_dict_set(&opts, "reorder_queue_size", "256", 0);   // modest reorder for low latency
    av_dict_set(&opts, "max_delay", "500000", 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    av_dict_set(&opts, "flags", "low_delay", 0);

    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, &opts) < 0) DIE("cannot open input %s", path);
    if (avformat_find_stream_info(fmt, NULL) < 0) DIE("no stream info");
    int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vs < 0) DIE("no video stream in %s", path);
    AVStream *st = fmt->streams[vs];
    LOG("input: %s, codec %s", path, avcodec_get_name(st->codecpar->codec_id));

    // ---- decoder: Cedrus via v4l2request, low-delay ----
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *cc = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(cc, st->codecpar);
    cc->flags |= AV_CODEC_FLAG_LOW_DELAY;
    AVBufferRef *hw = NULL;
    if (av_hwdevice_ctx_create(&hw, AV_HWDEVICE_TYPE_V4L2REQUEST, NULL, NULL, 0) < 0)
        DIE("v4l2request device create failed");
    cc->hw_device_ctx = av_buffer_ref(hw);
    cc->get_format = get_drm_prime;
    cc->extra_hw_frames = 8;
    if (avcodec_open2(cc, dec, NULL) < 0) DIE("avcodec_open2");

    // ---- decode + present loop (no pacing; modeset on first frame) ----
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *held[2] = { NULL, NULL };
    uint32_t heldfb[2] = { 0, 0 };
    int slot = 0, first = 1, warned_sw = 0;
    uint32_t pfb = 0;
    long shown = 0;

    LOG("waiting for stream...");
    while (running) {
        if (av_read_frame(fmt, pkt) < 0) break;     // stream ended / fatal error
        if (pkt->stream_index == vs) {
            if (avcodec_send_packet(cc, pkt) == 0) {
                while (running && avcodec_receive_frame(cc, frame) == 0) {
                    if (frame->format != AV_PIX_FMT_DRM_PRIME) {
                        if (!warned_sw) { LOG("not hardware-decoded (got %s) — skipping",
                                              av_get_pix_fmt_name(frame->format)); warned_sw = 1; }
                        av_frame_unref(frame); continue;
                    }
                    uint32_t fb = fb_from_frame(frame);
                    if (!fb) { av_frame_unref(frame); continue; }   // bad frame -> skip, never crash

                    if (first) {
                        pick_mode(frame->width, frame->height);
                        pfb = make_black_primary();
                        modeset(fb, pfb, frame->width, frame->height);
                        first = 0;
                        LOG("playing.");
                    } else {
                        drain_flip();
                        flip(fb);
                    }
                    shown++;

                    if (held[slot]) { drmModeRmFB(drm_fd, heldfb[slot]); av_frame_free(&held[slot]); }
                    held[slot] = frame; heldfb[slot] = fb; slot ^= 1;
                    frame = av_frame_alloc();
                }
            }
        }
        av_packet_unref(pkt);
    }
    drain_flip();
    LOG("presented %ld frames", shown);

    for (int i = 0; i < 2; i++) if (held[i]) { drmModeRmFB(drm_fd, heldfb[i]); av_frame_free(&held[i]); }
    if (pfb) drmModeRmFB(drm_fd, pfb);
    av_frame_free(&frame); av_packet_free(&pkt);
    avcodec_free_context(&cc); av_buffer_unref(&hw);
    avformat_close_input(&fmt);
    drmDropMaster(drm_fd);
    close(drm_fd);
    return 0;
}
