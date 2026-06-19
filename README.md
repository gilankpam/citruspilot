# CitrusPilot

Low-latency **FPV video player** for the Orange Pi Zero 2W (Allwinner **H618**)
and other sunxi boards with a **Cedrus** VPU + **DE33** display engine. The sunxi
analog of [`PixelPilot_rk`](https://github.com/gilankpam/PixelPilot_rk): take the
drone's H.265 off a wfb-ng link, hardware-decode on Cedrus, and scan
**direct-to-plane** (no GPU compositing) with the CPU mostly idle.

> The name: the Allwinner video engine is **Cedrus** → **Citrus**, it runs on an
> **Orange** Pi, and it's a **PixelPilot** for FPV — three puns, one word.

```
drone (OpenIPC VTX, H.265) → wfb-ng link → wfb_rx → RTP udp:5600
  → [ CitrusPilot: libav RTP → Cedrus decode → DRM overlay plane ]
```

CitrusPilot is the **player only**. It consumes the de-FEC'd RTP that an
**external** `wfb_rx` forwards to a local UDP port. Building and running the radio
stack itself (the `rtl8812au` driver, `wfb-ng`) is out of scope for this repo — it
lives separately; `wfb_rx` is just the upstream that must be feeding `udp:5600`.

The decode+display back-end is proven in
[`h618-mainline-video`](../../h618-mainline-video) (`drmvid`); CitrusPilot adds the
live RTP front-end on top of it.

## Layout

- `player/` — the player: `wfbvid` (RTP → Cedrus → DRM plane) and the `wfbplay`
  launcher
- `smoke/` — synthetic-RTP harness to validate the player front-end (no radio HW)
- `docs/` — design specs + findings

## Build & run

```sh
# build (on the board)
cc player/wfbvid.c player/osd.c player/osd_render.c player/stats.c \
   -o /usr/local/bin/wfbvid \
   $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
cp player/wfbplay /usr/local/bin/

# upstream (external): wfb_rx de-FECs the drone link -> RTP udp:5600
wfb_rx -K gs.key -c 127.0.0.1 -u 5600 -p <radio_port> <monitor_iface> &

# play: listens forever on udp:5600, HUD overlay on, console restored on exit
wfbplay --port 5600
```

`wfbvid` listens on a UDP port (default 5600) with a built-in SDP, ingests
the live RTP with a large UDP socket buffer + low-delay flags, HW-decodes on
Cedrus, and scans direct-to-plane — **no PTS pacing** (present on decode), the
screen brought up at **startup** (black primary + OSD) with the video plane added
on the first frame, tolerant of packet-loss decode errors, and runs forever — on
a stream drop the last frame stays **frozen** and it reconnects on the next IDR.
Env knobs: `WFBVID_OSD` (1=on, default), `WFBVID_OSD_SCALE` (glyph scale, default 2),
`WFBVID_PT` (RTP payload type, default 97), `WFBVID_NV21` (default 0; set 1 only to
force a Cb/Cr swap — on this DE33 it makes colours wrong, e.g. red→dark blue),
`WFBVID_BUFSIZE`, `WFBVID_ENC`/`WFBVID_RANGE`.

Requires the patched mainline kernel (NV12 on the DE33 VI plane, patch `0099`) and
the v4l2request ffmpeg from `h618-mainline-video`.

## Status

| Item | State |
|---|---|
| player pipe (RTP→Cedrus), front-end choice | ✅ libav/SDP |
| `wfbvid` live RTP→plane | ✅ validated end-to-end — live 1080p60 H.265 on the plane |
| OSD overlay (system: CPU/mem/temp) + run-forever daemon | ✅ v1 |

> The radio prerequisite (rtl8812au + wfb-ng bring-up) is documented separately and
> is **not** part of this repo.
