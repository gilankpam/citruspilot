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

The decode+display back-end is derived from `drmvid`, a proven file-based player
for this hardware; CitrusPilot adds the live RTP front-end on top of it.

## Layout

- `src/` — the player: `citrusplay` (RTP → Cedrus → DRM plane), plus `tools/` board
  diagnostics and `tests/` host unit tests
- `smoke/` — synthetic-RTP harness to validate the player front-end (no radio HW)
- `docs/` — design specs + findings

## Build & run

```sh
# build (on the board) + install citrusplay to /usr/local/bin
make && make install

# cross-build from an x86 dev host against a buildroot staging sysroot:
#   make SYSROOT=/path/to/output/<defconfig>/staging \
#        CC=/path/to/host/bin/aarch64-none-linux-gnu-gcc

# upstream (external): wfb_rx de-FECs the drone link -> RTP udp:5600
wfb_rx -K gs.key -c 127.0.0.1 -u 5600 -p <radio_port> <monitor_iface> &

# play: listens forever on udp:5600, HUD overlay on. Run as root (it takes DRM
# master and force-sets a large UDP buffer) on a free console.
citrusplay --port 5600
```

`citrusplay` listens on a UDP port (default 5600) with a built-in SDP, ingests
the live RTP with a large UDP socket buffer + low-delay flags, HW-decodes on
Cedrus, and scans direct-to-plane — **no PTS pacing** (present on decode), the
screen brought up at **startup** (black primary + OSD) with the video plane added
on the first frame, tolerant of packet-loss decode errors, and runs forever — on
a stream drop the last frame stays **frozen** and it reconnects on the next IDR.
Env knobs: `CITRUSPLAY_OSD` (1=on, default), `CITRUSPLAY_OSD_SCALE` (glyph scale, default 2),
`CITRUSPLAY_PT` (RTP payload type, default 97), `CITRUSPLAY_NV21` (default 0; set 1 only to
force a Cb/Cr swap — on this DE33 it makes colours wrong, e.g. red→dark blue),
`CITRUSPLAY_BUFSIZE`, `CITRUSPLAY_ENC`/`CITRUSPLAY_RANGE`.

Requires the patched mainline kernel (NV12 on the DE33 VI plane, patch `0099`) and
a v4l2request-patched ffmpeg (the `ffmpeg-v4l2request` build).

## Limitations

- **No video scaling (DE33 VI plane is 1:1 only).** The DE33 video plane cannot
  scale — it rejects any commit where the source and destination rects differ
  (probed: 1:1 accepted, up- *and* down-scale rejected with `ERANGE`). So the
  decoded frame is scanned out at its **native size** in the display mode. To
  avoid cropping (mode smaller than the stream) or black padding (mode larger),
  `citrusplay` **auto-matches the HDMI output mode to the stream** on the first
  decoded frame: it brings the screen up at startup in the sink's EDID-preferred
  mode, then — once it knows the real video size — retunes the panel to the EDID
  mode of the same resolution (highest non-interlaced refresh) if the sink offers
  one (see `retune_mode` in `src/citrusplay.c`). This matters because many FPV
  goggles advertise **720p as their preferred mode** even though they also list
  1080p — so a 1080p stream would otherwise be cropped to a 720p viewport. If the
  sink has no mode matching the stream, the startup mode is kept and the frame is
  centred (cropped or letterboxed) as before. Unlike Rockchip (`PixelPilot_rk`),
  which fills the screen via the VOP plane's hardware scaler, the DE33 has no such
  scaler, so plane-side fill is not an option.

## Status

| Item | State |
|---|---|
| player pipe (RTP→Cedrus), front-end choice | ✅ libav/SDP |
| `citrusplay` live RTP→plane | ✅ validated end-to-end — live 1080p60 H.265 on the plane |
| OSD overlay (system: CPU/mem/temp) + run-forever daemon | ✅ v1 |

> The radio prerequisite (rtl8812au + wfb-ng bring-up) is documented separately and
> is **not** part of this repo.
