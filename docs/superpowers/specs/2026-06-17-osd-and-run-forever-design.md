# CitrusPilot — OSD overlay + resilient run-forever player (v1 design)

Status: approved 2026-06-17. Next: implementation plan (writing-plans).

## Goal

Add two features to the CitrusPilot player (`wfbvid`):

1. **System-stats OSD** — an always-on heads-up overlay showing CPU load, memory
   used, and SoC temperature, composited *above* the live video.
2. **Resilient run-forever player** — instead of exiting when the stream drops,
   the player runs as a daemon: it listens on a configurable UDP port, waits for
   RTP indefinitely, freezes the last frame on signal loss, and re-syncs when the
   link returns.

Both keep the player's defining property intact: **zero-copy, direct-to-plane
video** (VPU NV12 → DRM overlay plane, no GPU compositing, no PTS pacing). The
OSD must not touch the video path; the daemon behaviour must not add latency to
flowing video.

Out of scope for v1 (explicit future work): wfb link stats (RSSI/SNR, packets/
FEC), video status (fps/decode errors), OSD toggling/input handling, OSD corner
configuration.

## Decisions (locked during brainstorming)

| Decision | Choice |
|---|---|
| OSD content (v1) | System only: CPU load, mem used, temperature |
| OSD visibility | Always-on, no input handling |
| OSD compositing | Dedicated ARGB8888 overlay plane above the video (DE33 HW blend) |
| Text rendering | Baked-in 8×16 bitmap font, integer-scaled — no new deps |
| Port input | `wfbvid [--port N]`, default 5600; built-in H.265/PT97 SDP template |
| Signal loss | Freeze the last decoded frame; OSD keeps updating |
| Screen bring-up | Modeset at startup (preferred mode) so OSD shows with no signal |

## Architecture & data flow

```
/proc/stat, /proc/meminfo, /sys/class/thermal  ──(1 Hz sample)──┐
                                                                 ▼
                                          render text → small ARGB dumb buffer
                                                                 ▼
   VPU NV12 frame ──(every frame, zero-copy)──► video plane  ┐
                                                             ├─► one atomic commit ─► DE33 ─► HDMI
   OSD ARGB buffer ──(folded in only when an update is due)─►┘
```

- The OSD plane's framebuffer swap rides **inside the existing video page-flip
  atomic commit** — one commit per flip. This avoids `EBUSY` from competing
  commits and keeps the two planes tear-free relative to each other.
- zpos is arranged `primary(0) < video < osd(top)`.
- During a stall (no packets) the loop still issues OSD-only atomic commits so
  the HUD keeps refreshing while the frozen video frame stays onscreen.

## Plane model & first step: capability probe

The player today enumerates only `primary + one NV12 overlay`. The OSD needs a
third plane on the same CRTC that supports an alpha format (`ARGB8888` /
`ABGR8888`) and can sit above the video in zpos.

**Step 1 of implementation is a board-side probe**, captured as a
`docs/findings/` note: enumerate every plane on the CRTC — type, supported
formats, and zpos range — to confirm a usable alpha plane exists and learn its
constraints (notably whether it can be a small positioned plane or must be
full-screen).

Outcomes:
- **Usable alpha plane, positionable** → primary design: a small ARGB buffer
  sized to the HUD box, positioned in a corner.
- **Alpha plane requires full-screen** → same approach, but a full-screen mostly
  transparent ARGB buffer (~8 MB + per-frame blend bandwidth — acceptable
  fallback).
- **No alpha plane at all** → log a warning and run **without** OSD. The video
  must never fail because the OSD can't initialise. (Primary-margin rendering is
  noted as a possible future fallback, not built in v1.)

## Components

New, isolated modules so `wfbvid.c` stays focused:

- **`player/font8x16.h`** — baked-in public-domain 8×16 ASCII bitmap font
  (printable range 0x20–0x7E), data only, no dependency.
- **`player/osd.h` / `player/osd.c`** — the OSD module. Owns its ARGB buffer(s),
  stats sampling, and glyph rendering. Proposed interface:
  - `osd_t *osd_create(int drm_fd, uint32_t crtc_id, uint32_t plane_id,
    const osd_plane_props *props, int screen_w, int screen_h, int scale)` —
    allocate buffer(s), query/cache plane prop ids, render the first frame.
  - `int osd_tick(osd_t *o)` — call each loop iteration; internally throttled to
    1 Hz; re-renders the back buffer and returns 1 when an update is due.
  - `void osd_add_to_commit(osd_t *o, drmModeAtomicReq *r, int initial)` — add the
    OSD plane props to an atomic req (FB always; CRTC_ID/position/zpos on the
    initial modeset).
  - `void osd_destroy(osd_t *o)`.
- **`wfbvid.c`** — selects the OSD plane during enumeration, calls `osd_create`
  at modeset, `osd_tick` each iteration, and folds `osd_add_to_commit` into
  `modeset()` / `flip()`.

The OSD module reaches into no `wfbvid` internals beyond what is passed to
`osd_create`; it can be reasoned about and tested independently.

## Stats sources & layout

- **CPU**: `/proc/stat` line `cpu` — busy/total jiffies delta between consecutive
  1 Hz samples → aggregate load %.
- **MEM**: `/proc/meminfo` — `MemTotal − MemAvailable` → used MB / total MB.
- **TEMP**: max of `/sys/class/thermal/thermal_zone*/temp` (millidegrees) → °C.

Layout: three short lines, **top-left** (hardcoded for v1), drawn on a ~50%-alpha
black box for legibility over bright video; white glyphs, integer scale 2×
(16×32 px). Small positioned buffer (the HUD box), not full-screen, when the
plane allows it.

```
CPU  23%
MEM  312/1024 MB
TEMP 48 C
```

## Input lifecycle — run forever

- **Built-in SDP**: at startup, compose a minimal SDP from a baked template with
  the CLI port substituted:
  ```
  v=0
  o=- 0 0 IN IP4 127.0.0.1
  s=CitrusPilot
  c=IN IP4 127.0.0.1
  t=0 0
  m=video <PORT> RTP/AVP 97
  a=rtpmap:97 H265/90000
  ```
  Written once to a temp file (`mkstemp`), reused for every (re)open, unlinked at
  exit. Codec/PT hardcoded **H.265 / 97**, with an optional `WFBVID_PT` env
  override. In-band VPS/SPS/PPS on the IDR supply the parameter sets (brief
  warm-up noise until the first IDR — the known, accepted issue).
- **Interruptible reads**: set the UDP `timeout` (~250 ms) and an
  `interrupt_callback` tied to `running`, so the loop wakes ~4×/s during a stall
  to refresh the OSD and honour SIGINT/SIGTERM. Ensure `avformat_open_input` /
  `find_stream_info` do not block waiting for packets (codec is known from the
  SDP rtpmap; keep probe small).
- **Main loop**:
  ```
  open input (no packets required to open)
  while running:
      r = av_read_frame(fmt, pkt)
      if r is timeout/EAGAIN:        // no data right now
          osd_tick(); commit OSD if an update is due
          continue                   // keep the same socket; do NOT exit
      if r < 0:                      // hard error
          tear down input + codec; KEEP drm state, OSD, and on-screen frame
          reopen input + codec; continue
      // got a packet
      decode → present (existing zero-copy path); osd_tick piggybacks on the flip
  ```
- **Freeze last frame**: the on-screen DRM framebuffer and its `AVFrame` are
  preserved across input/codec teardown — the displayed frame's `held[]`/FB is
  not freed until a *new* frame arrives. The `AVFrame` ref keeps the VPU
  buffer/pool alive after `avcodec_free_context`, so the frozen image keeps
  scanning out. On the first new frame: if its resolution differs from the
  current mode, re-`pick_mode`/`modeset`; otherwise just flip, then release the
  old frame.

## Screen bring-up

Modeset happens **at startup** using the connector's preferred mode (black
primary + OSD plane up immediately), rather than deferred to the first frame.
This makes the OSD / black screen appear the instant the player starts, even with
no signal. When the first video frame arrives, re-modeset only if its resolution
differs from the preferred mode; otherwise begin flipping video directly.

## CLI & repo changes

- `wfbvid [--port N]` — default **5600**; no positional SDP argument.
- `wfbplay` updated to a `[--port N]` passthrough (drops its SDP argument).
- `player/wfb-h265.sdp` is removed (no longer used; the template lives in code).
- Build now compiles two source files:
  ```sh
  cc player/wfbvid.c player/osd.c -o /usr/local/bin/wfbvid \
     $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
  ```

## Config knobs (match existing `WFBVID_*` style)

| Env | Default | Meaning |
|---|---|---|
| `WFBVID_OSD` | 1 | Enable the OSD overlay |
| `WFBVID_OSD_SCALE` | 2 | Integer glyph scale |
| `WFBVID_PT` | 97 | RTP payload type in the built-in SDP |
| (existing) `WFBVID_NV21`, `WFBVID_BUFSIZE`, `WFBVID_ENC`, `WFBVID_RANGE` | — | unchanged |

`WFBVID_OSD_POS` (corner selection) is deferred to v2.

## Testing

All drivable from the existing `smoke/` synthetic-RTP harness — no radio HW.

1. **Plane probe** — enumeration finding committed under `docs/findings/`.
2. **Build** — compiles via the two-file pkg-config command above.
3. **OSD visible** — over flowing synthetic video, the HUD is composited above
   the video, refreshes ~1 Hz, and values cross-check against `top` / `free -m` /
   `cat /sys/class/thermal/thermal_zone*/temp`.
4. **Cold start, no source** — launch player first → black + live OSD, no crash;
   start the source → video locks on the first IDR.
5. **Mid-stream drop** — kill the RTP source → last frame freezes while the OSD
   keeps updating; restart the source → re-syncs on the next IDR.
6. **Port arg** — `--port` binds the given port; default is 5600.
7. **No regression** — with the OSD on, the decode path's frame throughput and
   CPU stay at the pre-OSD baseline (the OSD must not perturb scanout).

## Risks & open items

- **Plane availability** (resolved by Step 1 probe). If no alpha plane exists,
  OSD degrades off cleanly; video unaffected.
- **Single combined atomic commit** for video flip + OSD must be validated on the
  DE33 (formats/zpos/position accepted together).
- **Frozen-frame buffer lifetime** across `avcodec_free_context` relies on the
  `AVFrame` ref keeping the hwframes pool alive — verify no use-after-free and no
  pool exhaustion when the new codec allocates a fresh pool while one old frame
  is still held.
- **Resolution change across reconnects** handled by the re-modeset-on-mismatch
  path; assume same drone/mode in the common case.
```
