# Orange Pi Zero 2W (H618) as a wfb-ng FPV ground station + player

**Status:** approved (brainstorm 2026-06-17) — executing Stages 0+1
**Board:** Orange Pi Zero 2W (Allwinner H618), Armbian community `26.8.0-trunk`,
kernel `6.18.35-current-sunxi64` (mainline + our DE33 NV12-overlay patch 0099).

## Goal

Turn the H618 into a complete low-latency FPV **ground station**: receive the
drone's H.265 video over a wfb-ng radio link, hardware-decode it on the Cedrus
VPU, and scan it **direct-to-plane** with the CPU idle — the sunxi analog of
`PixelPilot_rk` (which does the same on Rockchip).

## End-to-end path

```
Drone (OpenIPC SSC338Q VTX)
  → H.265 encoder → RTP
  → wfb-ng TX (gilankpam/wfb-ng @ swfec, sliding-window FEC)
  → 8812au radio  ))) RF (((  8812au on H618
  → wfb-ng RX (de-FEC) → RTP on udp:5600  (wfb-ng maps RTP 1:1 to 802.11)
  → PLAYER: RTP depay → Cedrus HW decode → direct-to-plane overlay
```

The **player half is already proven** by `drmvid` (repo `h618-mainline-video`):
Cedrus `v4l2request` decode → DRM-PRIME NV12 → atomic commit to the DE33 overlay
plane, smooth/CPU-idle at 1080p, with the NV12→NV21 chroma-swap workaround.
This project adds the **radio ingress** (8812au + wfb-ng RX) and swaps the
player's file front-end for a live **RTP front-end**.

## Architecture — two halves

1. **Radio ingress** — out-of-tree `rtl8812au` (svpcom, monitor+injection) +
   `wfb-ng` (swfec) RX. Output: clean RTP on a local UDP port. The onboard
   UWE5622 `wlan0` stays as the SSH/management link; the 8812au does monitor.
2. **Player** — RTP front-end → existing `drmvid` decode+display back-end.
   Front-end choice (gst-depay vs libav-RTP) is decided by Stage 0 data.

## Player front-end — the open decision (settled by Stage 0)

- **A. GStreamer depay** (mirror PixelPilot): `udpsrc ! rtph265depay !
  h265parse config-interval=-1 ! appsink`; each access unit → `avcodec_send_packet`
  (Cedrus). Robust RTP/loss handling, SPS/PPS re-inserted per keyframe. +gst dep.
- **B. libavformat RTP/SDP**: `avformat_open_input("stream.sdp")` with
  `protocol_whitelist=file,udp,rtp` + low-latency flags. Zero new deps; the
  payload-type in the SDP must match the transmitter. Less battle-tested on loss.

Player changes vs `drmvid` regardless of A/B: drop PTS pacing (present on
decode), defer the modeset to the first decoded frame's size, codec as an arg,
`AV_CODEC_FLAG_LOW_DELAY`, minimal on-screen frame ring, carry the NV21 workaround.

## Stages

| # | Stage | Needs | Proves |
|---|---|---|---|
| 0 | Player pipe, **synthetic RTP** loopback → depay → Cedrus → display | nothing | RTP front-end couples to the proven decode/display back-end; picks A vs B |
| 1 | Build/stage `rtl8812au` `.ko` + `wfb-ng` swfec GS, keys | nothing to compile | the radio stack builds clean against our patched kernel |
| 2 | 8812au monitor-mode bring-up | adapter on H618 | RF front-end alive |
| 3 | Live RF link (drone → wfb-ng RX → player) | adapter + drone | full glass-to-glass |

Stages 0+1 are hardware-independent and run now. 2+3 are a live bring-up once
the adapter is plugged in and the drone is powered (adapter + drone confirmed on
hand). Build only needs kernel headers, not the adapter.

## Build approach

- **rtl8812au**: native on-board build against the **matching** `linux-headers`
  deb (resolve `P47b7` vs `P70fa` by build metadata; `modprobe` vermagic/CRC is
  the final check). Fallback: cross-build inside the Armbian framework.
- **wfb-ng (swfec)**: plain `make` for aarch64 (the user already cross-builds
  into `wfb-build/{aarch64,armv7}`); `install_gs.sh` for systemd/config; shared
  `wfb_keygen` key pair between drone TX and H618 RX.

## Validation criteria

- **Decode (0/3):** sustained real-time fps with **CPU idle** — CPU pegged ⇒
  software fallback ⇒ fail.
- **Display (0/3):** visibly smooth video, correct colour (NV21 workaround).
- **Radio (2/3):** 8812au enumerates as monitor-capable; wfb-ng RX reports RX
  packets / FEC stats; RTP appears on udp:5600.

## Risks / known gotchas

- Kernel-headers/vermagic match for the out-of-tree module (mitigated above).
- `kmssink` was fragile on this DE33 in earlier work → the *real* player uses the
  `drmvid` back-end, not `kmssink`; the all-gst path is only a zero-code Stage-0 probe.
- DE33 reads NV12 chroma U/V-swapped → keep the NV21 relabel workaround.
- Console/SSH display gotchas (carried from drmvid): stop `getty@tty1`, unbind
  `fbcon` vtcon, `fuser -k /dev/video0` between display runs.
- Onboard `wlan0` (SSH) must coexist with the 8812au in monitor mode.

## Out of scope (for now)

PixelPilot-parity extras — OSD, DVR recording, IDR back-channel, restream — are
deferred until the core link + player is solid.

## Repo layout

```
smoke/         Stage 0 scripts (synthetic RTP source + receiver tests)
groundstation/ Stage 1+ scripts (rtl8812au + wfb-ng build/install/bring-up)
player/        the RTP-enabled player (drmvid-derived)
docs/          specs + findings log
```
