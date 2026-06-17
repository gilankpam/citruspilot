# OSD plane-capability probe (H618 DE33)

Date: 2026-06-17. Tool: `player/tools/plane-probe.c`.
Board: Orange Pi Zero 2W (H618), kernel `6.18.35-current-sunxi64`, libdrm 2.4.124.
Output: HDMI-A-1 connected (`/dev/dri/card0`).

## Result

| Plane | Type | NV12 | Alpha | zpos (default) | zpos range |
|---|---|---|---|---|---|
| 33 | OVERLAY | yes | ARGB8888 / ABGR8888 | 0 | [0..2] mutable |
| 39 | PRIMARY | no  | ARGB8888 / ABGR8888 | 1 | [0..2] mutable |
| 43 | OVERLAY | no  | ARGB8888 / ABGR8888 | 2 | [0..2] mutable |

- Video (NV12) overlay plane id: **33** (first non-primary NV12 overlay).
- OSD candidate (ARGB8888 overlay, no NV12) plane id: **43** (distinct from video).
- Primary plane id: **39**.
- Alpha format for the OSD: **ARGB8888** (ABGR8888 also available).
- zpos: **mutable**, range **[0..2]** on every plane.

## Decision

- [x] A usable alpha plane exists above the video → proceed with the OSD overlay
      plane (Tasks 7–8). No fallback required.
- [x] zpos is mutable → assign **primary(0) < video(1) < osd(2)**:
  - primary (plane 39) → zpos 0 (set in `startup_modeset`)
  - video (plane 33)  → `video_zpos = osd_zpos_max - 1 = 1` (clamped ≤ vplane zmax = 2)
  - osd (plane 43)    → `osd_zpos_max = 2` (top)
- [ ] zpos immutable — N/A (it is mutable here).
- [ ] No alpha plane — N/A (plane 43 qualifies).

Note: the default ordering is video(0) < primary(1) < osd(2); the player overrides
primary to 0 and video to 1 so the black primary sits behind the video, with the
OSD on top. All three zpos values (0,1,2) are distinct and within range, so no
conflict.

## Raw output

```
plane 33  type=OVERLAY  possible_crtcs=0x1
    formats: AB15 AB30 AB12 AB24 AR15 AR30 AR12 AR24 BG16 BG24 BA30 BA15 BA12 BA24 BX24 RG16 RG24 RA30 RA12 RA15 RA24 RX24 XB24 XR24 NV12 NV21 YU12 YV12 YUYV UYVY
    alpha=ARGB8888 ABGR8888  nv12=yes
    zpos=0 range=[0..2] mutable
plane 39  type=PRIMARY  possible_crtcs=0x1
    formats: AB15 AB12 AB24 AR15 AR12 AR24 BG16 BG24 BA15 BA12 BA24 BX24 RG16 RG24 RA12 RA15 RA24 RX24 XB24 XR24
    alpha=ARGB8888 ABGR8888  nv12=no
    zpos=1 range=[0..2] mutable
plane 43  type=OVERLAY  possible_crtcs=0x1
    formats: AB15 AB12 AB24 AR15 AR12 AR24 BG16 BG24 BA15 BA12 BA24 BX24 RG16 RG24 RA12 RA15 RA24 RX24 XB24 XR24
    alpha=ARGB8888 ABGR8888  nv12=no
    zpos=2 range=[0..2] mutable
```
