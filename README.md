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

## Status

Stages 0 (player pipe) and 1 (build radio stack) in progress; 2 (adapter) and
3 (live RF) pending hardware bring-up.
