# orangepi-fpv-gs

Orange Pi Zero 2W (Allwinner **H618**) as a low-latency **wfb-ng FPV ground
station + hardware video player**. The sunxi analog of
[`PixelPilot_rk`](https://github.com/gilankpam/PixelPilot_rk): receive the
drone's H.265 over a wfb-ng radio link, hardware-decode on the Cedrus VPU, and
scan **direct-to-plane** (no GPU compositing) with the CPU idle.

```
drone (OpenIPC VTX) → H.265 RTP → wfb-ng TX → 8812au ))) RF ((( 8812au
  → wfb-ng RX → RTP udp:5600 → [depay → Cedrus decode → DRM overlay plane]
```

The decode+display back-end is proven in
[`h618-mainline-video`](../../h618-mainline-video) (`drmvid`). This repo adds the
radio ingress (`rtl8812au` + `wfb-ng` swfec) and the live RTP front-end.

See [`docs/superpowers/specs/`](docs/superpowers/specs/) for the design and the
staged bring-up plan; [`docs/findings/`](docs/findings/) for test results.

## Layout

- `smoke/` — Stage 0: synthetic-RTP player-pipe validation (no radio HW)
- `groundstation/` — `rtl8812au` + `wfb-ng` build / install / bring-up
- `player/` — the RTP-enabled player (derived from `drmvid`)
- `docs/` — specs + findings

## Player usage

```sh
# build (on the board)
cc player/wfbvid.c -o /usr/local/bin/wfbvid \
   $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)

# live: wfb_rx de-FECs the drone link -> RTP udp:5600 -> player
wfb_rx -K gs.key -c 127.0.0.1 -u 5600 -p <radio_port> <monitor_iface> &
wfbplay player/wfb-h265.sdp     # frees the console, plays, restores on exit
```

`wfbvid` takes an SDP (or `rtp://` URL), pulls the codec/params from it, ingests
the live RTP with a large UDP socket buffer + low-delay flags, HW-decodes on
Cedrus, and scans direct-to-plane — **no PTS pacing** (present on decode),
modeset **deferred to the first frame**, and tolerant of packet-loss decode
errors. Env knobs: `WFBVID_NV21` (default 1, DE33 chroma workaround), `WFBVID_BUFSIZE`,
`WFBVID_ENC`/`WFBVID_RANGE`.

## Status

| Stage | State |
|---|---|
| 0 — player pipe (RTP→Cedrus), front-end choice | ✅ done (libav/SDP) |
| 1 — build `rtl8812au` + `wfb-ng` swfec on the board | ✅ done |
| **player** — `wfbvid` live RTP→plane | ✅ built, validated end-to-end (418 frames from synthetic RTP) |
| 2 — 8812au monitor-mode bring-up | ✅ done (ch161, non-DFS) |
| 3 — live RF link (drone → `wfb_rx` → player) | ✅ **working** — live 1080p60 H.265 on the plane (colour tuning pending) |

**Start here:** [`docs/BRINGUP.md`](docs/BRINGUP.md) — the full start-to-working
story + reproduce recipe. One-command launch: `groundstation/run-gs.sh`.
