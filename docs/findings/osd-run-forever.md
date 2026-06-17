# OSD overlay + run-forever player — end-to-end validation

Date: 2026-06-17. Board: Orange Pi Zero 2W (Allwinner **H618**), kernel
`6.18.35-current-sunxi64`, libdrm 2.4.124, libav 61.x, cc 14.2. Output: HDMI-A-1.
Branch: `feat/osd-run-forever`. Validated over SSH + `smoke/` synthetic RTP (no radio).

## Build / install
The README four-source command builds clean and installs:
```
cc player/wfbvid.c player/osd.c player/osd_render.c player/stats.c \
   -o /usr/local/bin/wfbvid $(pkg-config --cflags --libs libavformat libavcodec libavutil libdrm)
```
`-Wall` clean. Host unit tests (`player/tests/run-tests.sh`): all pass.

## Compositing topology (DRM atomic state, debugfs)
| Plane | Role | Format | Geometry | normalized-zpos |
|---|---|---|---|---|
| 33 | video | NV21 | 960×540 @ +800+450 (centred in 1440p) | 1 |
| 39 | primary (black) | XR24 | 2560×1440 fullscreen | 0 |
| 43 | **OSD** | **AR24 (ARGB8888)** | **296×104 @ +16+16** | **2** (top) |

`plane_mask=7` (all three on the HDMI CRTC). OSD box 296×104 matches
`osd_box_size(cols=18, lines=3, scale=2, pad=4)`. Ordering primary(0) < video(1)
< osd(2) exactly as designed.

## OSD
- **Visually confirmed on the HDMI display** (operator): translucent black box,
  top-left, three legible white lines `CPU .. %`, `MEM ../969 MB`, `TEMP .. C`,
  updating ~1 Hz with plausible values cross-checked against `free -m` and
  `/sys/class/thermal/*/temp`.
- Config knobs (verified via DRM state):
  - `WFBVID_OSD=0` → OSD plane dropped (`plane_mask=3`), video unaffected.
  - `WFBVID_OSD_SCALE=3` → box 440×152 (scale-3 math), still top-left zpos 2.
- Rendered in place at 1 Hz into a single ARGB dumb buffer on plane 43 (no
  per-frame atomic commit; the plane scans the same FB). Cost is negligible.

## Run-forever daemon
- **Cold start (no source):** screen up immediately (black primary + OSD),
  `startup mode 2560x1440`, `listening for H.265 RTP on udp:5600 (PT 97)`, no crash.
- **Lock:** `input open: codec hevc` → `Using V4L2 media driver cedrus` (HW
  decode) → `playing.`, video centred.
- **Signal loss (source killed):** player stays alive, last frame **frozen** on
  the video plane, OSD keeps updating. UDP drops produce no hard error, so the
  player takes the "keep socket, wait, refresh" path (periodic `AVERROR_EXIT`
  wakes); the close+reopen reconnect path is the fallback for genuine errors and
  was also exercised. Resumes on the next IDR (`RTP: missed N packets` = the
  source-restart sequence jump, handled).
- **`--port N`:** verified with a source on 5602 (`listening … udp:5602`, locked,
  HW-decoded, OSD present); default is 5600.
- **Clean exit:** SIGINT → `presented N frames` → console restored by `wfbplay`.

## Test-harness change
The player now uses a **built-in, sprop-less SDP** and expects the drone to send
VPS/SPS/PPS **in-band** (the documented "PPS id out of range … then locks"
behaviour). A plain `-c:v copy` loopback only emits param sets as SDP sprop, so
the player never locked. `smoke/rtp-loopback-src.sh` now defaults to `MODE=emulate`:
re-encode with `repeat-headers=1` (in-band param sets, PT 97), downscaled so the
**software** HEVC encoder keeps up (~5 fps @ 540p). This validates the player
pipeline/OSD/freeze; real 1080p60 throughput is the live-drone's job.

## Known limitations / latent risks (none triggerable on this hardware)
- **OSD attached in the startup modeset.** If `osd_create` *succeeds* but the
  atomic commit rejects the OSD plane params, `startup_modeset` would `DIE` even
  though video alone could run. The Task-5 probe pre-validates the plane
  (ARGB8888, mutable zpos, on this CRTC) and `osd_create` validates the buffer,
  so this is not triggerable here. If stricter isolation is ever wanted, attach
  the OSD in its own post-video commit and log-on-failure.
- **Same-resolution-across-reconnect** assumption: the video plane is sized/
  centred from the first frame and not re-committed if a later stream returns at
  a different resolution (fine for a fixed drone).
- **zpos / plane-selection edge cases:** equal zpos only if a plane reports
  `zpos max == 0`; OSD mis-selection only if enumeration order inverts *and* the
  ARGB plane also advertises NV12. Neither holds on this DE33 (zpos range [0..2];
  video plane 33 enumerated before OSD plane 43). Both degrade to "no OSD", never
  broken video.
